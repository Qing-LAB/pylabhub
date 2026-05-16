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
class RecordingEngine : public ScriptEngine
{
  public:
    /// When true, has_callback("on_channel_closing") returns true.
    bool has_on_channel_closing{false};
    /// When true, has_callback("on_consumer_died") returns true.
    bool has_on_consumer_died{false};

    /// Recorded on_channel_closing (channel, reason) pairs.
    std::vector<std::pair<std::string, std::string>> calls;

    /// Recorded on_consumer_died (channel, consumer_uid, reason) tuples.
    std::vector<std::tuple<std::string, std::string, std::string>>
        consumer_died_calls;

    [[nodiscard]] bool has_callback(const std::string &name) const noexcept override
    {
        if (name == "on_channel_closing") return has_on_channel_closing;
        if (name == "on_consumer_died")   return has_on_consumer_died;
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

} // anonymous namespace

class DispatchChannelClosingTest : public ::testing::Test
{
};

// ============================================================================
// Contract 1: callback absent → msgs unchanged, engine not called
// ============================================================================

TEST_F(DispatchChannelClosingTest, NoCallback_LeavesMsgsUnchanged)
{
    RecordingEngine eng;
    eng.has_on_channel_closing = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_notify("ch.a", "producer_deregistered"));
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_notify("ch.b", "pending_timeout"));

    pylabhub::scripting::dispatch_notifications(eng, msgs);

    EXPECT_TRUE(eng.calls.empty())
        << "engine must not be called when has_callback returns false";
    ASSERT_EQ(msgs.size(), 3u)
        << "msgs must be unchanged when callback is absent";
    EXPECT_EQ(msgs[0].event, "CHANNEL_CLOSING_NOTIFY");
    EXPECT_EQ(msgs[1].event, "HEARTBEAT_ACK");
    EXPECT_EQ(msgs[2].event, "CHANNEL_CLOSING_NOTIFY");
}

// ============================================================================
// Contract 2: callback present → all notify entries dispatched + removed
// ============================================================================

TEST_F(DispatchChannelClosingTest, Callback_DispatchesAndRemovesNotify)
{
    RecordingEngine eng;
    eng.has_on_channel_closing = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_notify("ch.alpha", "producer_deregistered"));

    pylabhub::scripting::dispatch_notifications(eng, msgs);

    ASSERT_EQ(eng.calls.size(), 1u);
    EXPECT_EQ(eng.calls[0].first,  "ch.alpha");
    EXPECT_EQ(eng.calls[0].second, "producer_deregistered");
    EXPECT_TRUE(msgs.empty())
        << "CHANNEL_CLOSING_NOTIFY must be removed from msgs after "
           "dispatch (single-delivery contract)";
}

// ============================================================================
// Contract 3: mixed entries — only notifies removed, others preserved in order
// ============================================================================

TEST_F(DispatchChannelClosingTest, Callback_PreservesNonNotifyEntries)
{
    RecordingEngine eng;
    eng.has_on_channel_closing = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_notify("ch.a", "voluntary"));
    msgs.push_back(make_other("CHANNEL_EVENT_NOTIFY"));
    msgs.push_back(make_notify("ch.b", "pending_timeout"));
    msgs.push_back(make_other("OTHER"));

    pylabhub::scripting::dispatch_notifications(eng, msgs);

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
    eng.has_on_channel_closing = false;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs);

    EXPECT_TRUE(eng.calls.empty());
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchChannelClosingTest, EmptyMsgs_NoOp_WithCallback)
{
    RecordingEngine eng;
    eng.has_on_channel_closing = true;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs);

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

    pylabhub::scripting::dispatch_notifications(eng, msgs);

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
    m.details["consumer_uid"]   = consumer_uid;
    m.details["reason"]         = reason;
    return m;
}

} // namespace

class DispatchConsumerDiedTest : public ::testing::Test
{
};

// Contract 1: callback absent → msgs unchanged, engine not called.
TEST_F(DispatchConsumerDiedTest, NoCallback_LeavesMsgsUnchanged)
{
    RecordingEngine eng;
    eng.has_on_consumer_died = false;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_consumer_died("ch.a", "cons-1", "heartbeat_timeout"));
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_consumer_died("ch.b", "cons-2", "process_dead"));

    pylabhub::scripting::dispatch_notifications(eng, msgs);

    EXPECT_TRUE(eng.consumer_died_calls.empty())
        << "engine must not be called when has_callback returns false";
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].event, "CONSUMER_DIED_NOTIFY");
    EXPECT_EQ(msgs[1].event, "HEARTBEAT_ACK");
    EXPECT_EQ(msgs[2].event, "CONSUMER_DIED_NOTIFY");
}

// Contract 2: callback present → all notify entries dispatched + removed.
// Carries both reason variants — heartbeat_timeout (Wave-B M2) and
// process_dead (PID-liveness path) — through the same dispatcher.
TEST_F(DispatchConsumerDiedTest, Callback_DispatchesAndRemovesBothReasons)
{
    RecordingEngine eng;
    eng.has_on_consumer_died = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_consumer_died("ch.alpha", "cons-hb",   "heartbeat_timeout"));
    msgs.push_back(make_consumer_died("ch.beta",  "cons-dead", "process_dead"));

    pylabhub::scripting::dispatch_notifications(eng, msgs);

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
    eng.has_on_consumer_died = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_other("HEARTBEAT_ACK"));
    msgs.push_back(make_consumer_died("ch.a", "u1", "heartbeat_timeout"));
    msgs.push_back(make_other("CHANNEL_EVENT_NOTIFY"));
    msgs.push_back(make_consumer_died("ch.b", "u2", "process_dead"));
    msgs.push_back(make_other("OTHER"));

    pylabhub::scripting::dispatch_notifications(eng, msgs);

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
    eng.has_on_consumer_died = false;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs);

    EXPECT_TRUE(eng.consumer_died_calls.empty());
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchConsumerDiedTest, EmptyMsgs_NoOp_WithCallback)
{
    RecordingEngine eng;
    eng.has_on_consumer_died = true;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_notifications(eng, msgs);

    EXPECT_TRUE(eng.consumer_died_calls.empty())
        << "no notify entries to dispatch — callback must NOT fire";
    EXPECT_TRUE(msgs.empty());
}

// Edge: notify with missing fields — empty strings, no crash.
TEST_F(DispatchConsumerDiedTest, NotifyWithMissingFields_DispatchesWithEmptyStrings)
{
    RecordingEngine eng;
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

    pylabhub::scripting::dispatch_notifications(eng, msgs);

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
// swap the kNotificationHandlers slot for ChannelClosing with the one
// for ConsumerDied → the channel-closing message gets sent to the
// consumer_died handler and the test fails on argument mismatch.
TEST_F(DispatchConsumerDiedTest, MixedNotifies_SinglePassDispatch)
{
    RecordingEngine eng;
    eng.has_on_channel_closing = true;
    eng.has_on_consumer_died   = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_notify("ch.x", "producer_deregistered"));   // first
    msgs.push_back(make_consumer_died("ch.y", "cons-z", "heartbeat_timeout"));  // second

    pylabhub::scripting::dispatch_notifications(eng, msgs);

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
    eng.has_on_channel_closing = true;
    eng.has_on_consumer_died   = true;

    std::vector<IncomingMessage> msgs;
    msgs.push_back(make_other("SOME_FUTURE_NOTIFY"));   // notification_id = Unknown

    pylabhub::scripting::dispatch_notifications(eng, msgs);

    EXPECT_TRUE(eng.calls.empty());
    EXPECT_TRUE(eng.consumer_died_calls.empty());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].event, "SOME_FUTURE_NOTIFY");
    EXPECT_EQ(msgs[0].notification_id, NotificationId::Unknown);
}
