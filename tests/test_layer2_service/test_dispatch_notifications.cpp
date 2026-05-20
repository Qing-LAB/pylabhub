/**
 * @file test_dispatch_notifications.cpp
 * @brief L2 unit tests for `pylabhub::scripting::dispatch_notifications`
 *        — the per-cycle table-driven dispatcher that routes broker
 *        notifications carried in `IncomingMessage::notification_id`
 *        to their dedicated script callback (or leaves them in `msgs`
 *        for the script's generic scan).
 *
 * Covered notifications (per `NotificationId` in role_host_core.hpp):
 *   - CHANNEL_CLOSING_NOTIFY → on_channel_closing(channel, reason, api)
 *   - CONSUMER_DIED_NOTIFY   → on_consumer_died(channel, consumer_uid,
 *                                               reason, api)
 *     reason ∈ {"heartbeat_timeout", "process_dead"} per HEP-CORE-0023 §2.1.1.
 *
 * Dispatcher contract (HEP-CORE-0011 callback table):
 *   1. If the script does NOT define the callback → `msgs` is unchanged
 *      for that entry; engine invoke method is NOT called.
 *   2. If defined → every matching notify is removed from `msgs`
 *      (single-delivery), and the engine invoke method is called once
 *      per entry with values extracted from `details`.
 *   3. Entries whose `notification_id` is `Unknown` (unrecognised wire
 *      type) stay in `msgs` in original order.
 *
 * IncomingMessage wiring contract (the test mirrors what
 * `role_api_base.cpp`'s BRC `on_notification` lambda does):
 *   - `event` field carries the wire type string (kept for debug /
 *     script-side generic scan).
 *   - `notification_id` is set via `parse_notification_id(event)` at
 *     the BRC enqueue boundary; dispatcher reads this field, not the
 *     string.  Test message builders below set both.
 */

#include "binary_lifecycle.h"
#include "service/cycle_ops.hpp"   // dispatch_notifications
#include "utils/logger.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::InvokeInbox;
using pylabhub::scripting::InvokeResponse;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeStatus;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::NotificationId;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::ScriptEngine;
using pylabhub::scripting::parse_notification_id;

// dispatch_notifications calls into engine.has_callback() +
// engine.invoke_on_*().  Engine surface uses LOGGER_* on error paths.
// Single-suite binary, Logger-only guard (Pattern 1+).
PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule()
)

namespace
{

/// Minimal recording engine: implements `has_callback` and
/// `invoke_on_channel_closing` enough for the dispatch helper to
/// drive.  All other virtual methods are no-op stubs.  Calls to
/// `invoke_on_channel_closing` are recorded in `calls` as
/// (channel, reason) pairs in the order they were invoked.
///
/// ─── CONSTRAINED EXCEPTION to the no-mocks rule ──────────────────
///
/// pylabhub's testing discipline (`feedback_no_mocks_via_observability`
/// / `feedback_test_layering_and_no_mocks`) says: **use real classes
/// + extend their accessors for observability; don't write parallel
/// implementations**.  This file violates that rule deliberately and
/// in a specifically scoped way.  Read this BEFORE copying the
/// pattern elsewhere.
///
/// **Why it's used here:** the dispatcher (`dispatch_notifications`
/// in `service/cycle_ops.hpp`) is a pure C++ function whose entire
/// contract with `ScriptEngine` is a narrow subset: `has_callback`,
/// `invoke_on_channel_closing`, `invoke_on_consumer_died`,
/// `invoke_on_hub_dead`.  Branch coverage of every dispatch path
/// (callback-defined × callback-absent × notification-type) is the
/// L2-level test goal.  Doing that with a real `LuaEngine` or
/// `PythonEngine` means spinning up the interpreter + loading a
/// script for each TEST_F — a 10–100× cost increase for tests whose
/// job is to pin pure-C++ table-driven dispatch logic.
///
/// **What this exception costs:** every time `ScriptEngine` gains a
/// new pure virtual, this class plus two siblings
/// (`MockEngine` in `test_hub_api.cpp`, the stub in
/// `workers/role_data_loop_workers.cpp`) must be updated by hand.
/// Drift is a real risk.
///
/// **What still validates real-engine correctness:** the real
/// `PythonEngine` / `LuaEngine` / `NativeEngine` implementations
/// have their own end-to-end tests with real scripts in
/// `test_python_engine.cpp` / `test_lua_engine.cpp` /
/// `test_native_engine.cpp`.  Those tests pin the real engines'
/// `has_callback` cache + every `invoke_on_*` actually invoking the
/// script.  Composition: real-engine tests verify the engine's side
/// of the contract; THIS file's `RecordingEngine` tests verify the
/// dispatcher's side.
///
/// **DO NOT copy this pattern** for tests that exercise role-host
/// loops, broker interactions, lifecycle modules, or any
/// cross-cutting integration.  Those MUST use real production
/// classes + observable side effects.  The narrow scope of
/// `RecordingEngine` (branch coverage of a pure-C++ dispatch
/// function with a 4-method interface) is what makes the exception
/// defensible here.  If you're tempted to add the 5th method or to
/// override `invoke_produce` / `invoke_consume` here for testing,
/// **stop** — that's the boundary where you should reach for a
/// real engine instead.
///
/// Related: HEP-CORE-0011 §"Notification dispatch" describes the
/// dispatcher contract being tested.
class RecordingEngine : public ScriptEngine
{
  public:
    /// When true, has_callback("on_channel_closing") returns true.
    bool has_on_channel_closing{false};
    /// When true, has_callback("on_consumer_died") returns true.
    bool has_on_consumer_died{false};
    /// When true, has_callback("on_hub_dead") returns true.
    bool has_on_hub_dead{false};
    // S4 expansion 2026-05-19 — typed band callbacks.
    bool has_on_band_member_joined{false};
    bool has_on_band_member_left{false};
    bool has_on_band_message{false};
    bool has_on_band_lost{false};

    /// Recorded on_channel_closing (channel, reason) pairs.
    std::vector<std::pair<std::string, std::string>> calls;

    /// Recorded on_consumer_died (channel, consumer_uid, reason) tuples.
    std::vector<std::tuple<std::string, std::string, std::string>>
        consumer_died_calls;

    /// Recorded on_hub_dead (source_hub_uid) invocations.
    std::vector<std::string> hub_dead_calls;

    /// Recorded band-callback invocations.
    std::vector<std::tuple<std::string, std::string, std::string>>
        band_member_joined_calls;
    std::vector<std::tuple<std::string, std::string, std::string>>
        band_member_left_calls;
    std::vector<std::tuple<std::string, std::string, nlohmann::json>>
        band_message_calls;
    std::vector<std::pair<std::string, std::string>> band_lost_calls;

    [[nodiscard]] bool has_callback(const std::string &name) const noexcept override
    {
        if (name == "on_channel_closing")      return has_on_channel_closing;
        if (name == "on_consumer_died")        return has_on_consumer_died;
        if (name == "on_hub_dead")             return has_on_hub_dead;
        if (name == "on_band_member_joined")   return has_on_band_member_joined;
        if (name == "on_band_member_left")     return has_on_band_member_left;
        if (name == "on_band_message")         return has_on_band_message;
        if (name == "on_band_lost")            return has_on_band_lost;
        return false;
    }

    void invoke_on_channel_closing(const std::string &channel,
                                    const std::string &reason) override
    {
        calls.emplace_back(channel, reason);
    }

    void invoke_on_consumer_died(const std::string &channel,
                                  const std::string &consumer_uid,
                                  const std::string &reason) override
    {
        consumer_died_calls.emplace_back(channel, consumer_uid, reason);
    }

    void invoke_on_hub_dead(const std::string &source_hub_uid) override
    {
        hub_dead_calls.emplace_back(source_hub_uid);
    }

    void invoke_on_band_member_joined(const std::string &band,
                                      const std::string &role_uid,
                                      const std::string &role_name) override
    {
        band_member_joined_calls.emplace_back(band, role_uid, role_name);
    }
    void invoke_on_band_member_left(const std::string &band,
                                    const std::string &role_uid,
                                    const std::string &reason) override
    {
        band_member_left_calls.emplace_back(band, role_uid, reason);
    }
    void invoke_on_band_message(const std::string &band,
                                const std::string &sender_role_uid,
                                const nlohmann::json &body) override
    {
        band_message_calls.emplace_back(band, sender_role_uid, body);
    }
    void invoke_on_band_lost(const std::string &band,
                             const std::string &reason) override
    {
        band_lost_calls.emplace_back(band, reason);
    }

    // ── No-op stubs for the rest of the ScriptEngine surface ────────
  protected:
    bool init_engine_(const std::string &, RoleHostCore *) override
    {
        return true;
    }
    bool build_api_(RoleAPIBase &) override { return true; }
    void finalize_engine_() override {}

  public:
    bool load_script(const std::filesystem::path &, const std::string &,
                     const std::string &) override
    {
        return true;
    }
    bool register_slot_type(const pylabhub::hub::SchemaSpec &,
                            const std::string &,
                            const std::string &) override
    {
        return true;
    }
    [[nodiscard]] size_t type_sizeof(const std::string &) const override
    {
        return 0;
    }
    bool invoke(const std::string &) override { return true; }
    bool invoke(const std::string &, const nlohmann::json &) override
    {
        return true;
    }
    InvokeResponse eval(const std::string &) override
    {
        return {InvokeStatus::NotFound, {}};
    }
    InvokeResponse invoke_returning(const std::string &,
                                     const nlohmann::json &,
                                     int64_t) override
    {
        return {InvokeStatus::NotFound, {}};
    }
    void invoke_on_init() override {}
    void invoke_on_stop() override {}
    InvokeResult invoke_produce(
        InvokeTx,
        std::vector<IncomingMessage> &) override
    {
        return InvokeResult::Commit;
    }
    InvokeResult invoke_consume(
        InvokeRx,
        std::vector<IncomingMessage> &) override
    {
        return InvokeResult::Commit;
    }
    InvokeResult invoke_process(InvokeRx, InvokeTx,
                                std::vector<IncomingMessage> &) override
    {
        return InvokeResult::Commit;
    }
    InvokeResult invoke_on_inbox(InvokeInbox) override
    {
        return InvokeResult::Commit;
    }
    [[nodiscard]] uint64_t script_error_count() const noexcept override
    {
        return 0;
    }
    [[nodiscard]] bool supports_multi_state() const noexcept override
    {
        return false;
    }
};

// Mirrors what `role_api_base.cpp`'s BRC `on_notification` lambda does:
// sets `event` (wire string for debug + generic scan) AND
// `notification_id` (parsed enum that the dispatcher reads).  Tests
// that fail to set `notification_id` would silently fall through the
// dispatcher's `Unknown` slot — these helpers prevent that mistake.
IncomingMessage make_notify(const std::string &channel,
                            const std::string &reason)
{
    IncomingMessage m;
    m.event                   = "CHANNEL_CLOSING_NOTIFY";
    m.notification_id         = parse_notification_id(m.event);
    m.details                 = nlohmann::json::object();
    m.details["channel_name"] = channel;
    m.details["reason"]       = reason;
    return m;
}

IncomingMessage make_other(const std::string &event)
{
    IncomingMessage m;
    m.event           = event;
    m.notification_id = parse_notification_id(m.event);   // typically Unknown
    return m;
}

// HUB_DEAD is synthetic (not a wire frame).  Mirrors role_api_base.cpp
// Phase 2 `on_hub_dead` lambda: sets event="HUB_DEAD",
// notification_id=HubDead, details["is_master"], source_hub_uid.
// Tests MUST set both event AND notification_id (string vs enum drift
// would silently route to the Unknown slot — these helpers prevent
// that mistake; audit D1/D2, 2026-05-18).
IncomingMessage make_hub_dead(const std::string &source_hub_uid,
                              bool is_master)
{
    IncomingMessage m;
    m.event              = "HUB_DEAD";
    m.notification_id    = parse_notification_id(m.event);
    m.details            = nlohmann::json::object();
    m.details["is_master"] = is_master;
    m.details["reason"]    = "ctrl_thread_on_hub_dead";
    m.source_hub_uid     = source_hub_uid;
    return m;
}

// S4 expansion 2026-05-19 — band-notification factory helpers.

IncomingMessage make_band_join_notify(const std::string &band,
                                      const std::string &role_uid,
                                      const std::string &role_name)
{
    IncomingMessage m;
    m.event              = "BAND_JOIN_NOTIFY";
    m.notification_id    = parse_notification_id(m.event);
    m.details            = nlohmann::json::object();
    m.details["band"]      = band;
    m.details["role_uid"]  = role_uid;
    m.details["role_name"] = role_name;
    return m;
}

IncomingMessage make_band_leave_notify(const std::string &band,
                                       const std::string &role_uid,
                                       const std::string &reason)
{
    IncomingMessage m;
    m.event              = "BAND_LEAVE_NOTIFY";
    m.notification_id    = parse_notification_id(m.event);
    m.details            = nlohmann::json::object();
    m.details["band"]      = band;
    m.details["role_uid"]  = role_uid;
    m.details["reason"]    = reason;
    return m;
}

IncomingMessage make_band_broadcast_notify(const std::string &band,
                                           const std::string &sender,
                                           const nlohmann::json &body)
{
    IncomingMessage m;
    m.event              = "BAND_BROADCAST_NOTIFY";
    m.notification_id    = parse_notification_id(m.event);
    m.details            = nlohmann::json::object();
    m.details["band"]      = band;
    m.details["role_uid"]  = sender;
    m.details["body"]      = body;
    return m;
}

IncomingMessage make_band_lost(const std::string &band,
                               const std::string &reason)
{
    IncomingMessage m;
    m.event              = "BAND_LOST";
    m.notification_id    = parse_notification_id(m.event);
    m.details            = nlohmann::json::object();
    m.details["band"]    = band;
    m.details["reason"]  = reason;
    return m;
}

} // anonymous namespace

class DispatchChannelClosingTest : public ::testing::Test
{
};

// ============================================================================
// Contract 1 (audit D1, 2026-05-18): channel-closing is PATTERN A
// (default-action with overridable callback).  Callback absent →
// framework MUST consume the notify AND request stop with
// StopReason::ChannelClosed.  Reversal of M1.5 (2026-05-14) original
// "no-default-action" behavior — see docs/tech_draft/
// M1.5_channel_closing_redesign_2026-05-12.md §0 for the design call
// (2026-05-15T03:29:14) and the corrected semantics.
// ============================================================================

TEST_F(DispatchChannelClosingTest, NoCallback_DefaultStopsAndConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_notify("ch.a", "producer_deregistered"));
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_notify("ch.b", "pending_timeout"));

    EXPECT_FALSE(core.is_shutdown_requested())
        << "precondition: stop must not yet be requested";

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.calls.empty())
        << "engine must not be called when has_callback returns false";
    // Channel-closing notifies CONSUMED (default action took them).
    // Only the non-notify entry remains.
    ASSERT_EQ(msgs.size(), 1u)
        << "all CHANNEL_CLOSING_NOTIFY entries must be consumed by the "
           "default-stop path (audit D1)";
    EXPECT_EQ(msgs[0].event, "HEARTBEAT_ACK");

    EXPECT_TRUE(core.is_shutdown_requested())
        << "default action MUST call request_stop() when no callback";
    EXPECT_EQ(core.stop_reason_string(), "channel_closed")
        << "default action MUST tag StopReason::ChannelClosed before "
           "request_stop()";
}

// Mutation-sweep guard: if a future refactor removes the
// set_stop_reason() call before request_stop(), the stop_reason will
// fall back to Normal — this companion test pins that the reason is
// distinguishable from a plain api.stop() call.
TEST_F(DispatchChannelClosingTest, NoCallback_ReasonDistinguishableFromGenericStop)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_notify("ch.x", "producer_deregistered"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_NE(core.stop_reason_string(), "normal")
        << "channel-closing default-stop must tag the reason; otherwise "
           "downstream readers can't distinguish from generic api.stop()";
}

// ============================================================================
// Contract 2: callback present → all notify entries dispatched + removed
// ============================================================================

TEST_F(DispatchChannelClosingTest, Callback_DispatchesAndRemovesNotify)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_notify("ch.alpha", "producer_deregistered"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.calls.size(), 1u);
    EXPECT_EQ(eng.calls[0].first,  "ch.alpha");
    EXPECT_EQ(eng.calls[0].second, "producer_deregistered");
    EXPECT_TRUE(msgs.empty())
        << "CHANNEL_CLOSING_NOTIFY must be removed from msgs after "
           "dispatch (single-delivery contract)";

    // Audit D1 (2026-05-18) — callback REPLACES the default action.
    // The framework must NOT request_stop() when the script has
    // chosen to handle the event itself (script may keep the role
    // alive for reconnection / best-effort logic).
    EXPECT_FALSE(core.is_shutdown_requested())
        << "callback replaces default — framework must not call "
           "request_stop() when on_channel_closing is defined";
    EXPECT_EQ(core.stop_reason_string(), "normal")
        << "no stop reason should be set when callback fires";
}

// ============================================================================
// Contract 3: mixed entries — only notifies removed, others preserved in order
// ============================================================================

TEST_F(DispatchChannelClosingTest, Callback_PreservesNonNotifyEntries)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_notify("ch.a", "voluntary"));
    msgs.push_back(make_other("CHANNEL_EVENT_NOTIFY"));
    msgs.push_back(make_notify("ch.b", "pending_timeout"));
    msgs.push_back(make_other("OTHER"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.calls.size(), 2u);
    EXPECT_EQ(eng.calls[0].first,  "ch.a");
    EXPECT_EQ(eng.calls[0].second, "voluntary");
    EXPECT_EQ(eng.calls[1].first,  "ch.b");
    EXPECT_EQ(eng.calls[1].second, "pending_timeout");

    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].event, "HEARTBEAT_ACK");
    EXPECT_EQ(msgs[1].event, "CHANNEL_EVENT_NOTIFY");
    EXPECT_EQ(msgs[2].event, "OTHER");
}

// ============================================================================
// Edge: empty msgs vector — no-op regardless of callback state
// ============================================================================

TEST_F(DispatchChannelClosingTest, EmptyMsgs_NoOp_NoCallback)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = false;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.calls.empty());
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchChannelClosingTest, EmptyMsgs_NoOp_WithCallback)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = true;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.calls.empty())
        << "no notify entries to dispatch — callback must NOT fire";
    EXPECT_TRUE(msgs.empty());
}

// ============================================================================
// Edge: notify with missing/wrong-type fields — empty strings, no crash
// ============================================================================

TEST_F(DispatchChannelClosingTest, NotifyWithMissingFields_DispatchesWithEmptyStrings)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = true;

    std::vector<IncomingMessage> msgs;
    {
        // Mirror the BRC enqueue: event + notification_id MUST both
        // be set or the dispatcher's table lookup falls through to
        // the Unknown slot.  This test intentionally omits details
        // fields (not the notification_id) to verify the dispatcher's
        // value(..., default) extraction handles missing details.
        IncomingMessage m;
        m.event           = "CHANNEL_CLOSING_NOTIFY";
        m.notification_id = parse_notification_id(m.event);
        m.details         = nlohmann::json::object();   // no channel_name / reason
        msgs.push_back(m);
    }

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.calls.size(), 1u);
    EXPECT_EQ(eng.calls[0].first,  "")
        << "missing channel_name must surface as empty string, not crash";
    EXPECT_EQ(eng.calls[0].second, "")
        << "missing reason must surface as empty string, not crash";
    EXPECT_TRUE(msgs.empty());
}

// ============================================================================
// dispatch_notifications — CONSUMER_DIED_NOTIFY scenarios
// ============================================================================

namespace
{

IncomingMessage make_consumer_died(const std::string &channel,
                                   const std::string &consumer_uid,
                                   const std::string &reason)
{
    IncomingMessage m;
    m.event                     = "CONSUMER_DIED_NOTIFY";
    m.notification_id           = parse_notification_id(m.event);
    m.details                   = nlohmann::json::object();
    m.details["channel_name"]   = channel;
    m.details["role_uid"]   = consumer_uid;
    m.details["reason"]         = reason;
    return m;
}

} // namespace

class DispatchConsumerDiedTest : public ::testing::Test
{
};

// Contract 1 (audit D1/D2, 2026-05-18): under the unified
// "user-override-or-native-default" model, CONSUMER_DIED's default is
// no-op — but the notify is STILL consumed.  Producers that don't
// define on_consumer_died never see CONSUMER_DIED_NOTIFY in msgs.
// Non-notify entries (HEARTBEAT_ACK etc.) are preserved.
// No default stop fires (the producer survives consumer death).
TEST_F(DispatchConsumerDiedTest, NoCallback_DefaultNoOpButConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_consumer_died = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_consumer_died("ch.a", "cons-1", "heartbeat_timeout"));
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_consumer_died("ch.b", "cons-2", "process_dead"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.consumer_died_calls.empty())
        << "engine must not be called when has_callback returns false";
    ASSERT_EQ(msgs.size(), 1u)
        << "CONSUMER_DIED_NOTIFY entries must be consumed by the "
           "default-no-op path (unified dispatch — audit D1/D2)";
    EXPECT_EQ(msgs[0].event, "HEARTBEAT_ACK");

    EXPECT_FALSE(core.is_shutdown_requested())
        << "consumer death is not by itself reason to stop the "
           "producer; default action MUST be a no-op";
    EXPECT_EQ(core.stop_reason_string(), "normal");
}

// Contract 2: callback present → all notify entries dispatched + removed.
// Carries both reason variants — heartbeat_timeout (Wave-B M2) and
// process_dead (PID-liveness path) — through the same dispatcher.
TEST_F(DispatchConsumerDiedTest, Callback_DispatchesAndRemovesBothReasons)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_consumer_died = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_consumer_died("ch.alpha", "cons-hb",   "heartbeat_timeout"));
    msgs.push_back(make_consumer_died("ch.beta",  "cons-dead", "process_dead"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.consumer_died_calls.size(), 2u);
    EXPECT_EQ(std::get<0>(eng.consumer_died_calls[0]), "ch.alpha");
    EXPECT_EQ(std::get<1>(eng.consumer_died_calls[0]), "cons-hb");
    EXPECT_EQ(std::get<2>(eng.consumer_died_calls[0]), "heartbeat_timeout");
    EXPECT_EQ(std::get<0>(eng.consumer_died_calls[1]), "ch.beta");
    EXPECT_EQ(std::get<1>(eng.consumer_died_calls[1]), "cons-dead");
    EXPECT_EQ(std::get<2>(eng.consumer_died_calls[1]), "process_dead");
    EXPECT_TRUE(msgs.empty())
        << "CONSUMER_DIED_NOTIFY must be removed from msgs (single delivery)";
}

// Contract 3: mixed entries — only notifies removed, others preserved in order.
TEST_F(DispatchConsumerDiedTest, Callback_PreservesNonNotifyEntries)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_consumer_died = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_consumer_died("ch.a", "u1", "heartbeat_timeout"));
    msgs.push_back(make_other("CHANNEL_EVENT_NOTIFY"));
    msgs.push_back(make_consumer_died("ch.b", "u2", "process_dead"));
    msgs.push_back(make_other("OTHER"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.consumer_died_calls.size(), 2u);
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].event, "HEARTBEAT_ACK");
    EXPECT_EQ(msgs[1].event, "CHANNEL_EVENT_NOTIFY");
    EXPECT_EQ(msgs[2].event, "OTHER");
}

// Edge: empty msgs vector — no-op regardless of callback state.
TEST_F(DispatchConsumerDiedTest, EmptyMsgs_NoOp_NoCallback)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_consumer_died = false;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.consumer_died_calls.empty());
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchConsumerDiedTest, EmptyMsgs_NoOp_WithCallback)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_consumer_died = true;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.consumer_died_calls.empty())
        << "no notify entries to dispatch — callback must NOT fire";
    EXPECT_TRUE(msgs.empty());
}

// Edge: notify with missing fields — empty strings, no crash.
TEST_F(DispatchConsumerDiedTest, NotifyWithMissingFields_DispatchesWithEmptyStrings)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_consumer_died = true;

    std::vector<IncomingMessage> msgs;
    {
        // Mirror the BRC enqueue: event + notification_id MUST both
        // be set or the dispatcher's table lookup falls through to
        // the Unknown slot and the callback never fires.  This test
        // intentionally omits details fields (not the event_id) to
        // verify the dispatcher's value(..., default) extraction.
        IncomingMessage m;
        m.event           = "CONSUMER_DIED_NOTIFY";
        m.notification_id = parse_notification_id(m.event);
        m.details         = nlohmann::json::object();
        msgs.push_back(m);
    }

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.consumer_died_calls.size(), 1u);
    EXPECT_EQ(std::get<0>(eng.consumer_died_calls[0]), "");
    EXPECT_EQ(std::get<1>(eng.consumer_died_calls[0]), "");
    EXPECT_EQ(std::get<2>(eng.consumer_died_calls[0]), "");
    EXPECT_TRUE(msgs.empty());
}

// Single-pass dispatch: one call handles a mixed-event msgs vector
// (both CHANNEL_CLOSING_NOTIFY and CONSUMER_DIED_NOTIFY in one cycle).
// Replaces the previous design that required separate dispatcher calls
// per event type.  Pins: (a) both handlers fire; (b) both consumed
// from msgs; (c) wire arrival order = dispatch order.  Mutation:
// swap the kNotificationTable row for ChannelClosing with the one
// for ConsumerDied → the channel-closing message gets sent to the
// consumer_died handler and the test fails on argument mismatch.
TEST_F(DispatchConsumerDiedTest, MixedNotifies_SinglePassDispatch)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = true;
    eng.has_on_consumer_died   = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_notify("ch.x", "producer_deregistered"));   // first
    msgs.push_back(make_consumer_died("ch.y", "cons-z", "heartbeat_timeout"));  // second

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    // Both handlers fired exactly once.
    ASSERT_EQ(eng.calls.size(), 1u);
    EXPECT_EQ(eng.calls[0].first,  "ch.x");
    EXPECT_EQ(eng.calls[0].second, "producer_deregistered");

    ASSERT_EQ(eng.consumer_died_calls.size(), 1u);
    EXPECT_EQ(std::get<0>(eng.consumer_died_calls[0]), "ch.y");
    EXPECT_EQ(std::get<1>(eng.consumer_died_calls[0]), "cons-z");
    EXPECT_EQ(std::get<2>(eng.consumer_died_calls[0]), "heartbeat_timeout");

    // Both consumed from msgs.
    EXPECT_TRUE(msgs.empty());
}

// Unrecognised notification_id (Unknown) — message stays in msgs
// untouched even if scripts define every known callback.  This is
// the "generic scan" path: future broker notifications not yet in
// the NotificationId enum should not be silently lost.
TEST_F(DispatchConsumerDiedTest, UnknownNotificationId_StaysInMsgs)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_channel_closing = true;
    eng.has_on_consumer_died   = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_other("SOME_FUTURE_NOTIFY"));   // notification_id = Unknown

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.calls.empty());
    EXPECT_TRUE(eng.consumer_died_calls.empty());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].event, "SOME_FUTURE_NOTIFY");
    EXPECT_EQ(msgs[0].notification_id, NotificationId::Unknown);
}

// ============================================================================
// HUB_DEAD dispatch — Pattern A (default-action with overridable callback).
// Audit D1/D2 (2026-05-18).  Mirrors on_channel_closing semantics but
// with an asymmetric default action (master → stop; peer → no-op)
// because the master ctrl thread drives the heartbeat timer (HEP-0023
// §2.5).  Script's `on_hub_dead` callback REPLACES the default for
// BOTH master and peer hub-deaths.
// ============================================================================

class DispatchHubDeadTest : public ::testing::Test
{
};

// Master hub-death, no callback → framework calls request_stop()
// with StopReason::HubDead; notify consumed from msgs.
TEST_F(DispatchHubDeadTest, NoCallback_MasterDefaultStopsAndConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_hub_dead = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_hub_dead("tcp://broker-a:5555", /*is_master=*/true));
    msgs.push_back(make_other("HEARTBEAT_ACK"));

    EXPECT_FALSE(core.is_shutdown_requested())
        << "precondition: stop must not yet be requested";

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.hub_dead_calls.empty())
        << "engine must not be called when has_callback returns false";
    ASSERT_EQ(msgs.size(), 1u)
        << "HUB_DEAD must be consumed by the default-action path";
    EXPECT_EQ(msgs[0].event, "HEARTBEAT_ACK");

    EXPECT_TRUE(core.is_shutdown_requested())
        << "master hub-death default MUST call request_stop()";
    EXPECT_EQ(core.stop_reason_string(), "hub_dead")
        << "master hub-death default MUST tag StopReason::HubDead";
}

// Peer hub-death, no callback → framework does NOTHING (role keeps
// running on master per HEP-CORE-0023 §2.5).  Notify still consumed
// (Pattern A).
TEST_F(DispatchHubDeadTest, NoCallback_PeerNoOpButConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_hub_dead = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_hub_dead("tcp://broker-b:5555", /*is_master=*/false));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.hub_dead_calls.empty());
    EXPECT_TRUE(msgs.empty())
        << "peer HUB_DEAD must still be consumed even though no default "
           "stop fires (Pattern A — handler returns true uniformly)";

    EXPECT_FALSE(core.is_shutdown_requested())
        << "peer hub-death without script callback MUST NOT stop the "
           "role (HEP-CORE-0023 §2.5 — role keeps running on master)";
    EXPECT_EQ(core.stop_reason_string(), "normal");
}

// Master hub-death with callback → callback fires, framework takes
// NO default action.  Script may keep role alive for reconnection.
TEST_F(DispatchHubDeadTest, MasterCallback_ReplacesDefaultStop)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_hub_dead = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_hub_dead("tcp://broker-a:5555", /*is_master=*/true));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.hub_dead_calls.size(), 1u);
    EXPECT_EQ(eng.hub_dead_calls[0], "tcp://broker-a:5555")
        << "source_hub_uid must reach the callback verbatim";
    EXPECT_TRUE(msgs.empty());

    EXPECT_FALSE(core.is_shutdown_requested())
        << "callback REPLACES the default-stop — framework must not "
           "call request_stop() even on master hub-death (audit D2)";
}

// Peer hub-death with callback → callback fires (script wants to
// react to peer-death too); framework takes no default action.
TEST_F(DispatchHubDeadTest, PeerCallback_FiresAndNoOp)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_hub_dead = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_hub_dead("tcp://broker-b:5555", /*is_master=*/false));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.hub_dead_calls.size(), 1u);
    EXPECT_EQ(eng.hub_dead_calls[0], "tcp://broker-b:5555");
    EXPECT_TRUE(msgs.empty());

    EXPECT_FALSE(core.is_shutdown_requested())
        << "peer-death callback path must not request stop (script "
           "may call api.stop() itself if it wants to)";
}

// Mutation guard: if a future refactor flips the master/peer default
// (stops on peer-death) the no-callback peer test catches it.  This
// test pins the inverse (master stops even when peer is also dead in
// the same cycle) — defends against accidental cross-wiring.
TEST_F(DispatchHubDeadTest, NoCallback_MasterAndPeerSameCycle)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_hub_dead = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_hub_dead("tcp://broker-a:5555", /*is_master=*/false));  // peer first
    msgs.push_back(make_hub_dead("tcp://broker-b:5555", /*is_master=*/true));   // master second

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.hub_dead_calls.empty());
    EXPECT_TRUE(msgs.empty())
        << "both HUB_DEAD entries consumed by Pattern-A dispatch";
    EXPECT_TRUE(core.is_shutdown_requested())
        << "master hub-death in the batch MUST request stop";
    EXPECT_EQ(core.stop_reason_string(), "hub_dead");
}

// ============================================================================
// Band callbacks (S4 expansion 2026-05-19, HEP-CORE-0030 §5.3).
//
// All 4 band callbacks default to no-op (bands are script-domain
// coordination; framework only delivers events).  Tests pin:
//   - No callback defined → dispatcher consumes the notify but
//     does NOT touch the engine, does NOT request stop, does NOT
//     leave the entry in `msgs`.
//   - Callback defined → engine.invoke_on_X is called with the
//     correct args; entry consumed; framework still doesn't stop.
//
// Mutation guards: if a future refactor accidentally drops a band
// notify into the default-stop path, the no-callback test fails
// (core.is_shutdown_requested()).  If args are misordered or wire-
// field names drift, the with-callback test fails (recording
// vector content mismatch).
// ============================================================================

class DispatchBandTest : public ::testing::Test
{
};

TEST_F(DispatchBandTest, MemberJoined_NoCallback_DefaultNoOpButConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_member_joined = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_band_join_notify("!ctrl", "prod.lab.uid01", "lab_a"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.band_member_joined_calls.empty());
    EXPECT_TRUE(msgs.empty()) << "BAND_JOIN_NOTIFY must be consumed";
    EXPECT_FALSE(core.is_shutdown_requested())
        << "default band-callback action MUST be no-op (not stop)";
}

TEST_F(DispatchBandTest, MemberJoined_Callback_DispatchesAndRemovesNotify)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_member_joined = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_band_join_notify("!ctrl",
                                          "prod.lab.uid01",
                                          "lab_a"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.band_member_joined_calls.size(), 1u);
    EXPECT_EQ(std::get<0>(eng.band_member_joined_calls[0]), "!ctrl");
    EXPECT_EQ(std::get<1>(eng.band_member_joined_calls[0]), "prod.lab.uid01");
    EXPECT_EQ(std::get<2>(eng.band_member_joined_calls[0]), "lab_a");
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchBandTest, MemberLeft_NoCallback_DefaultNoOpButConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_member_left = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_band_leave_notify("!ctrl", "prod.lab.uid01",
                                           "heartbeat_timeout"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.band_member_left_calls.empty());
    EXPECT_TRUE(msgs.empty());
    EXPECT_FALSE(core.is_shutdown_requested());
}

TEST_F(DispatchBandTest, MemberLeft_Callback_DispatchesWithReason)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_member_left = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_band_leave_notify("!ctrl", "cons.lab.uid02",
                                           "voluntary"));
    msgs.push_back(make_band_leave_notify("!ctrl", "prod.lab.uid03",
                                           "process_dead"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.band_member_left_calls.size(), 2u);
    EXPECT_EQ(std::get<2>(eng.band_member_left_calls[0]), "voluntary");
    EXPECT_EQ(std::get<2>(eng.band_member_left_calls[1]), "process_dead");
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchBandTest, Message_NoCallback_DefaultNoOpButConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_message = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_band_broadcast_notify(
        "!ctrl", "prod.lab.uid01",
        nlohmann::json{{"event", "calibration_done"}}));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.band_message_calls.empty());
    EXPECT_TRUE(msgs.empty());
    EXPECT_FALSE(core.is_shutdown_requested());
}

TEST_F(DispatchBandTest, Message_Callback_DispatchesWithBody)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_message = true;

    std::vector<IncomingMessage> msgs;
    auto body = nlohmann::json{{"event", "start_run"},
                               {"params", {{"duration_s", 60}}}};
    msgs.push_back(make_band_broadcast_notify(
        "!sensor_sync", "prod.lab.sensor_a", body));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.band_message_calls.size(), 1u);
    EXPECT_EQ(std::get<0>(eng.band_message_calls[0]), "!sensor_sync");
    EXPECT_EQ(std::get<1>(eng.band_message_calls[0]), "prod.lab.sensor_a");
    EXPECT_EQ(std::get<2>(eng.band_message_calls[0]), body);
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchBandTest, Lost_NoCallback_DefaultNoOpButConsumes)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_lost = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_band_lost("!ctrl", "hub_dead"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    EXPECT_TRUE(eng.band_lost_calls.empty());
    EXPECT_TRUE(msgs.empty());
    EXPECT_FALSE(core.is_shutdown_requested())
        << "default on_band_lost action MUST be no-op — peer-hub "
           "loss doesn't stop the role; scripts override to react";
}

TEST_F(DispatchBandTest, Lost_Callback_DispatchesWithBandAndReason)
{
    RecordingEngine eng;
    RoleHostCore core;
    eng.has_on_band_lost = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_band_lost("!ctrl_a", "hub_dead"));
    msgs.push_back(make_band_lost("!ctrl_b", "hub_dead"));

    pylabhub::scripting::dispatch_notifications(eng, msgs,
        pylabhub::scripting::StopRequestor{core});

    ASSERT_EQ(eng.band_lost_calls.size(), 2u);
    EXPECT_EQ(eng.band_lost_calls[0].first,  "!ctrl_a");
    EXPECT_EQ(eng.band_lost_calls[0].second, "hub_dead");
    EXPECT_EQ(eng.band_lost_calls[1].first,  "!ctrl_b");
    EXPECT_TRUE(msgs.empty());
}
