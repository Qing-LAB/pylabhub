/**
 * @file hub_host.cpp
 * @brief HubHost — start/stop owner for the hub binary
 *        (HEP-CORE-0033 §4).  Not a `LifecycleGuard` module.
 */
#include "utils/hub_host.hpp"

#include "utils/security/key_store.hpp"

#include "hub_script_runner.hpp" // private header — HubScriptRunner ctor
#include "utils/admin_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_state.hpp"
#include "utils/logger.hpp"
#include "utils/script_engine.hpp"        // ScriptEngine + InvokeResponse
#include "utils/security/known_roles.hpp" // HEP-CORE-0035 §4.8 + Phase B
#include "utils/thread_manager.hpp"
#include "utils/timeout_constants.hpp" // kAdminPollIntervalMs
#include "utils/zmq_context.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>

namespace pylabhub::hub_host
{

// ============================================================================
// Impl
// ============================================================================

struct HubHost::Impl
{
    explicit Impl(config::HubConfig c) : cfg(std::move(c))
    {
        // Wire the operator-console output-buffer caps from config (§11.0.4)
        // before any thread can append — cfg and state are both live here.
        const auto &ob = cfg.admin().output_buffer;
        state.set_console_buffer_caps(ob.max_lines, ob.max_bytes, ob.max_line_bytes);
    }

    config::HubConfig cfg;
    hub::HubState state;                              // owned by HubHost
    std::unique_ptr<broker::BrokerService> broker;    // built in startup()
    std::unique_ptr<admin::AdminService> admin_svc;   // built in startup() if admin.enabled
    std::unique_ptr<utils::ThreadManager> thread_mgr; // built in startup()

    // Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
    // engine_pre_startup REMOVED.  HubHost no longer holds the engine.
    // The runner constructs it on its worker thread in
    // `worker_main_` Step 0 via scripting::create_engine.  Script-
    // enabled vs script-disabled mode is selected by cfg.script().path.

    // Hub-side script runtime — built in startup() if engine is set
    // and `cfg.script().path` is non-empty (Phase 7 §4.1 step 10).
    // Stopped FIRST in shutdown (§4.2 step 2 — before admin and
    // broker) so any in-flight script callbacks complete before
    // subsystems they may depend on tear down.
    std::unique_ptr<scripting::HubScriptRunner> runner;

    /// Start/stop run-state FSM — monotonic transitions
    /// `Constructed → Running → ShutDown`.  CAS-based transitions in
    /// `startup()` / `shutdown()` make both race-free; the contract
    /// still expects a single-driver caller, but accidental
    /// concurrent calls resolve cleanly.  See HEP-CORE-0033 §4.1/§4.2
    /// for the ordered protocol steps these transitions enforce.
    ///
    /// Note: this is NOT related to `LifecycleGuard` — HubHost is not
    /// a `LifecycleGuard` module (its `LifecycleModule::Init/Shutdown`
    /// hooks are not used).  This `phase` field tracks the host's own
    /// start/stop state; the `LifecycleGuard` system manages
    /// independent process-wide modules (Logger, ZMQContext, …) and is
    /// installed by the binary `main` BEFORE constructing HubHost.
    ///
    /// Member is named `phase` (not `state`) to avoid shadowing the
    /// @c HubState value member directly above — `state` is the
    /// protocol/data state owned by the hub, while `phase` is the
    /// run-state of this host instance.
    enum class Phase : uint8_t
    {
        Constructed, // initial; startup() never succeeded
        Running,     // startup() succeeded; cleanup pending
        ShutDown,    // shutdown() called; terminal — single-use
    };
    std::atomic<Phase> phase{Phase::Constructed};

    // Wake-up signal for run_main_loop().  Distinct from `phase`
    // because run_main_loop wakes on `request_shutdown()` (which
    // sets the flag) BEFORE phase transitions to ShutDown (which
    // happens later in `shutdown()`).
    std::atomic<bool> shutdown_flag{false};

    // Bound endpoint + pubkey, populated when the broker fires
    // `on_ready` (post-bind, pre-poll-loop).  startup() blocks on the
    // ready future so the accessors below are populated by the time
    // it returns.  Survive shutdown for diagnostics.
    std::string bound_endpoint;
    std::string bound_pubkey;

    // Wake-up coordination for run_main_loop().
    std::mutex wake_mu;
    std::condition_variable wake_cv;
};

// ============================================================================
// Special members
// ============================================================================

HubHost::HubHost(config::HubConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg)))
{
    // Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
    // engine is no longer injected.  HubScriptRunner constructs it on
    // its worker thread in worker_main_ Step 0.  Script-enabled vs
    // script-disabled mode is selected by impl_->cfg.script().path.
}

HubHost::~HubHost()
{
    // Idempotent shutdown if the caller didn't run it explicitly.
    if (impl_ && impl_->phase.load(std::memory_order_acquire) == Impl::Phase::Running)
    {
        try
        {
            shutdown();
        }
        catch (...)
        {
            // Destructor must not throw — swallow; ThreadManager dtor
            // logs detached threads if any.
        }
    }
    // impl_ destructor runs:
    //   1. thread_mgr_ dtor → drains tracked threads (bounded join)
    //   2. broker_ dtor (broker already stopped above)
    //   3. state dtor
    //   4. cfg dtor
}

// ============================================================================
// Start / stop  (NOT a LifecycleGuard module — see Impl::phase comment)
// ============================================================================

void HubHost::startup()
{
    // Phase FSM: Constructed → Running.  CAS rejects re-entry from
    // Running (idempotent no-op) and from ShutDown (single-use after
    // shutdown — construct a new HubHost).
    Impl::Phase expected = Impl::Phase::Constructed;
    if (!impl_->phase.compare_exchange_strong(expected, Impl::Phase::Running,
                                              std::memory_order_acq_rel))
    {
        if (expected == Impl::Phase::Running)
            return; // idempotent: already running
        // expected == Impl::Phase::ShutDown
        throw std::logic_error("HubHost::startup() called after shutdown() — this host "
                               "instance is single-use.  Construct a new HubHost instance "
                               "to start fresh.");
    }

    // Build BrokerService::Config from HubConfig.
    // HEP-CORE-0033 §4.2 step 2 + HEP-CORE-0035 §2: CURVE is
    // unconditional, so a populated keypair is a startup precondition.
    // Production reaches this point via `HubConfig::load_keypair`
    // (decrypts the HubVault) which populates the HEP-CORE-0040
    // KeyStore entry `"hub_identity"`.  Tests call it through the
    // same vault path.  Absence here means the caller skipped both —
    // a programmer error per HEP-0035 §4.6.5 (no-bypass discipline).
    if (!pylabhub::utils::security::sodium_ready() ||
        !pylabhub::utils::security::secure().keys().has(
            pylabhub::utils::security::kHubIdentityName))
    {
        impl_->phase.store(Impl::Phase::Constructed, std::memory_order_release);
        throw std::logic_error("HubHost::startup: KeyStore entry 'hub_identity' is absent — "
                               "load the vault keypair via `HubConfig::load_keypair(password)` "
                               "before calling startup() (HEP-CORE-0035 §2; HEP-CORE-0040 §171).");
    }
    broker::BrokerService::Config bcfg;
    bcfg.endpoint = impl_->cfg.network().broker_endpoint;
    // HEP-CORE-0040 §172: BrokerService bind site now calls
    // `secure().keys().with_seckey("hub_identity", ...)` + `pubkey(...)`
    // directly — no keypair field in `bcfg`.

    // Hub-global schema records live at `<hub_dir>/schemas/` per
    // HEP-CORE-0034 §12 — "the only filesystem-authoritative source".
    // The broker's `load_hub_globals_()` walks `cfg.schema_search_dirs`
    // at startup; production HubHost wires the canonical location here
    // so operators only need to drop schema JSON files into the hub
    // directory's `schemas/` subdir.  When the subdir is absent or
    // empty, the broker logs `registered 0 hub-global schema record(s)`
    // and continues — no error.
    {
        std::filesystem::path schemas_dir = impl_->cfg.base_dir() / "schemas";
        bcfg.schema_search_dirs.push_back(schemas_dir.string());
    }

    // HEP-CORE-0035 §4.8 + PeerAdmission Phase B — the operator-managed
    // known_roles allowlist lives INSIDE the encrypted hub vault
    // (extracted by HubConfig::load_keypair, which also enforces the
    // §4.8.7 hard cutover against a stale plaintext sidecar).  Feed the
    // broker's Layer-1 ZAP allowlist from it; empty = §4.8.4 deny-all
    // bootstrap (every REG_REQ rejected until the operator runs
    // `--add-known-role`).
    for (const auto &kr : impl_->cfg.known_roles())
        bcfg.known_roles.push_back(kr);

    // Heartbeat-multiplier timeouts (HEP-CORE-0023 §2.5; HEP-0033 §6.4).
    const auto &hb = impl_->cfg.broker();
    bcfg.heartbeat_interval = std::chrono::milliseconds(hb.heartbeat_interval_ms);
    bcfg.ready_miss_heartbeats = hb.ready_miss_heartbeats;
    bcfg.pending_miss_heartbeats = hb.pending_miss_heartbeats;
    if (hb.ready_timeout_ms.has_value())
        bcfg.ready_timeout_override = std::chrono::milliseconds(*hb.ready_timeout_ms);
    if (hb.pending_timeout_ms.has_value())
        bcfg.pending_timeout_override = std::chrono::milliseconds(*hb.pending_timeout_ms);

    // Checksum-repair policy (HEP-CORE-0007 §12.4 +
    // `BrokerService::Config::checksum_repair_policy`).  Translate the
    // HubConfig string to the production enum.
    if (hb.checksum_repair_policy == "notify_only")
        bcfg.checksum_repair_policy = broker::ChecksumRepairPolicy::NotifyOnly;
    else
        bcfg.checksum_repair_policy = broker::ChecksumRepairPolicy::None;

    // Federation peers (HEP-CORE-0022; HEP-0033 §6.4).
    bcfg.self_hub_uid = impl_->cfg.identity().uid;
    for (const auto &p : impl_->cfg.federation().peers)
    {
        broker::FederationPeer fp;
        fp.hub_uid = p.uid;
        fp.broker_endpoint = p.endpoint;
        fp.pubkey_z85 = p.pubkey;
        // p.channels is currently absent in HubFederationConfig — see
        // HEP-CORE-0035; falls back to an empty relay list (no relay).
        bcfg.peers.push_back(std::move(fp));
    }

    // Wire on_ready so startup() can block until the broker has bound
    // and capture the actual endpoint / pubkey (broker_endpoint may be
    // an ephemeral port like `tcp://127.0.0.1:0`).  on_ready is
    // contracted to fire exactly once per BrokerService instance; if
    // it fires twice, set_value will throw `future_error` which we
    // surface as a startup failure (catches a real broker bug).
    auto ready_promise = std::make_shared<std::promise<std::pair<std::string, std::string>>>();
    auto ready_future = ready_promise->get_future();
    bcfg.on_ready = [ready_promise](const std::string &ep, const std::string &pk)
    { ready_promise->set_value({ep, pk}); };

    // Cleanup helper used by every failure branch below.  Must be
    // safe regardless of how far startup() got.  Restores the phase
    // to Constructed so the caller can retry after fixing whatever
    // caused the failure (e.g., port collision).
    auto rollback_partial_startup = [this]() noexcept
    {
        try
        {
            // Runner first (HEP-CORE-0033 §4.2 step 2 ordering — script
            // shuts down before admin/broker so any in-flight callbacks
            // complete).  shutdown_() is idempotent.
            if (impl_->runner)
                impl_->runner->shutdown_();
            if (impl_->admin_svc)
                impl_->admin_svc->stop();
            if (impl_->broker)
                impl_->broker->stop();
            if (impl_->thread_mgr)
                impl_->thread_mgr->drain();
        }
        catch (...)
        { /* swallow — already in error path */
        }
        impl_->runner.reset();
        impl_->admin_svc.reset();
        impl_->broker.reset();
        impl_->thread_mgr.reset();
        impl_->bound_endpoint.clear();
        impl_->bound_pubkey.clear();
        impl_->shutdown_flag.store(false, std::memory_order_release);
        impl_->phase.store(Impl::Phase::Constructed, std::memory_order_release);
    };

    try
    {
        // Construct broker bound to our HubState (HEP-0033 §4 ownership).
        impl_->broker = std::make_unique<broker::BrokerService>(std::move(bcfg), impl_->state);

        // ThreadManager auto-registers as a dynamic lifecycle module
        // "ThreadManager:HubHost:<uid>".  Owns the broker thread (and,
        // in Phase 6.2, the admin thread) for bounded-join shutdown.
        impl_->thread_mgr =
            std::make_unique<utils::ThreadManager>("HubHost", impl_->cfg.identity().uid);

        // Spawn broker thread.  Wrap the run() call in a try/catch
        // that forwards any exception to `ready_promise` so the
        // parent thread fails fast (~ms) instead of waiting out the
        // 5s ready timeout when bind fails (busy port, etc.).  Without
        // this, the thread dies, no on_ready fires, and the parent
        // observes only a 5s wait_for timeout — a real-bug-shaped
        // delay that masquerades as a wait.  Worth the boilerplate.
        //
        // Two outcomes from inside the lambda:
        //   1. broker.run() returns normally → on_ready already fired
        //      from inside; promise was set there; this path is a
        //      regular thread exit (e.g. after stop()).
        //   2. broker.run() throws (bind failure most commonly) →
        //      promise wasn't set yet; we set it to the exception so
        //      ready_future.get() rethrows on the parent thread.
        // If the broker fired on_ready successfully (case 1) and then
        // threw later, the promise is already satisfied — `set_exception`
        // would throw `future_error::promise_already_satisfied`; we
        // swallow that since the failure is post-startup and the broker
        // poll loop will have stopped on its own.
        auto *broker_ptr = impl_->broker.get();
        const std::string uid_for_log = impl_->cfg.identity().uid;
        if (!impl_->thread_mgr->spawn("broker",
                                      [broker_ptr, ready_promise, uid_for_log]
                                      {
                                          try
                                          {
                                              broker_ptr->run();
                                          }
                                          catch (...)
                                          {
                                              // Capture before any other
                                              // exception machinery rebinds
                                              // current_exception().
                                              auto broker_exc = std::current_exception();
                                              try
                                              {
                                                  ready_promise->set_exception(broker_exc);
                                              }
                                              catch (const std::future_error &)
                                              {
                                                  // Promise already satisfied
                                                  // — broker fired on_ready
                                                  // successfully then died
                                                  // mid-run.  Log the
                                                  // ORIGINAL broker exception
                                                  // so the failure isn't
                                                  // silently swallowed; the
                                                  // host's `request_shutdown`
                                                  // path is responsible for
                                                  // tearing down on detection.
                                                  try
                                                  {
                                                      std::rethrow_exception(broker_exc);
                                                  }
                                                  catch (const std::exception &e)
                                                  {
                                                      LOGGER_ERROR("[HubHost:{}] broker thread "
                                                                   "died after on_ready: {}",
                                                                   uid_for_log, e.what());
                                                  }
                                                  catch (...)
                                                  {
                                                      LOGGER_ERROR("[HubHost:{}] broker thread "
                                                                   "died after on_ready "
                                                                   "(non-std exception)",
                                                                   uid_for_log);
                                                  }
                                              }
                                          }
                                      }))
        {
            throw std::runtime_error(
                "HubHost::startup: ThreadManager refused to spawn broker thread");
        }

        // Block until the broker fires on_ready (success) or the
        // worker forwards its exception via the promise (failure).
        // A healthy broker binds in <100 ms; the 5 s deadline now only
        // matters for genuinely hung threads (no exception, no on_ready
        // — should not happen in practice, but kept as a safety net).
        const auto status = ready_future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::timeout)
        {
            throw std::runtime_error("HubHost::startup: broker did not signal ready within 5s "
                                     "(broker thread is hung — neither bound nor exited)");
        }
        auto info = ready_future.get(); // rethrows broker bind exception fast
        impl_->bound_endpoint = info.first;
        impl_->bound_pubkey = info.second;

        // ── Phase 6.2 — AdminService (HEP-CORE-0033 §4.1 step 9) ───────────
        //
        // Built only when `admin.enabled=true`.  AdminService takes the
        // shared process-wide ZMQ context (caller installs `ZMQContext`
        // via the LifecycleGuard before constructing HubHost) and the
        // admin token populated by `HubConfig::load_keypair()` from the
        // unlocked vault.  Spawned on the same ThreadManager as the
        // broker for unified bounded-join shutdown.
        if (impl_->cfg.admin().enabled)
        {
            impl_->admin_svc = std::make_unique<admin::AdminService>(
                hub::get_zmq_context(), impl_->cfg.admin(), impl_->cfg.admin().admin_token, *this);

            auto *admin_ptr = impl_->admin_svc.get();
            if (!impl_->thread_mgr->spawn("admin", [admin_ptr] { admin_ptr->run(); }))
            {
                throw std::runtime_error(
                    "HubHost::startup: ThreadManager refused to spawn admin thread");
            }
            // No ready handshake: AdminService binds at the head of
            // run().  If the bind throws, the worker thread terminates
            // and the next ThreadManager join surfaces it.  For
            // ephemeral-port test endpoints, the actual bound endpoint
            // is queryable via `host.admin()->bound_endpoint()` once
            // the worker has progressed past bind (Phase 6.2a tests
            // poll for non-empty endpoint before connecting).
        }

        // ── Phase 7 — HubScriptRunner (HEP-CORE-0033 §4.1 step 10) ─────────
        //
        // Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
        // script-enabled vs script-disabled mode is now selected solely
        // by `cfg.script().runs_script_engine()` — the single explicit "run a
        // script?" signal: false when `type:"none"` (pure broker) OR the path
        // is empty (legacy script-disabled hub).  Engine construction has moved
        // off main() / HubHost into the runner's worker_main_ Step 0.  No
        // engine_pre_startup injection any more.
        const bool runs_script = impl_->cfg.script().runs_script_engine();
        if (runs_script)
        {
            // Construct + start the runner.  Per Option E, runner
            // reads HubConfig fields through its host_ backref —
            // HubHost stays the sole owner of HubConfig.  The engine
            // is constructed on the runner's worker thread in
            // worker_main_ Step 0 via scripting::create_engine.
            impl_->runner =
                std::make_unique<scripting::HubScriptRunner>(*this, &impl_->shutdown_flag);

            // startup_() spawns the worker thread + blocks on its
            // ready_promise.  On worker startup failure (script load
            // error, etc.), startup_() returns having internally run
            // shutdown_() — runner is in ShutDown phase.  We treat
            // that as a hard startup failure (HubHost cannot meet
            // its contract: an engine-enabled hub has no live script
            // runtime).  The rollback path resets the runner and
            // surfaces a runtime_error to the caller.
            impl_->runner->startup_();
            if (!impl_->runner->is_running())
            {
                throw std::runtime_error("HubHost::startup: script runner failed to start "
                                         "(see preceding logs from HubScriptRunner / engine "
                                         "for the underlying cause — typically a script "
                                         "syntax error or missing callback).");
            }
        }
    }
    catch (...)
    {
        rollback_partial_startup();
        throw;
    }

    // Phase was already set to Running by the CAS at the top of this
    // function.  Reaching here means the broker is bound and ready.
    LOGGER_INFO("[HubHost:{}] startup complete (broker on {})", impl_->cfg.identity().uid,
                impl_->bound_endpoint);
}

void HubHost::run_main_loop()
{
    // Mirror the role-side `run_role_main_loop` pattern (see
    // `role_main_helpers.hpp` + `RoleHostCore::wait_for_incoming`):
    // timed wait + flag check at the LOOP TOP, NOT predicate-inside-
    // wait.  This makes the shutdown atomic the sole source of truth
    // (read outside any mutex at the top of each iteration); the CV
    // is just an optimization to skip useless polling work between
    // ticks.  Lost-wake (a `notify_all` that arrives before the
    // waiter is fully registered with the CV) costs at most one tick
    // of latency — never permanent block.  This eliminates the
    // empty-body lock_guard handshake in `request_shutdown` and
    // keeps the request_shutdown implementation in lockstep with
    // `RoleHostCore::notify_incoming` (just `cv.notify_all()`).
    while (!impl_->shutdown_flag.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lk(impl_->wake_mu);
        impl_->wake_cv.wait_for(lk, std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));
    }
    LOGGER_INFO("[HubHost:{}] main loop woke for shutdown", impl_->cfg.identity().uid);
}

void HubHost::shutdown()
{
    // Phase FSM: Running → ShutDown.  CAS makes the transition
    // race-free; only the first effective caller does the cleanup
    // work.  Other concurrent callers (or repeated calls after
    // ShutDown) early-return without touching state.
    Impl::Phase expected = Impl::Phase::Running;
    if (!impl_->phase.compare_exchange_strong(expected, Impl::Phase::ShutDown,
                                              std::memory_order_acq_rel))
    {
        // Either Constructed (never started — nothing to stop) or
        // already ShutDown (idempotent).  Either way, no-op.
        return;
    }

    LOGGER_INFO("[HubHost:{}] shutdown initiated", impl_->cfg.identity().uid);

    // Wake any thread blocked in run_main_loop().
    impl_->shutdown_flag.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(impl_->wake_mu);
    }
    impl_->wake_cv.notify_all();

    // ── Drain admin FIRST (HEP-CORE-0033 §4.2 step 2 — Phase 8c
    //    revision) ────────────────────────────────────────────────────
    //
    // Pre-Phase-8c the order was runner → admin → broker, justified by
    // the script-side `on_stop` needing both broker and admin alive.
    // Phase 8c added `HubAPI::augment_*` hooks invoked by AdminService
    // handlers; admin in-flight RPCs can therefore reach into the
    // script-side HubAPI mid-call.  If we tore the runner down first
    // (which destroys HubAPI as part of the EngineHost teardown), an
    // admin RPC parked between recv'ing the request and finishing the
    // augment hook would deref a destroyed HubAPI → UB.
    //
    // Fix: signal admin (atomic flip) and synchronously join its
    // thread BEFORE runner shutdown.  AdminService::stop is non-
    // blocking by design (atomic only); the bounded-join lives at the
    // ThreadManager via the new `join_named("admin")` primitive.
    // After the join returns, no admin thread is alive → no path
    // reaches into HubAPI from outside the worker thread → runner
    // destruction is safe.
    //
    // Admin's `on_stop`-equivalent observability invariant is
    // preserved: scripts run their `on_stop` against a live broker,
    // and a live HubAPI (since runner.shutdown_ runs AFTER admin is
    // joined but BEFORE HubAPI is destroyed inside that call).  Admin
    // RPCs are simply unavailable during on_stop — same as in any
    // shutdown-driven scenario.
    if (impl_->admin_svc)
    {
        impl_->admin_svc->stop(); // atomic flag flip
        if (impl_->thread_mgr)
            (void)impl_->thread_mgr->join_named("admin"); // bounded join
    }

    // ── Runner shutdown ─────────────────────────────────────────────
    // Drains pending events, runs `on_stop` (broker still alive for
    // `api.broadcast_channel`), finalizes the engine (cancels any
    // pending augment futures with EngineShutdown — currently none
    // since admin is already drained), and destroys HubAPI.
    if (impl_->runner)
        impl_->runner->shutdown_();

    // ── Broker stop ─────────────────────────────────────────────────
    // Async signal as before; the broker thread is joined by the
    // thread_mgr drain below.  (Future HEP §13 work: `HUB_TARGETED_ACK`
    // wires `on_peer_message_augment`, at which point broker also
    // needs a synchronous `stop_inbound + join_named` step BEFORE
    // runner shutdown — symmetric to admin above.)
    if (impl_->broker)
        impl_->broker->stop();

    // Synchronously drain the ThreadManager so the broker thread has
    // actually exited by the time shutdown() returns.  This is the
    // strong contract: after shutdown(), no protocol traffic is being
    // processed and the broker will reject new connections.  If we
    // only flipped the flags (delegating join to ~HubHost), in-flight
    // requests could still be serviced between shutdown() and dtor.
    if (impl_->thread_mgr)
    {
        const auto detached = impl_->thread_mgr->drain();
        if (detached > 0)
        {
            LOGGER_WARN("[HubHost:{}] shutdown: {} thread(s) exceeded "
                        "bounded-join timeout and were detached",
                        impl_->cfg.identity().uid, detached);
        }
    }

    LOGGER_INFO("[HubHost:{}] shutdown complete", impl_->cfg.identity().uid);
}

void HubHost::request_shutdown() noexcept
{
    // DUMB async signal — exactly mirrors `RoleHostCore::notify_incoming`:
    //   flag store + cv.notify_all().  Nothing else.
    //
    // Pre-D2.3 this method actively called `admin_svc->stop()` and
    // `broker->stop()` directly — that broke the HEP-CORE-0033 §4.2
    // ordering on the async path because admin/broker began winding
    // down before the runner had a chance to drain its dispatch
    // queue, leaving a window where a script callback (Phase 8 will
    // let scripts call `host.broker().*`) could observe a half-
    // stopped broker.  The role-side pattern shows the right shape:
    // only the main thread orchestrates teardown, exclusively in
    // shutdown(), in the order §4.2 defines (runner → admin →
    // broker → drain).
    //
    // No lock_guard handshake needed: `run_main_loop` uses
    // `wait_for(timeout)` + flag-checked-at-loop-top, so any lost-
    // wake recovers within one tick.  See `run_main_loop` above.
    impl_->shutdown_flag.store(true, std::memory_order_release);
    impl_->wake_cv.notify_all();
}

bool HubHost::is_running() const noexcept
{
    return impl_->phase.load(std::memory_order_acquire) == Impl::Phase::Running;
}

// ============================================================================
// Accessors
// ============================================================================

const config::HubConfig &HubHost::config() const noexcept
{
    return impl_->cfg;
}

broker::BrokerService &HubHost::broker() noexcept
{
    return *impl_->broker; // UB if called outside startup..shutdown window
}

const hub::HubState &HubHost::state() const noexcept
{
    return impl_->state;
}

bool HubHost::nonce_seen(std::string_view identity, std::string_view client_nonce,
                         std::uint64_t window_ms) noexcept
{
    // Forward to the one hub-wide dedup store on the owned HubState —
    // the same store the broker mutates through its HubState& (§4).  The
    // guard owns its trusted clock; no timestamp crosses this boundary
    // (see ReplayGuard header).
    return impl_->state.nonce_seen(identity, client_nonce, window_ms);
}

// Operator console output buffer (HEP-CORE-0033 §11.0.1 layer 6 / §11.0.4) —
// forward to the one buffer on the owned HubState (self-locked; §4).
void HubHost::append_console_line(std::string request_id, nlohmann::json content)
{
    impl_->state.append_console_line(std::move(request_id), std::move(content));
}

nlohmann::json HubHost::drain_console_output()
{
    return impl_->state.drain_console_output();
}

void HubHost::console_on_connect()
{
    impl_->state.console_on_connect();
}

void HubHost::console_on_disconnect()
{
    impl_->state.console_on_disconnect();
}

const std::string &HubHost::broker_endpoint() const noexcept
{
    return impl_->bound_endpoint;
}

const std::string &HubHost::broker_pubkey() const noexcept
{
    return impl_->bound_pubkey;
}

admin::AdminService *HubHost::admin() noexcept
{
    return impl_->admin_svc.get();
}

HubAPI *HubHost::hub_api() noexcept
{
    // Runner is the EngineHost<HubAPI> — its ApiT instance is built
    // lazily inside startup_() on the worker thread.  Before startup
    // (or if scripts are disabled) runner is null; before the worker
    // thread completes its setup phase the api isn't built.  Both
    // cases return nullptr — caller skips augmentation.
    if (!impl_->runner)
        return nullptr;
    return impl_->runner->hub_api_ptr();
}

} // namespace pylabhub::hub_host
