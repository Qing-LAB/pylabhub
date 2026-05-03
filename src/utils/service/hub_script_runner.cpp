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
// worker_main_ — event-and-tick loop
// ============================================================================

void HubScriptRunner::worker_main_()
{
    using Clock = std::chrono::steady_clock;
    namespace js = pylabhub::hub;   // channel_to_json / role_to_json / etc.

    LOGGER_INFO("[HubScriptRunner:{}] worker_main_ entered",
                host_.config().identity().uid);

    // ── Mark runner as running (gates `is_running()` queries) ─────────────
    //
    // RoleHostCore's `is_running()` (which `EngineHost::is_running()`
    // delegates to) reads `running_threads_`.  Default value is FALSE
    // until the worker explicitly sets it true — same convention every
    // role host uses (see producer_role_host.cpp / consumer_role_host.cpp /
    // processor_role_host.cpp `set_running(true/false)` bracketing).
    //
    // Without this, the loop condition `is_running()` returns false on
    // the first iteration → loop exits immediately → runner does no
    // work despite "starting cleanly".  Caught by static review during
    // Phase 7 D1.6 (R1).
    core().set_running(true);

    // ── Wire HubAPI to outer host + engine ─────────────────────────────────
    //
    // EngineHost<HubAPI>::startup_() lazily-constructed `api()` on this
    // worker thread before invoking us.  We complete the wiring (host
    // backref + engine pointer) here so dispatch_event/tick on api()
    // can reach back into HubHost / ScriptEngine.  Both are set BEFORE
    // we start subscribing to broker events to avoid an early-arrival
    // dispatch that would no-op silently.
    api().set_host(host_);
    api().set_engine(engine());

    // ── Subscribe to all 11 HubState events ────────────────────────────────
    //
    // Each subscription captures the typed payload, serializes via the
    // shared hub_state_json serializers (single source of truth shared
    // with admin RPC / metrics), and pushes onto the cross-thread
    // queue inherited from RoleHostCore.  notify_incoming() wakes this
    // worker out of wait_for_incoming() promptly.
    //
    // HubState's subscribe_* methods register handlers that fire on the
    // broker thread (the broker is the sole HubState mutator per HEP-
    // 0033 G2).  Capturing `&core_ = this->core()` keeps the lambdas
    // small and avoids holding `this` (no chance of accidentally
    // dereferencing a dangling HubScriptRunner if ever the lifetime
    // contract were violated — handlers see only the queue).
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

    // const_cast OK: HubHost::state() returns const&, but subscribe_* is
    // a non-const op (mutates handler list).  HubState's own header
    // documents that subscription is thread-safe and uses an internal
    // lock independent of the read-only state-snapshot lock.
    auto &state_mut = const_cast<pylabhub::hub::HubState &>(state);

    state_mut.subscribe_channel_opened(
        [enqueue](const pylabhub::hub::ChannelEntry &ch) {
            enqueue("channel_opened", ch.name, js::channel_to_json(ch));
        });
    state_mut.subscribe_channel_status_changed(
        [enqueue](const pylabhub::hub::ChannelEntry &ch) {
            enqueue("channel_status_changed", ch.name, js::channel_to_json(ch));
        });
    state_mut.subscribe_channel_closed(
        [enqueue](const std::string &name) {
            enqueue("channel_closed", name, nlohmann::json{{"name", name}});
        });
    state_mut.subscribe_consumer_added(
        [enqueue](const std::string &channel,
                  const pylabhub::hub::ConsumerEntry &cons) {
            nlohmann::json j;
            j["channel"]  = channel;
            j["role_uid"] = cons.role_uid;
            j["role_name"] = cons.role_name;
            j["consumer_pid"] = cons.consumer_pid;
            enqueue("consumer_added", channel, std::move(j));
        });
    state_mut.subscribe_consumer_removed(
        [enqueue](const std::string &channel, const std::string &role_uid) {
            enqueue("consumer_removed", channel,
                    nlohmann::json{{"channel", channel}, {"role_uid", role_uid}});
        });
    state_mut.subscribe_role_registered(
        [enqueue](const pylabhub::hub::RoleEntry &r) {
            enqueue("role_registered", r.uid, js::role_to_json(r));
        });
    state_mut.subscribe_role_disconnected(
        [enqueue](const std::string &uid) {
            enqueue("role_disconnected", uid, nlohmann::json{{"uid", uid}});
        });
    state_mut.subscribe_band_joined(
        [enqueue](const std::string &band, const pylabhub::hub::BandMember &m) {
            nlohmann::json j;
            j["band"]      = band;
            j["role_uid"]  = m.role_uid;
            j["role_name"] = m.role_name;
            enqueue("band_joined", band, std::move(j));
        });
    state_mut.subscribe_band_left(
        [enqueue](const std::string &band, const std::string &role_uid) {
            enqueue("band_left", band,
                    nlohmann::json{{"band", band}, {"role_uid", role_uid}});
        });
    state_mut.subscribe_peer_connected(
        [enqueue](const pylabhub::hub::PeerEntry &p) {
            enqueue("peer_connected", p.uid, js::peer_to_json(p));
        });
    state_mut.subscribe_peer_disconnected(
        [enqueue](const std::string &hub_uid) {
            enqueue("peer_disconnected", hub_uid,
                    nlohmann::json{{"hub_uid", hub_uid}});
        });

    // Signal the parent thread (still inside startup_()) that we're
    // ready.  After this, startup_() returns; the script's `on_init`
    // would normally run before the loop — we delegate to the engine's
    // generic `invoke("on_start")` for the hub-side equivalent.
    (void) engine().invoke("on_start");
    ready_promise().set_value(true);

    // ── Event-and-tick loop ────────────────────────────────────────────────
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

    auto deadline = Clock::time_point::max();   // first cycle has no deadline

    while (is_running() && !core().is_shutdown_requested())
    {
        const auto cycle_start = Clock::now();

        // Drain pending events first.
        auto events = core().drain_messages();
        for (auto &e : events)
            api().dispatch_event(e);

        // Tick gate.  MaxRate fires every iteration unconditionally;
        // timed policies fire when the cycle has reached/passed the
        // current deadline.  First cycle (deadline == max) only fires
        // for MaxRate — timed policies wait one full period before
        // their first tick.
        if (is_max_rate ||
            (deadline != Clock::time_point::max() &&
             cycle_start >= deadline))
        {
            api().dispatch_tick();
        }

        // Advance the deadline using the shared canonical helper.
        deadline = compute_next_deadline(policy, deadline,
                                          cycle_start, period_us);

        // Wait for next event or until the deadline elapses.  MaxRate
        // uses 0 (non-blocking poll); timed policies wait the
        // remaining ms until `deadline`, capped at zero (already-due).
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

    // ── Shutdown — drain remaining events, run on_stop ─────────────────────
    //
    // Mirrors role-side data-loop teardown: drain whatever's left in
    // the queue (no harm; each event is a typed dispatch that the
    // script can ignore if it doesn't care), then invoke on_stop so
    // the script can flush dashboards / write final metrics.
    //
    // S1 — user-facing shutdown notice.  EngineHost::shutdown_() races
    // can leave the worker blocked in wait_for_incoming for up to one
    // tick period (default 1 s) before observing the shutdown signal.
    // Surfacing this expectation up-front avoids "is the hub stuck?"
    // confusion during operator-driven shutdown.
    LOGGER_INFO("[HubScriptRunner:{}] shutting down — draining pending "
                "events and running on_stop; cleanup may take up to one "
                "tick period ({} ms)",
                host_.config().identity().uid,
                is_max_rate ? 0 : static_cast<int>(period_us / 1000));

    auto remaining = core().drain_messages();
    for (auto &e : remaining)
        api().dispatch_event(e);
    (void) engine().invoke("on_stop");

    // Mirror the role-side bracketing — flips `running_threads_` back
    // to false so any subsequent `is_running()` query returns false.
    core().set_running(false);

    LOGGER_INFO("[HubScriptRunner:{}] worker_main_ exiting",
                host_.config().identity().uid);
}

} // namespace pylabhub::scripting
