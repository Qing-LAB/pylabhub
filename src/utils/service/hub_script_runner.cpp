/**
 * @file hub_script_runner.cpp
 * @brief HubScriptRunner — worker_main_() event-and-tick loop.
 *
 * See hub_script_runner.hpp for design rationale.  Phase 7 Commit C
 * ships the dispatch path; rich script-side bindings come in Phase 8.
 */

#include "hub_script_runner.hpp"

#include "utils/config/hub_config.hpp"     // host_.config() reads (worker only)
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/hub_state_json.hpp"   // channel_to_json / role_to_json / ... (Phase 6.2b)
#include "utils/logger.hpp"
#include "utils/loop_timing_policy.hpp"  // compute_next_deadline — DO NOT REINVENT
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"

#include <chrono>
#include <variant>                       // std::monostate sentinel for ConfigT
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// Construction / destruction
// ============================================================================

HubScriptRunner::HubScriptRunner(pylabhub::hub_host::HubHost  &host,
                                  std::unique_ptr<ScriptEngine> engine,
                                  std::atomic<bool>            *shutdown_flag)
    // Phase 7 Option E: HubScriptRunner does NOT own a HubConfig (HubHost
    // is the sole owner — single config per hub instance).  Pass
    // `std::monostate{}` for ConfigT — EngineHost is opaque to it and
    // its trait specialization for HubAPI returns empty uid (which we
    // override below via `set_uid` once host_ is wired).
    : HubScriptRunnerBase("hub",
                           std::monostate{},
                           std::move(engine),
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
// Public surface
// ============================================================================

InvokeResponse HubScriptRunner::eval(const std::string &code)
{
    // Gate on is_running() so direct callers (today: HubHost::eval_in_script;
    // tomorrow: anything that holds a runner pointer) get the same
    // NotFound shape post-shutdown that HubHost's wrapper provides.
    // Without this, a caller bypassing HubHost would forward into a
    // partially-torn-down engine after shutdown_() — the runner ptr
    // is intentionally left intact post-shutdown for diagnostics
    // (parallel to broker_/admin_), but the engine state is gone.
    if (!is_running())
        return {InvokeStatus::NotFound, {}};

    // engine() is the protected accessor on EngineHost — usable here
    // because HubScriptRunner derives from EngineHost<HubAPI>.  Forwards
    // verbatim; serialization w.r.t. the worker thread's invoke calls
    // is the caller's responsibility (see header docs).
    return engine().eval(code);
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

    // ─────────────────────────────────────────────────────────────────────
    // Mirrors the role-side `worker_main_()` phase ordering (see
    // producer/consumer/processor_role_host.cpp + role_host_lifecycle.hpp).
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
        const std::string entry_point =
            (sc.type == "lua") ? "init.lua" : "__init__.py";

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
            enqueue("channel_opened", ch.name, js::channel_to_json(ch));
        });
    state.subscribe_channel_status_changed(
        [enqueue](const pylabhub::hub::ChannelEntry &ch) {
            enqueue("channel_status_changed", ch.name, js::channel_to_json(ch));
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
        [enqueue](const std::string &band, const std::string &role_uid) {
            enqueue("band_left", band,
                    nlohmann::json{{"band", band}, {"role_uid", role_uid}});
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
    // After this, parent thread's startup_() returns; HubHost::startup()
    // checks is_running() and proceeds.
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
