/**
 * @file hub_script_runner.cpp
 * @brief HubScriptRunner — worker_main_() event-and-tick loop.
 *
 * See hub_script_runner.hpp for design rationale.  Phase 7 Commit C
 * ships the dispatch path; rich script-side bindings come in Phase 8.
 */

#include "hub_script_runner.hpp"

#include "utils/config/hub_config.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/hub_state_json.hpp"   // channel_to_json / role_to_json / ... (Phase 6.2b)
#include "utils/logger.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"

#include <chrono>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// Construction / destruction
// ============================================================================

HubScriptRunner::HubScriptRunner(pylabhub::hub_host::HubHost  &host,
                                  config::HubConfig             cfg,
                                  std::unique_ptr<ScriptEngine> engine,
                                  std::atomic<bool>            *shutdown_flag)
    : HubScriptRunnerBase("hub", std::move(cfg), std::move(engine), shutdown_flag)
    , host_(host)
{
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
    using clock = std::chrono::steady_clock;
    namespace js = pylabhub::hub;   // channel_to_json / role_to_json / etc.

    LOGGER_INFO("[HubScriptRunner:{}] worker_main_ entered",
                config().identity().uid);

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
    // Pacing per `cfg.timing()` — same LoopTimingPolicy enum + same
    // params as role data loops.  MaxRate (period_us == 0) means
    // wait_for_incoming(0) → returns immediately, on_tick fires every
    // iteration at maximum rate.  FixedRate (period_us > 0) means
    // wait_for_incoming(period_ms) → blocks until an event arrives or
    // the period elapses, on_tick fires once per period.
    const auto timing = config().timing().timing_params();
    const int  tick_period_ms =
        (timing.period_us == 0)
            ? 0  // MaxRate: poll-only, on_tick fires every iteration
            : static_cast<int>(timing.period_us / 1000);

    auto next_tick = clock::now() +
        std::chrono::microseconds(timing.period_us);

    while (is_running() && !core().is_shutdown_requested())
    {
        // Drain any pending events first; if the queue is empty, block
        // until something arrives or the tick deadline fires.
        auto events = core().drain_messages();
        for (auto &e : events)
            api().dispatch_event(e);

        const auto now = clock::now();
        if (timing.period_us > 0 && now >= next_tick)
        {
            api().dispatch_tick();
            // Reset deadline per LoopTimingPolicy::FixedRate semantics —
            // on overrun, deadline resets to `now + period` (no catch-
            // up).  FixedRateWithCompensation (catch-up) variant would
            // advance from `next_tick` instead; defer until a real use
            // case demands it.
            next_tick = now +
                std::chrono::microseconds(timing.period_us);
        }
        else if (timing.period_us == 0)
        {
            // MaxRate: fire on_tick every iteration with no sleep.
            api().dispatch_tick();
        }

        // Compute how long to wait for the NEXT event arrival before
        // either the next tick deadline or shutdown wakes us.
        if (timing.period_us > 0)
        {
            const auto remain_us = std::chrono::duration_cast<
                std::chrono::microseconds>(next_tick - clock::now()).count();
            const int wait_ms = (remain_us > 0)
                ? static_cast<int>(remain_us / 1000)
                : 0;
            core().wait_for_incoming(wait_ms);
        }
        else
        {
            // MaxRate: do not block; spin between drain + tick.
            // wait_for_incoming(0) returns immediately if nothing pending.
            core().wait_for_incoming(0);
        }
    }

    // ── Shutdown — drain remaining events, run on_stop ─────────────────────
    //
    // Mirrors role-side data-loop teardown: drain whatever's left in
    // the queue (no harm; each event is a typed dispatch that the
    // script can ignore if it doesn't care), then invoke on_stop so
    // the script can flush dashboards / write final metrics.
    auto remaining = core().drain_messages();
    for (auto &e : remaining)
        api().dispatch_event(e);
    (void) engine().invoke("on_stop");

    LOGGER_INFO("[HubScriptRunner:{}] worker_main_ exiting",
                config().identity().uid);
    (void) tick_period_ms;  // silence -Wunused if compiler optimises it out
}

} // namespace pylabhub::scripting
