/**
 * @file hub_script_runner.cpp
 * @brief HubScriptRunner — worker_main_() event-and-tick loop.
 *
 * See hub_script_runner.hpp for design rationale.  Phase 7 Commit C
 * ships the dispatch path; rich script-side bindings come in Phase 8.
 */

#include "hub_script_runner.hpp"

#include "utils/script_engine_factory.hpp"  // create_engine + PythonGilLease (Step 0)
#include "utils/config/hub_config.hpp"     // host_.config() reads (worker only)
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/hub_state_json.hpp"   // channel_to_json / role_to_json / ... (Phase 6.2b)
#include "utils/logger.hpp"
#include "utils/loop_timing_policy.hpp"  // compute_next_deadline — DO NOT REINVENT
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"

#include <chrono>
#include <optional>                      // EngineGlobalLockRelease guard
#include <variant>                       // std::monostate sentinel for ConfigT
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// Construction / destruction
// ============================================================================

HubScriptRunner::HubScriptRunner(pylabhub::hub_host::HubHost  &host,
                                  std::atomic<bool>            *shutdown_flag)
    // Phase 7 Option E: HubScriptRunner does NOT own a HubConfig (HubHost
    // is the sole owner — single config per hub instance).  Pass
    // `std::monostate{}` for ConfigT — EngineHost is opaque to it and
    // its trait specialization for HubAPI returns empty uid (which we
    // override below via `set_uid` once host_ is wired).
    //
    // Per HEP-CORE-0011 §"Engine Construction Lifecycle" (2026-05-07):
    // engine is NOT passed in — `worker_main_` Step 0 constructs it on
    // the worker thread via scripting::create_engine.
    : HubScriptRunnerBase("hub",
                           std::monostate{},
                           shutdown_flag)
    , host_(host)
{
    // Set the real uid from HubHost's config — must run before
    // `startup_()` (EngineHost reads uid_ when constructing ApiT).
    // Construction order guarantees this: HubScriptRunner is built by
    // HubHost::startup() and `startup_()` is called explicitly AFTER
    // construction completes, so the body runs first.
    set_uid(host_.config().identity().uid);
}

HubScriptRunner::~HubScriptRunner()
{
    // EngineHost contract: derived destructor MUST call shutdown_() as
    // its first statement so the worker thread is joined before
    // derived members destruct.  EngineHost::~EngineHost panics if
    // phase is still Running on entry, surfacing the bug instead of
    // silently use-after-freeing.
    shutdown_();
}

// ============================================================================
// worker_main_ — event-and-tick loop
// ============================================================================

void HubScriptRunner::worker_main_()
{
    using Clock = std::chrono::steady_clock;
    namespace js = pylabhub::hub;   // channel_to_json / role_to_json / etc.

    // Capture uid + log_tag once.  uid is stable for HubHost's lifetime;
    // log_tag matches the role-side short-form convention ("prod" /
    // "cons" / "proc" / now "hub") used by the engine init step + log
    // prefixes.
    const std::string  uid     = host_.config().identity().uid;
    const std::string  log_tag = "hub";

    LOGGER_INFO("[HubScriptRunner:{}] worker_main_ entered", uid);

    // ── GIL pickup (HEP-CORE-0011 §"Engine Construction Lifecycle",
    //    Option E final design).  PythonInterpreter is owned by main(),
    //    which released the GIL via a stored py::gil_scoped_release in
    //    pi_startup so workers can acquire it.  PythonGilLease's ctor
    //    acquires the GIL on THIS thread (no-op if Python isn't loaded
    //    — Lua/native deployments).  The lease is held for the entire
    //    worker_main_ lifetime, so all PythonEngine py::object
    //    operations below run under GIL on this thread.
    pylabhub::scripting::PythonGilLease gil_lease;

    // ── Step 0: construct engine on this worker thread.  GIL is held
    //    by the lease above iff Python is in play, so PythonEngine's
    //    py::object{py::none()} member-default-initializers run under
    //    GIL safely.  For script-disabled hubs (cfg.script().path
    //    empty), HubHost does not construct the runner at all, so we
    //    always have a valid script config here.
    set_engine_(scripting::create_engine(host_.config().script()));
    if (!has_engine())
    {
        LOGGER_ERROR("[HubScriptRunner:{}] scripting::create_engine returned "
                     "null for script.type='{}'",
                     uid, host_.config().script().type);
        ready_promise().set_value(false);
        return;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Mirrors the role-side `worker_main_()` phase ordering (see
    // producer/consumer/processor_role_host.cpp + the `do_role_teardown`
    // helper in `engine_host.{hpp,cpp}` — formerly role_host_lifecycle.{hpp,cpp},
    // renamed 2026-05-20 because the old name collided with the framework
    // Lifecycle module).
    // Hub omits the schema / per-role-infra / broker-register phases:
    //
    //   Role:                                 Hub:
    //   1. resolve schemas                    skipped (no slots/flexzones)
    //   2. setup infrastructure               skipped (broker is HubHost's,
    //                                           not ours)
    //   3. wire api refs                      step A below (set_host,
    //                                           set_engine — minimal)
    //   4. engine_lifecycle_startup           step B (initialize +
    //                                           load_script + build_api;
    //                                           inline 3 calls — no
    //                                           register_slot_type, no
    //                                           cross-validate)
    //   5. invoke_on_init                     step C
    //   6. set_running + start_ctrl_thread    step D (set_running) +
    //                                           step E (subscribe to
    //                                           HubState events — the
    //                                           hub analogue of broker
    //                                           registration)
    //   7. ready_promise.set_value(true)      step F
    //   8. run_data_loop<Ops>                 step G (event/tick loop —
    //                                           hub-specific, NOT
    //                                           run_data_loop because
    //                                           that's queue-driven)
    //   9-14. do_role_teardown                step H (invoke_on_stop +
    //                                           engine.finalize + drain
    //                                           — inline; no broker_comm
    //                                           to deregister)
    // ─────────────────────────────────────────────────────────────────────

    // ── A. Wire api refs ──────────────────────────────────────────────────
    //
    // `EngineHost<HubAPI>::startup_()` lazily-constructed api() on this
    // worker thread before invoking us.  Wire the host backref + engine
    // pointer NOW so dispatch_event/tick can reach into both once events
    // start flowing.
    api().set_host(host_);
    api().set_engine(engine());

    // ── B. Engine setup (initialize + load_script + build_api) ────────────
    //
    // 3 inline calls instead of `engine_lifecycle_startup` because hub
    // skips schema registration + cross-validation.  Each step has its
    // own error path: log, finalize what we've already initialized,
    // signal failure to the parent thread via ready_promise.set_value(false),
    // and return.  ready_promise MUST be set on every exit path (success
    // or failure) or the parent blocks forever in startup_().
    {
        const auto &sc = host_.config().script();
        const std::filesystem::path base_path =
            sc.path.empty() ? std::filesystem::current_path()
                            : std::filesystem::weakly_canonical(sc.path);
        const std::filesystem::path script_dir = base_path / "script" / sc.type;
        // Audit B12 follow-up (2026-05-21, native-engine code review):
        // The original B12 fix landed in the 3 role hosts but missed
        // hub_script_runner.cpp — hub-side native scripts would still
        // try to load `__init__.py` (Python entry) and fail.  Same
        // 3-way ternary as the role hosts.
        const std::string entry_point =
            (sc.type == "lua")    ? "init.lua"
          : (sc.type == "native") ? "plugin.so"
                                  : "__init__.py";

        if (!engine().initialize(log_tag, &core()))
        {
            LOGGER_ERROR("[HubScriptRunner:{}] engine.initialize() failed",
                         uid);
            ready_promise().set_value(false);
            return;
        }
        if (!engine().load_script(script_dir, entry_point, /*required_callback=*/""))
        {
            LOGGER_ERROR("[HubScriptRunner:{}] engine.load_script({}) failed",
                         uid, script_dir.string());
            engine().finalize();
            ready_promise().set_value(false);
            return;
        }
        if (!engine().build_api(api()))
        {
            LOGGER_ERROR("[HubScriptRunner:{}] engine.build_api(HubAPI) "
                         "failed — engine likely lacks hub-side build_api_ "
                         "override", uid);
            engine().finalize();
            ready_promise().set_value(false);
            return;
        }
    }

    // ── C. invoke_on_init ─────────────────────────────────────────────────
    //
    // Run the script's on_init(api) callback.  Mirrors role-side step 5
    // (`engine.invoke_on_init()` after `engine_lifecycle_startup`).  Runs
    // BEFORE event subscriptions so the script's initial state setup
    // completes before broker events start enqueueing — early events
    // arriving DURING on_init would be lost on the role side too (broker
    // hasn't registered yet); hub mirrors by subscribing post-init.
    engine().invoke_on_init();

    // ── D. set_running ────────────────────────────────────────────────────
    //
    // Gates the main loop's `is_running()` predicate.  Default is false;
    // bracketed with `set_running(false)` in the teardown below — same
    // convention every role host uses.  Caught by static review D1.6
    // (R1) — without this, the loop exits on iteration 1.
    core().set_running(true);

    // ── E. Subscribe to all 11 HubState events ────────────────────────────
    //
    // Hub-specific equivalent of role-side `start_ctrl_thread` (which
    // registers the role with the broker so the broker starts sending it
    // events).  Hub doesn't register; instead it subscribes to HubState's
    // change notifications directly.
    //
    // Each subscription captures the typed payload, serializes via the
    // shared hub_state_json serializers (single source of truth shared
    // with admin RPC / metrics), and pushes onto the cross-thread queue
    // inherited from RoleHostCore.  notify_incoming() wakes this worker
    // out of wait_for_incoming() promptly.
    //
    // Subscription is `const` on HubState (handler-list mutex is
    // `mutable`, separate from the data-state mutex — see hub_state.hpp
    // "Thread model" note), so we register through the const reference
    // returned by `host_.state()` directly.
    auto &state = host_.state();
    auto &core_ = core();

    auto enqueue = [&core_](std::string event,
                            std::string sender,
                            nlohmann::json details)
    {
        IncomingMessage m;
        m.event   = std::move(event);
        m.sender  = std::move(sender);
        m.details = std::move(details);
        core_.enqueue_message(std::move(m));
    };

    state.subscribe_channel_opened(
        [enqueue](const pylabhub::hub::ChannelEntry &ch) {
            // Eager presence creation (Phase 1) means the producer-presence
            // exists at REG_REQ time but has not seen its first heartbeat
            // yet — observable is `kRegistering` per HEP-CORE-0023 §2.2.
            enqueue("channel_opened", ch.name,
                    js::channel_to_json(ch, pylabhub::hub::ChannelObservable::kRegistering));
        });
    state.subscribe_channel_status_changed(
        [enqueue](const pylabhub::hub::ChannelEntry &ch,
                  pylabhub::hub::ChannelObservable obs) {
            enqueue("channel_status_changed", ch.name, js::channel_to_json(ch, obs));
        });
    state.subscribe_channel_closed(
        [enqueue](const std::string &name) {
            enqueue("channel_closed", name, nlohmann::json{{"name", name}});
        });
    state.subscribe_consumer_added(
        [enqueue](const std::string &channel,
                  const pylabhub::hub::ConsumerEntry &cons) {
            nlohmann::json j;
            j["channel"]  = channel;
            j["role_uid"] = cons.role_uid;
            j["role_name"] = cons.role_name;
            j["consumer_pid"] = cons.consumer_pid;
            enqueue("consumer_added", channel, std::move(j));
        });
    state.subscribe_consumer_removed(
        [enqueue](const std::string &channel, const std::string &role_uid) {
            enqueue("consumer_removed", channel,
                    nlohmann::json{{"channel", channel}, {"role_uid", role_uid}});
        });
    state.subscribe_role_registered(
        [enqueue](const pylabhub::hub::RoleEntry &r) {
            enqueue("role_registered", r.uid, js::role_to_json(r));
        });
    state.subscribe_role_disconnected(
        [enqueue](const std::string &uid) {
            enqueue("role_disconnected", uid, nlohmann::json{{"uid", uid}});
        });
    state.subscribe_band_joined(
        [enqueue](const std::string &band, const pylabhub::hub::BandMember &m) {
            nlohmann::json j;
            j["band"]      = band;
            j["role_uid"]  = m.role_uid;
            j["role_name"] = m.role_name;
            enqueue("band_joined", band, std::move(j));
        });
    state.subscribe_band_left(
        [enqueue](const std::string &band, const std::string &role_uid,
                  const std::string &reason) {
            enqueue("band_left", band,
                    nlohmann::json{{"band", band}, {"role_uid", role_uid},
                                    {"reason", reason}});
        });
    state.subscribe_peer_connected(
        [enqueue](const pylabhub::hub::PeerEntry &p) {
            enqueue("peer_connected", p.uid, js::peer_to_json(p));
        });
    state.subscribe_peer_disconnected(
        [enqueue](const std::string &hub_uid) {
            enqueue("peer_disconnected", hub_uid,
                    nlohmann::json{{"hub_uid", hub_uid}});
        });

    // ── F. Signal ready ───────────────────────────────────────────────────
    //
    // After set_value the parent thread's startup_() returns and
    // HubHost::startup() emits `[HubHost:...] startup complete`.  If
    // any log line here fires AFTER set_value, it races the parent's
    // startup-complete line for the LOGGER queue on independent
    // threads — same shape as the broker startup race fixed at
    // broker_service.cpp:1091 (2026-07-04, siblings of task #93 /
    // #242).  Discipline: emit our "ready" log line FIRST so it hits
    // the log queue before the parent is woken.
    //
    // Log format: convention 1 (task #238) — `[component] event=X
    // field='value'`.  Component `HubScriptRunner:{uid}` matches the
    // file's existing prefix style; `event=Ready` per the newer
    // NORMATIVE marker convention.
    LOGGER_INFO("[HubScriptRunner:{}] event=Ready", uid);
    ready_promise().set_value(true);

    // ── G. Event/tick loop ────────────────────────────────────────────────
    //
    // Hub-specific.  NOT `run_data_loop<Ops>` because that frame is
    // queue-driven (acquire / invoke_and_commit / inbox-drain) — wrong
    // shape for the hub's drain-events / dispatch / tick semantics.  The
    // shared timing primitive is `compute_next_deadline()` which both
    // frames use.
    //
    // Pacing per `cfg.timing()` uses the SHARED loop-timing facility
    // — `compute_next_deadline()` defined in
    // `src/include/utils/loop_timing_policy.hpp`.  This is the same
    // helper role-side `run_data_loop<Ops>` uses
    // (`src/utils/service/data_loop.hpp:160`).  DO NOT REINVENT — the
    // helper handles all three policies correctly:
    //   - MaxRate                   → returns time_point::max() (no deadline)
    //   - FixedRate                 → resets to cycle_start + period
    //   - FixedRateWithCompensation → advances from prev_deadline (catch-up)
    //
    // Hand-rolled `next = now + period` only implements FixedRate and
    // silently downgrades the catch-up variant — fixed in Phase 7
    // D1.6 (S2) after the static review.
    const auto timing      = host_.config().timing().timing_params();
    const auto policy      = timing.policy;
    const double period_us = static_cast<double>(timing.period_us);
    const bool is_max_rate = (policy == LoopTimingPolicy::MaxRate);

    // Cache GIL-release-during-wait flag.  See data_loop.hpp comment for
    // the same pattern.  Read once from the engine; the loop body never
    // reaches into the engine for this.  When false (default) the inner
    // optional<EngineGlobalLockRelease> stays empty and the call shape
    // is byte-identical to pre-flag behaviour.
    const bool release_lock_idle = engine().release_global_lock_during_wait();

    // Initialize the first deadline ONCE here rather than relying on
    // `compute_next_deadline`'s `prev_deadline == max` first-cycle
    // branch.  Cleaner because the post-tick advance below now has
    // exactly one job (advance from a known prior deadline), and the
    // `max` sentinel still flags MaxRate paths uniformly.
    auto deadline = is_max_rate
        ? Clock::time_point::max()
        : Clock::now() + std::chrono::microseconds{
              static_cast<int64_t>(period_us)};

    while (is_running() && !core().is_shutdown_requested())
    {
        const auto cycle_start = Clock::now();

        // Drain pending events first.
        auto events = core().drain_messages();
        for (auto &e : events)
            api().dispatch_event(e);

        // Drain pending cross-thread invoke_returning requests
        // (HEP-CORE-0033 §12.4 augmentation request transport).  Admin
        // and broker threads call `engine.invoke_returning(name, args)`
        // and block on a future; the engine queues the request onto its
        // pending list and signals `notify_incoming()`.  We pick up the
        // request here, the engine runs the callback on this worker
        // thread (sole owner of script state), and sets the caller's
        // future with the (possibly mutated) response.
        //
        // Engine-specific behaviour of this call:
        //   - PythonEngine: typically a no-op here.  PythonEngine's
        //     `execute_direct_` already drains pending at the END of
        //     every `dispatch_event` invoke (see python_engine.cpp:690),
        //     so by this point the queue is empty.  The call is
        //     idempotent — one mutex acquire + empty-queue check.
        //   - LuaEngine: REQUIRED.  Lua's `invoke()` does NOT auto-drain
        //     (no equivalent to PythonEngine's tail-drain pattern), so
        //     augmentation requests posted by admin/broker threads
        //     between worker iterations would otherwise sit unserviced
        //     until the next event arrived to drag them through.
        //   - NativeEngine: no-op (default base implementation).
        //
        // The call is here to give a single, engine-agnostic invariant
        // ("between events and tick the pending queue is empty") rather
        // than relying on per-engine implicit drain points.  The
        // marginal cost on Python is one mutex lock — acceptable for
        // the cross-engine uniformity it buys.
        engine().process_pending();

        // Tick gate (post-events).  Use `now()` rather than `cycle_start`
        // so a slow event handler that pushed past the deadline still
        // fires `on_tick` this iteration ("beyond timeout but called",
        // per the ratified contract).  MaxRate fires unconditionally;
        // timed policies fire whenever the deadline has been crossed.
        // First cycle for timed policies: `now < deadline` until one
        // full period has elapsed — script gets one period of warm-up
        // before the first tick.
        const auto now = Clock::now();
        if (is_max_rate ||
            (deadline != Clock::time_point::max() && now >= deadline))
        {
            api().dispatch_tick();
            // Advance the deadline ONLY when the tick fires.  Events
            // alone do not shift the on_tick schedule — `on_tick` fires
            // once per period deterministically, regardless of how
            // often events drove the loop in between.  For
            // FixedRateWithCompensation this also restores the
            // documented catch-up semantic: missed slots fire in tight
            // succession on resume because `compute_next_deadline`
            // walks the absolute grid forward by `period`.
            deadline = compute_next_deadline(policy, deadline,
                                              cycle_start, period_us);
        }

        // Wait for next event or until the deadline elapses.  MaxRate
        // uses 0 (non-blocking poll); timed policies wait the
        // remaining ms until `deadline`, capped at zero (already-due —
        // the next iteration runs back-to-back, used by FRWithComp
        // catch-up after a long stall).
        //
        // GIL-release wrap (HEP-CORE-0011 §"Engine Thread Affinity"
        // sub-section "Optional global-lock release during idle waits").
        // When release_lock_idle is true the GIL is released across
        // the wait so cooperative Python sub-threads (Flask /
        // asyncio / threading.Thread spawned from the hub script)
        // can run.  wait_for_incoming itself is a pure C++
        // condition-variable wait — no engine touch — so releasing
        // here is safe.  When false the optional stays empty: the
        // ctor/dtor are no-ops and the call shape matches the
        // pre-flag version exactly.
        //
        // The post-wait shutdown observation is made INSIDE the wrap
        // (atomic-flag read; no GIL needed) so that a shutdown
        // signal is observed even if the dtor's reacquire would
        // block on a stolen GIL.  The bounded-join in
        // EngineHost::shutdown_() (HEP-CORE-0011 §"ThreadManager" →
        // "Bounded Shutdown Join") catches the worst-case blocked
        // reacquire.
        bool shutdown_observed = false;
        {
            std::optional<scripting::EngineGlobalLockRelease> idle_release;
            if (release_lock_idle)
                idle_release.emplace();

            if (is_max_rate)
            {
                // Poll the queue; do not block.  on_tick already fired
                // above so the loop spins through both phases at maximum
                // rate as documented for LoopTimingPolicy::MaxRate.
                core().wait_for_incoming(0);
            }
            else
            {
                const auto remain_us = std::chrono::duration_cast<
                    std::chrono::microseconds>(deadline - Clock::now()).count();
                const int wait_ms = (remain_us > 0)
                    ? static_cast<int>(remain_us / 1000)
                    : 0;
                core().wait_for_incoming(wait_ms);
            }

            // Step B' — observe shutdown BEFORE the dtor reacquires
            // the GIL.  The hub has no per-cycle slot to clean up
            // (no cleanup_on_shutdown analog); the loop's outer
            // teardown (H0/H1/H3/H4 below) handles draining + on_stop.
            if (!is_running() || core().is_shutdown_requested())
                shutdown_observed = true;
        }

        if (shutdown_observed)
            break;
    }

    // ── H. Teardown ───────────────────────────────────────────────────────
    //
    // Mirrors role-side `do_role_teardown` (steps 9-14 of worker_main),
    // minus the broker-comm steps (hub has no per-role broker
    // registration to deregister from):
    //
    //   role step 9   stop_accepting               — n/a (hub has no
    //                                                  cross-thread script
    //                                                  invocation guard)
    //   role step 9a  api.deregister_from_broker   — n/a (hub IS the
    //                                                  broker host)
    //   role step 10  engine.invoke_on_stop        — H1 below
    //   role step 11  engine.finalize              — H3 below
    //   role step 12  set_running(false) + notify  — H4 below
    //   role step 13  teardown_infrastructure      — n/a (hub has none)
    //   role step 14  thread_manager().drain       — handled by
    //                 EngineHost::shutdown_ via api_.reset() (drops
    //                 ApiT → drops ThreadManager → drains threads)
    //
    // S1 — user-facing shutdown notice.  EngineHost::shutdown_() races
    // can leave the worker blocked in wait_for_incoming for up to one
    // tick period (default 1 s) before observing the shutdown signal.
    // Surfacing this expectation up-front avoids "is the hub stuck?"
    // confusion during operator-driven shutdown.
    LOGGER_INFO("[HubScriptRunner:{}] shutting down — draining pending "
                "events and running on_stop; cleanup may take up to one "
                "tick period ({} ms)",
                uid,
                is_max_rate ? 0 : static_cast<int>(period_us / 1000));

    // H0. Drain whatever events arrived between the loop's last
    // wait_for_incoming and the shutdown signal — no harm; each event
    // is a typed dispatch the script can ignore if it doesn't care.
    auto remaining = core().drain_messages();
    for (auto &e : remaining)
        api().dispatch_event(e);

    // H1. Script's on_stop callback — flush dashboards, write final
    // metrics, etc.  Mirrors role step 10.
    engine().invoke_on_stop();

    // H3. Engine finalize — releases the engine's per-host resources
    // (Lua state, Python interpreter sub-objects, etc.).  finalize() is
    // idempotent.  Mirrors role step 11.
    engine().finalize();

    // H4. Flip `running_threads_` back to false; any subsequent
    // `is_running()` query returns false.  Mirrors role step 12 (minus
    // broker_comm.stop / notify_incoming — handled by EngineHost::
    // shutdown_'s core_.notify_incoming).
    core().set_running(false);

    LOGGER_INFO("[HubScriptRunner:{}] worker_main_ exiting", uid);
}

} // namespace pylabhub::scripting
