/**
 * @file hub_host.cpp
 * @brief HubHost — start/stop owner for the hub binary
 *        (HEP-CORE-0033 §4).  Not a `LifecycleGuard` module.
 */
#include "utils/hub_host.hpp"

#include "utils/admin_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_state.hpp"
#include "utils/logger.hpp"
#include "utils/thread_manager.hpp"
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
    explicit Impl(config::HubConfig c)
        : cfg(std::move(c))
    {}

    config::HubConfig                          cfg;
    hub::HubState                              state;        // owned by HubHost
    std::unique_ptr<broker::BrokerService>     broker;       // built in startup()
    std::unique_ptr<admin::AdminService>       admin_svc;    // built in startup() if admin.enabled
    std::unique_ptr<utils::ThreadManager>      thread_mgr;   // built in startup()

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
        Constructed,  // initial; startup() never succeeded
        Running,      // startup() succeeded; cleanup pending
        ShutDown,     // shutdown() called; terminal — single-use
    };
    std::atomic<Phase>                         phase{Phase::Constructed};

    // Wake-up signal for run_main_loop().  Distinct from `phase`
    // because run_main_loop wakes on `request_shutdown()` (which
    // sets the flag) BEFORE phase transitions to ShutDown (which
    // happens later in `shutdown()`).
    std::atomic<bool>                          shutdown_flag{false};

    // Bound endpoint + pubkey, populated when the broker fires
    // `on_ready` (post-bind, pre-poll-loop).  startup() blocks on the
    // ready future so the accessors below are populated by the time
    // it returns.  Survive shutdown for diagnostics.
    std::string                                bound_endpoint;
    std::string                                bound_pubkey;

    // Wake-up coordination for run_main_loop().
    std::mutex                                 wake_mu;
    std::condition_variable                    wake_cv;
};

// ============================================================================
// Special members
// ============================================================================

HubHost::HubHost(config::HubConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg)))
{
}

HubHost::~HubHost()
{
    // Idempotent shutdown if the caller didn't run it explicitly.
    if (impl_ &&
        impl_->phase.load(std::memory_order_acquire) ==
            Impl::Phase::Running)
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
    if (!impl_->phase.compare_exchange_strong(
            expected, Impl::Phase::Running,
            std::memory_order_acq_rel))
    {
        if (expected == Impl::Phase::Running)
            return; // idempotent: already running
        // expected == Impl::Phase::ShutDown
        throw std::logic_error(
            "HubHost::startup() called after shutdown() — this host "
            "instance is single-use.  Construct a new HubHost instance "
            "to start fresh.");
    }

    // Build BrokerService::Config from HubConfig.
    broker::BrokerService::Config bcfg;
    bcfg.endpoint = impl_->cfg.network().broker_endpoint;
    // CURVE: enabled if a keypair was loaded into the auth config
    // (caller did `cfg.load_keypair(password)` before constructing HubHost).
    bcfg.use_curve         = !impl_->cfg.auth().client_pubkey.empty();
    bcfg.server_secret_key = impl_->cfg.auth().client_seckey;
    bcfg.server_public_key = impl_->cfg.auth().client_pubkey;

    // Heartbeat-multiplier timeouts (HEP-CORE-0023 §2.5; HEP-0033 §6.4).
    const auto &hb = impl_->cfg.broker();
    bcfg.heartbeat_interval =
        std::chrono::milliseconds(hb.heartbeat_interval_ms);
    bcfg.ready_miss_heartbeats   = hb.ready_miss_heartbeats;
    bcfg.pending_miss_heartbeats = hb.pending_miss_heartbeats;
    bcfg.grace_heartbeats        = hb.grace_heartbeats;
    if (hb.ready_timeout_ms.has_value())
        bcfg.ready_timeout_override =
            std::chrono::milliseconds(*hb.ready_timeout_ms);
    if (hb.pending_timeout_ms.has_value())
        bcfg.pending_timeout_override =
            std::chrono::milliseconds(*hb.pending_timeout_ms);
    if (hb.grace_ms.has_value())
        bcfg.grace_override =
            std::chrono::milliseconds(*hb.grace_ms);

    // Federation peers (HEP-CORE-0022; HEP-0033 §6.4).
    bcfg.self_hub_uid = impl_->cfg.identity().uid;
    for (const auto &p : impl_->cfg.federation().peers)
    {
        broker::FederationPeer fp;
        fp.hub_uid         = p.uid;
        fp.broker_endpoint = p.endpoint;
        fp.pubkey_z85      = p.pubkey;
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
    auto ready_promise = std::make_shared<
        std::promise<std::pair<std::string, std::string>>>();
    auto ready_future  = ready_promise->get_future();
    bcfg.on_ready = [ready_promise](const std::string &ep,
                                     const std::string &pk)
    {
        ready_promise->set_value({ep, pk});
    };

    // Cleanup helper used by every failure branch below.  Must be
    // safe regardless of how far startup() got.  Restores the phase
    // to Constructed so the caller can retry after fixing whatever
    // caused the failure (e.g., port collision).
    auto rollback_partial_startup = [this]() noexcept {
        try
        {
            if (impl_->admin_svc) impl_->admin_svc->stop();
            if (impl_->broker) impl_->broker->stop();
            if (impl_->thread_mgr) impl_->thread_mgr->drain();
        }
        catch (...) { /* swallow — already in error path */ }
        impl_->admin_svc.reset();
        impl_->broker.reset();
        impl_->thread_mgr.reset();
        impl_->bound_endpoint.clear();
        impl_->bound_pubkey.clear();
        impl_->shutdown_flag.store(false, std::memory_order_release);
        impl_->phase.store(Impl::Phase::Constructed,
                            std::memory_order_release);
    };

    try
    {
        // Construct broker bound to our HubState (HEP-0033 §4 ownership).
        impl_->broker = std::make_unique<broker::BrokerService>(
            std::move(bcfg), impl_->state);

        // ThreadManager auto-registers as a dynamic lifecycle module
        // "ThreadManager:HubHost:<uid>".  Owns the broker thread (and,
        // in Phase 6.2, the admin thread) for bounded-join shutdown.
        impl_->thread_mgr = std::make_unique<utils::ThreadManager>(
            "HubHost", impl_->cfg.identity().uid);

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
                                               auto broker_exc =
                                                   std::current_exception();
                                               try
                                               {
                                                   ready_promise->set_exception(
                                                       broker_exc);
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
                                                       LOGGER_ERROR(
                                                           "[HubHost:{}] broker thread "
                                                           "died after on_ready: {}",
                                                           uid_for_log, e.what());
                                                   }
                                                   catch (...)
                                                   {
                                                       LOGGER_ERROR(
                                                           "[HubHost:{}] broker thread "
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
        const auto status =
            ready_future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::timeout)
        {
            throw std::runtime_error(
                "HubHost::startup: broker did not signal ready within 5s "
                "(broker thread is hung — neither bound nor exited)");
        }
        auto info = ready_future.get(); // rethrows broker bind exception fast
        impl_->bound_endpoint = info.first;
        impl_->bound_pubkey   = info.second;

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
                hub::get_zmq_context(),
                impl_->cfg.admin(),
                impl_->cfg.admin().admin_token,
                *this);

            auto *admin_ptr = impl_->admin_svc.get();
            if (!impl_->thread_mgr->spawn("admin",
                                           [admin_ptr] { admin_ptr->run(); }))
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
    }
    catch (...)
    {
        rollback_partial_startup();
        throw;
    }

    // Phase was already set to Running by the CAS at the top of this
    // function.  Reaching here means the broker is bound and ready.
    LOGGER_INFO("[HubHost:{}] startup complete (broker on {})",
                impl_->cfg.identity().uid, impl_->bound_endpoint);
}

void HubHost::run_main_loop()
{
    std::unique_lock<std::mutex> lk(impl_->wake_mu);
    impl_->wake_cv.wait(lk, [this] {
        return impl_->shutdown_flag.load(std::memory_order_acquire);
    });
    LOGGER_INFO("[HubHost:{}] main loop woke for shutdown",
                impl_->cfg.identity().uid);
}

void HubHost::shutdown()
{
    // Phase FSM: Running → ShutDown.  CAS makes the transition
    // race-free; only the first effective caller does the cleanup
    // work.  Other concurrent callers (or repeated calls after
    // ShutDown) early-return without touching state.
    Impl::Phase expected = Impl::Phase::Running;
    if (!impl_->phase.compare_exchange_strong(
            expected, Impl::Phase::ShutDown,
            std::memory_order_acq_rel))
    {
        // Either Constructed (never started — nothing to stop) or
        // already ShutDown (idempotent).  Either way, no-op.
        return;
    }

    LOGGER_INFO("[HubHost:{}] shutdown initiated",
                impl_->cfg.identity().uid);

    // Wake any thread blocked in run_main_loop().
    impl_->shutdown_flag.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(impl_->wake_mu);
    }
    impl_->wake_cv.notify_all();

    // Stop AdminService FIRST (HEP-CORE-0033 §4.2 step 3): closing
    // the REP socket blocks new RPCs while in-flight handlers that
    // already entered the broker complete normally.  Order matters:
    // if we stopped the broker first, an in-flight admin RPC could
    // observe a half-torn-down broker mid-call.
    if (impl_->admin_svc)
        impl_->admin_svc->stop();

    // Break the broker's poll loop.  Internal atomic flip + ZMQ
    // wake-up; broker thread exits run() shortly after.
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

    LOGGER_INFO("[HubHost:{}] shutdown complete",
                impl_->cfg.identity().uid);
}

void HubHost::request_shutdown() noexcept
{
    // Async equivalent of `shutdown()` minus the synchronous drain:
    //   1. flip the shutdown flag (run_main_loop's wait predicate)
    //   2. break the broker's poll loop
    //   3. wake run_main_loop
    // After this returns, the broker has begun stopping; the broker
    // thread will exit shortly.  ThreadManager join happens later
    // (in `shutdown()` if the caller drives it; otherwise in
    // `~HubHost`).  Safe from any thread (signal handler, admin RPC
    // dispatcher, broker error path).
    impl_->shutdown_flag.store(true, std::memory_order_release);
    if (impl_->admin_svc)
        impl_->admin_svc->stop();   // already noexcept
    if (impl_->broker)
    {
        try { impl_->broker->stop(); }
        catch (...) { /* noexcept contract — swallow */ }
    }
    {
        std::lock_guard<std::mutex> lk(impl_->wake_mu);
    }
    impl_->wake_cv.notify_all();
}

bool HubHost::is_running() const noexcept
{
    return impl_->phase.load(std::memory_order_acquire) ==
           Impl::Phase::Running;
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

} // namespace pylabhub::hub_host
