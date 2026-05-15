/**
 * @file test_dispatch_channel_closing.cpp
 * @brief L2 unit tests for `pylabhub::scripting::dispatch_channel_closing`
 *        — the helper that routes CHANNEL_CLOSING_NOTIFY entries from the
 *        per-cycle `msgs` list to the script's `on_channel_closing`
 *        callback when defined.
 *
 * Behavior contract (HEP-CORE-0011 lifecycle table; M1.5):
 *   1. If `engine.has_callback("on_channel_closing")` is false → msgs
 *      is unchanged; engine.invoke_on_channel_closing is NOT called.
 *   2. If `has_callback` is true → every CHANNEL_CLOSING_NOTIFY entry
 *      is removed from msgs, and `invoke_on_channel_closing(channel,
 *      reason)` is called once per entry with values from
 *      `details.channel_name` / `details.reason`.
 *   3. Non-CHANNEL_CLOSING_NOTIFY entries stay in msgs in original
 *      order.
 */

#include "binary_lifecycle.h"
#include "service/cycle_ops.hpp"   // dispatch_channel_closing
#include "utils/logger.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::InvokeInbox;
using pylabhub::scripting::InvokeResponse;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeStatus;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::ScriptEngine;

// `dispatch_channel_closing` calls into engine.has_callback() +
// engine.invoke_on_channel_closing().  Engine surface uses LOGGER_*
// on error paths.  Single-suite binary, Logger-only guard.
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

    /// Recorded (channel, reason) pairs.
    std::vector<std::pair<std::string, std::string>> calls;

    [[nodiscard]] bool has_callback(const std::string &name) const noexcept override
    {
        return name == "on_channel_closing" && has_on_channel_closing;
    }

    void invoke_on_channel_closing(const std::string &channel,
                                    const std::string &reason) override
    {
        calls.emplace_back(channel, reason);
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

IncomingMessage make_notify(const std::string &channel,
                            const std::string &reason)
{
    IncomingMessage m;
    m.event           = "CHANNEL_CLOSING_NOTIFY";
    m.details         = nlohmann::json::object();
    m.details["channel_name"] = channel;
    m.details["reason"]       = reason;
    return m;
}

IncomingMessage make_other(const std::string &event)
{
    IncomingMessage m;
    m.event = event;
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

    pylabhub::scripting::dispatch_channel_closing(eng, msgs);

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

    pylabhub::scripting::dispatch_channel_closing(eng, msgs);

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

    pylabhub::scripting::dispatch_channel_closing(eng, msgs);

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

    pylabhub::scripting::dispatch_channel_closing(eng, msgs);

    EXPECT_TRUE(eng.calls.empty());
    EXPECT_TRUE(msgs.empty());
}

TEST_F(DispatchChannelClosingTest, EmptyMsgs_NoOp_WithCallback)
{
    RecordingEngine eng;
    eng.has_on_channel_closing = true;
    std::vector<IncomingMessage> msgs;

    pylabhub::scripting::dispatch_channel_closing(eng, msgs);

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
        IncomingMessage m;
        m.event   = "CHANNEL_CLOSING_NOTIFY";
        m.details = nlohmann::json::object();   // no channel_name / reason
        msgs.push_back(m);
    }

    pylabhub::scripting::dispatch_channel_closing(eng, msgs);

    ASSERT_EQ(eng.calls.size(), 1u);
    EXPECT_EQ(eng.calls[0].first,  "")
        << "missing channel_name must surface as empty string, not crash";
    EXPECT_EQ(eng.calls[0].second, "")
        << "missing reason must surface as empty string, not crash";
    EXPECT_TRUE(msgs.empty());
}
