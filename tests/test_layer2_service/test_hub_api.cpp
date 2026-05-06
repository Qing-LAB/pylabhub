/**
 * @file test_hub_api.cpp
 * @brief HubAPI unit tests — Phase 7 Commit D1 (lifecycle / log / uid /
 *        metrics) + Phase 8c (post_event, augment_*, augment timeout).
 *
 * Tests the script-visible API surface in isolation, without spinning
 * up HubHost or any script engine.  Construction, log(), uid(),
 * post_event() name validation, and augment_*() routing logic are
 * fully testable here; metrics() and end-to-end script-callback
 * dispatch require a wired HubHost backref + started broker and are
 * exercised by L3 integration tests instead.
 *
 * Coverage matrix vs HEP-CORE-0033:
 *   §12.3 read accessors / control delegates  — defended pre-wiring fallbacks here
 *   §12.2.2 augmentation hooks (augment_*)    — engine-routing logic here
 *   §12.2.3 user-posted events (post_event)   — name validation + enqueue here
 */

#include "utils/hub_api.hpp"

#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"
#include "plh_service.hpp"
#include "log_capture_fixture.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using pylabhub::utils::Logger;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::MakeModDefList;
using pylabhub::scripting::RoleHostCore;
using pylabhub::hub_host::HubAPI;

class HubApiTest : public ::testing::Test,
                    public pylabhub::tests::LogCaptureFixture
{
public:
    static void SetUpTestSuite()
    {
        // Logger module required so LogCaptureFixture::Install() can
        // call Logger::set_logfile() — same pattern used by other
        // standalone L2 fixtures (test_role_host_core, etc.).
        s_lifecycle_ = std::make_unique<LifecycleGuard>(
            MakeModDefList(Logger::GetLifecycleModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override    { LogCaptureFixture::Install(); }
    void TearDown() override
    {
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
    }

    /// Standalone HubAPI fixture — no HubHost backref, no engine.
    /// Exercises construction + the host-independent methods (log, uid).
    HubAPI make_api(const std::string &uid = "hub.test.uid00000001")
    {
        return HubAPI(core_, "hub", uid);
    }

    RoleHostCore core_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> HubApiTest::s_lifecycle_;

// ─── Construction invariants ─────────────────────────────────────────────────

TEST_F(HubApiTest, Construct_RoleTagAndUid_NonEmpty)
{
    // Both fields are required at construction; throws std::invalid_argument
    // on empty input — matches RoleAPIBase contract.
    EXPECT_THROW({
        HubAPI bad(core_, "", "hub.test.uid00000001");
    }, std::invalid_argument);

    EXPECT_THROW({
        HubAPI bad(core_, "hub", "");
    }, std::invalid_argument);
}

TEST_F(HubApiTest, Construct_Valid_UidAccessorReturnsConstructorValue)
{
    auto api = make_api("hub.lab1.uid12345678");
    EXPECT_EQ(api.uid(), "hub.lab1.uid12345678");
}

// ─── log() — level routing ───────────────────────────────────────────────────
//
// Path-discriminating tests per audit §1.1 Class A:  ExpectLogWarn /
// ExpectLogError only DECLARE an allowlist (so unexpected emissions
// fail), but they do NOT enforce the expected emission actually fired.
// To gate the level mapping itself we read the capture log file
// directly and grep for the expected `[<LEVEL>]` boundary plus the
// uid-prefixed body.  Without this affirmative check, a regression
// that mapped warn→info would silently pass (verified by mutation
// sweep 2026-05-03 during D1 implementation).

namespace
{
/// Read the capture file and return true if any line contains BOTH
/// the level boundary (`] [WARN ` / `] [ERROR ` / `] [INFO `) and
/// the body substring.  Caller flushes the Logger first.
bool log_file_contains(const std::filesystem::path &path,
                        std::string_view level_boundary,
                        std::string_view body_needle)
{
    pylabhub::utils::Logger::instance().flush();
    std::ifstream f(path);
    std::string   line;
    while (std::getline(f, line))
    {
        if (line.find(level_boundary) != std::string::npos &&
            line.find(body_needle)    != std::string::npos)
            return true;
    }
    return false;
}
} // namespace

TEST_F(HubApiTest, Log_InfoLevel_RoutesToLoggerInfo)
{
    auto api = make_api();
    api.log("info", "first info line");
    api.log("",     "second info line");        // unknown → info
    api.log("Info", "third info — capitalized");

    EXPECT_TRUE(log_file_contains(log_path(), "] [INFO ", "first info line"));
    EXPECT_TRUE(log_file_contains(log_path(), "] [INFO ", "second info line"));
    EXPECT_TRUE(log_file_contains(log_path(), "] [INFO ", "third info"));

    // Negative path: none of these should have surfaced as WARN/ERROR.
    EXPECT_FALSE(log_file_contains(log_path(), "] [WARN ",  "first info line"));
    EXPECT_FALSE(log_file_contains(log_path(), "] [ERROR ", "first info line"));
}

TEST_F(HubApiTest, Log_WarnLevel_EmitsLoggerWarn)
{
    ExpectLogWarn("heads up");        // tell LogCaptureFixture this WARN is expected
    auto api = make_api();
    api.log("warn", "heads up");

    // Affirmative: the WARN must actually be in the file.
    ASSERT_TRUE(log_file_contains(log_path(), "] [WARN ",
                                   "[hub/hub.test.uid00000001] heads up"))
        << "api.log(\"warn\", ...) must route to LOGGER_WARN";

    // Path-discrimination: must NOT have routed to INFO/ERROR.
    EXPECT_FALSE(log_file_contains(log_path(), "] [INFO ",  "heads up"));
    EXPECT_FALSE(log_file_contains(log_path(), "] [ERROR ", "heads up"));
}

TEST_F(HubApiTest, Log_WarnAliases_AllRouteToWarn)
{
    // "warn" / "Warn" / "warning" must all hit LOGGER_WARN per the
    // RoleAPIBase::log convention HubAPI mirrors.
    ExpectLogWarn("alias-test");
    auto api = make_api();
    api.log("warn",    "alias-test warn");
    api.log("Warn",    "alias-test Warn");
    api.log("warning", "alias-test warning");

    EXPECT_TRUE(log_file_contains(log_path(), "] [WARN ", "alias-test warn"));
    EXPECT_TRUE(log_file_contains(log_path(), "] [WARN ", "alias-test Warn"));
    EXPECT_TRUE(log_file_contains(log_path(), "] [WARN ", "alias-test warning"));
}

TEST_F(HubApiTest, Log_ErrorLevel_EmitsLoggerError)
{
    ExpectLogError("something failed");
    auto api = make_api();
    api.log("error", "something failed");

    ASSERT_TRUE(log_file_contains(log_path(), "] [ERROR ",
                                   "[hub/hub.test.uid00000001] something failed"))
        << "api.log(\"error\", ...) must route to LOGGER_ERROR";

    EXPECT_FALSE(log_file_contains(log_path(), "] [INFO ", "something failed"));
    EXPECT_FALSE(log_file_contains(log_path(), "] [WARN ", "something failed"));
}

TEST_F(HubApiTest, Log_DebugLevel_RoutesToLoggerDebug)
{
    auto api = make_api();
    api.log("debug", "trace-marker debug");
    api.log("Debug", "trace-marker Debug");

    // Default Logger threshold may filter DEBUG below INFO; even if
    // suppressed, the level mapping must NOT escalate to WARN/ERROR.
    EXPECT_FALSE(log_file_contains(log_path(), "] [WARN ",  "trace-marker"));
    EXPECT_FALSE(log_file_contains(log_path(), "] [ERROR ", "trace-marker"));
}

// ─── metrics() — defended path (no host backref) ─────────────────────────────

TEST_F(HubApiTest, Metrics_NoHostBackref_ReturnsEmptyObject)
{
    // Production code path always wires set_host before calling
    // metrics(); but this test verifies the defended path returns
    // an empty JSON object (NOT a nullptr-deref) when called pre-wiring.
    auto api = make_api();
    const auto m = api.metrics();
    EXPECT_TRUE(m.is_object());
    EXPECT_TRUE(m.empty()) << "metrics() with no host backref should return {}; got: "
                            << m.dump();
}

// ============================================================================
// Phase 8c — read accessors + control delegates: pre-wiring fallbacks
// ============================================================================
//
// HEP-CORE-0033 §12.3 read block + control block.  HubHost backref is
// wired by HubScriptRunner::worker_main_ immediately after EngineHost
// constructs HubAPI; before that wiring (or in unit tests that exercise
// HubAPI in isolation) the methods MUST return safe defaults rather
// than null-deref.  The defended-path contract is documented per-method
// in hub_api.hpp.

TEST_F(HubApiTest, ReadAccessors_NoHostBackref_AllReturnEmptyDefaults)
{
    auto api = make_api();

    // Empty string for the two non-JSON returners.
    EXPECT_EQ(api.name(), "");

    // Object-shaped returners produce empty objects.
    EXPECT_TRUE(api.config().is_object());
    EXPECT_TRUE(api.config().empty());

    // Array-shaped list_* returners produce empty arrays.
    EXPECT_TRUE(api.list_channels().is_array());
    EXPECT_TRUE(api.list_channels().empty());
    EXPECT_TRUE(api.list_roles().is_array());
    EXPECT_TRUE(api.list_roles().empty());
    EXPECT_TRUE(api.list_bands().is_array());
    EXPECT_TRUE(api.list_bands().empty());
    EXPECT_TRUE(api.list_peers().is_array());
    EXPECT_TRUE(api.list_peers().empty());

    // Singleton get_* returners produce JSON null.
    EXPECT_TRUE(api.get_channel("anything").is_null());
    EXPECT_TRUE(api.get_role("any.uid").is_null());
    EXPECT_TRUE(api.get_band("any-band").is_null());
    EXPECT_TRUE(api.get_peer("hub.elsewhere.uid").is_null());

    // query_metrics filtered — empty object on no-host.
    EXPECT_TRUE(api.query_metrics({}).is_object());
    EXPECT_TRUE(api.query_metrics({"channels", "broker"}).is_object());
}

TEST_F(HubApiTest, ControlDelegates_NoHostBackref_AreSilentNoOps)
{
    // No-host fallbacks must not throw or log errors — script can call
    // them at any point and the runtime is responsible for the no-op.
    auto api = make_api();
    EXPECT_NO_THROW(api.close_channel("any.channel"));
    EXPECT_NO_THROW(api.broadcast_channel("any.channel", "msg"));
    EXPECT_NO_THROW(api.broadcast_channel("any.channel", "msg", "extra-data"));
    EXPECT_NO_THROW(api.request_shutdown());
}

// ============================================================================
// Phase 8c — post_event (HEP-CORE-0033 §12.2.3)
// ============================================================================

TEST_F(HubApiTest, PostEvent_ValidName_EnqueuesAppPrefixedMessage)
{
    auto api = make_api();
    api.post_event("my_event", nlohmann::json{{"k", 42}});

    auto msgs = core_.drain_messages();
    ASSERT_EQ(msgs.size(), 1u);
    // Hub auto-prefixes with "app_" so the worker dispatches as
    // engine.invoke("on_app_<name>", details).
    EXPECT_EQ(msgs[0].event,        "app_my_event");
    EXPECT_EQ(msgs[0].sender,       "hub.test.uid00000001");  // from the fixture uid
    EXPECT_EQ(msgs[0].details.value("k", 0), 42);
}

TEST_F(HubApiTest, PostEvent_DefaultData_IsEmptyObject)
{
    auto api = make_api();
    api.post_event("minimal");

    auto msgs = core_.drain_messages();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].event, "app_minimal");
    EXPECT_TRUE(msgs[0].details.is_object());
    EXPECT_TRUE(msgs[0].details.empty());
}

TEST_F(HubApiTest, PostEvent_InvalidNames_RejectAtCallSite)
{
    auto api = make_api();

    // Empty.
    EXPECT_THROW(api.post_event(""), std::invalid_argument);
    // Leading digit.
    EXPECT_THROW(api.post_event("1foo"), std::invalid_argument);
    // Hyphen.
    EXPECT_THROW(api.post_event("foo-bar"), std::invalid_argument);
    // Dot.
    EXPECT_THROW(api.post_event("foo.bar"), std::invalid_argument);
    // Space.
    EXPECT_THROW(api.post_event("foo bar"), std::invalid_argument);
    // None of these polluted the queue.
    EXPECT_TRUE(core_.drain_messages().empty());
}

TEST_F(HubApiTest, PostEvent_ValidIdentifierShapes_AllAccepted)
{
    auto api = make_api();
    // Letter, underscore, digit (after first), CamelCase, snake_case —
    // every shape that satisfies the C-identifier grammar.
    api.post_event("a");
    api.post_event("_underscore_lead");
    api.post_event("with42digit");
    api.post_event("CamelCaseEvent");
    api.post_event("snake_case_event");

    auto msgs = core_.drain_messages();
    ASSERT_EQ(msgs.size(), 5u);
    EXPECT_EQ(msgs[0].event, "app_a");
    // Double underscore is correct: prefix is `app_`, and the
    // user-supplied name starts with `_` — concatenation gives `app__`.
    EXPECT_EQ(msgs[1].event, "app__underscore_lead");
    EXPECT_EQ(msgs[2].event, "app_with42digit");
    EXPECT_EQ(msgs[3].event, "app_CamelCaseEvent");
    EXPECT_EQ(msgs[4].event, "app_snake_case_event");
}

// ============================================================================
// Phase 8c — augment timeout (HEP-CORE-0033 §12.2.2)
// ============================================================================

TEST_F(HubApiTest, AugmentTimeout_DefaultIsHeartbeatBased)
{
    auto api = make_api();
    // Default = kDefaultAugmentTimeoutHeartbeats * kDefaultHeartbeatIntervalMs.
    // Any positive value within ~minute range is the contract; pin the
    // exact derivation rather than a magic number so changing the
    // heartbeat constants doesn't bit-rot the test.
    const int64_t expected =
        static_cast<int64_t>(pylabhub::kDefaultAugmentTimeoutHeartbeats)
        * static_cast<int64_t>(pylabhub::kDefaultHeartbeatIntervalMs);
    EXPECT_EQ(api.augment_timeout_ms(), expected);
}

TEST_F(HubApiTest, AugmentTimeout_SetGetRoundTrip_PreservesAllProjectConventionValues)
{
    auto api = make_api();

    // -1 = infinite (project convention; matches SharedSpinLock::try_lock_for)
    api.set_augment_timeout(-1);
    EXPECT_EQ(api.augment_timeout_ms(), -1);

    // 0 = non-blocking (effectively disables augmentation)
    api.set_augment_timeout(0);
    EXPECT_EQ(api.augment_timeout_ms(), 0);

    // >0 = wait N ms
    api.set_augment_timeout(5000);
    EXPECT_EQ(api.augment_timeout_ms(), 5000);

    // Large positive (one-day-ms).
    api.set_augment_timeout(86'400'000);
    EXPECT_EQ(api.augment_timeout_ms(), 86'400'000);

    // Negative-other (-2, -100): also infinite per "<0 = infinite" rule,
    // but the value is preserved verbatim — runtime treats <0 as infinite,
    // accessor doesn't normalize.
    api.set_augment_timeout(-100);
    EXPECT_EQ(api.augment_timeout_ms(), -100);
}

// ============================================================================
// Phase 8c — augment_* methods: engine routing logic
// ============================================================================
//
// MockEngine records the (callback, args, timeout_ms) triple of every
// invoke_returning call so tests pin the routing without spinning up a
// real Lua/Python engine.  All other ScriptEngine virtuals are no-ops.

namespace
{

class MockEngine : public pylabhub::scripting::ScriptEngine
{
public:
    struct Call
    {
        std::string                  callback;
        nlohmann::json               args;
        int64_t                      timeout_ms;
    };
    std::vector<Call>                     recorded;

    // Configurable response for the next invoke_returning call.
    pylabhub::scripting::InvokeStatus     next_status{
        pylabhub::scripting::InvokeStatus::Ok};
    nlohmann::json                        next_value;
    bool                                  has_callback_returns{true};

    // ── Required ScriptEngine surface ───────────────────────────
    bool init_engine_(const std::string &, pylabhub::scripting::RoleHostCore *) override { return true; }
    bool build_api_(pylabhub::scripting::RoleAPIBase &) override { return true; }
    bool build_api_(pylabhub::hub_host::HubAPI &) override { return true; }
    void finalize_engine_() override {}

    bool load_script(const std::filesystem::path &, const std::string &,
                     const std::string &) override { return true; }
    bool register_slot_type(const pylabhub::hub::SchemaSpec &,
                            const std::string &, const std::string &) override
    { return true; }
    size_t type_sizeof(const std::string &) const override { return 0; }
    bool   has_callback(const std::string &) const override { return has_callback_returns; }

    bool   invoke(const std::string &) override { return true; }
    bool   invoke(const std::string &, const nlohmann::json &) override { return true; }
    pylabhub::scripting::InvokeResponse eval(const std::string &) override
    { return {pylabhub::scripting::InvokeStatus::NotFound, {}}; }

    pylabhub::scripting::InvokeResponse
    invoke_returning(const std::string &name,
                     const nlohmann::json &args,
                     int64_t timeout_ms) override
    {
        recorded.push_back({name, args, timeout_ms});
        return {next_status, next_value};
    }

    void invoke_on_init() override {}
    void invoke_on_stop() override {}
    pylabhub::scripting::InvokeResult invoke_produce(
        pylabhub::scripting::InvokeTx,
        std::vector<pylabhub::scripting::IncomingMessage> &) override
    { return pylabhub::scripting::InvokeResult::Commit; }
    pylabhub::scripting::InvokeResult invoke_consume(
        pylabhub::scripting::InvokeRx,
        std::vector<pylabhub::scripting::IncomingMessage> &) override
    { return pylabhub::scripting::InvokeResult::Commit; }
    pylabhub::scripting::InvokeResult invoke_process(
        pylabhub::scripting::InvokeRx, pylabhub::scripting::InvokeTx,
        std::vector<pylabhub::scripting::IncomingMessage> &) override
    { return pylabhub::scripting::InvokeResult::Commit; }
    pylabhub::scripting::InvokeResult invoke_on_inbox(
        pylabhub::scripting::InvokeInbox) override
    { return pylabhub::scripting::InvokeResult::Commit; }

    uint64_t script_error_count() const noexcept override { return 0; }
    bool     supports_multi_state() const noexcept override { return false; }
};

} // namespace

TEST_F(HubApiTest, Augment_NoEngineWired_LeavesResponseUnchanged)
{
    auto api = make_api();
    nlohmann::json response = {{"default", true}};
    api.augment_query_metrics({{"params", "x"}}, response);
    EXPECT_EQ(response.value("default", false), true)
        << "with no engine wired, augment_* must be a no-op";
}

TEST_F(HubApiTest, Augment_EngineWithoutCallback_LeavesResponseUnchanged)
{
    auto api = make_api();
    MockEngine eng;
    eng.has_callback_returns = false;
    api.set_engine(eng);

    nlohmann::json response = {{"default", true}};
    api.augment_query_metrics({{"f", 1}}, response);

    EXPECT_TRUE(eng.recorded.empty())
        << "should NOT call invoke_returning when has_callback returned false";
    EXPECT_EQ(response.value("default", false), true);
}

TEST_F(HubApiTest, Augment_QueryMetrics_RoutesToEngineWithBoundTimeout)
{
    auto api = make_api();
    MockEngine eng;
    eng.next_value = nlohmann::json{{"merged", "value"}};
    api.set_engine(eng);
    api.set_augment_timeout(1234);

    nlohmann::json params   = {{"categories", nlohmann::json::array({"channels"})}};
    nlohmann::json response = {{"default", "x"}};
    api.augment_query_metrics(params, response);

    ASSERT_EQ(eng.recorded.size(), 1u);
    const auto &c = eng.recorded[0];
    EXPECT_EQ(c.callback,   "on_query_metrics");
    EXPECT_EQ(c.timeout_ms, 1234);
    // Args carry both the params we passed AND the prepared response,
    // so the script callback can mutate-and-return.
    EXPECT_EQ(c.args.value("params",   nlohmann::json{}).at("categories")[0],
              "channels");
    EXPECT_EQ(c.args.value("response", nlohmann::json{}).at("default"), "x");

    // Returned non-null value replaces the response.
    EXPECT_EQ(response.at("merged"), "value");
    EXPECT_EQ(response.contains("default"), false)
        << "non-null engine return value MUST replace the original response";
}

TEST_F(HubApiTest, Augment_NullReturn_KeepsDefaultResponse)
{
    auto api = make_api();
    MockEngine eng;
    eng.next_value = nullptr;   // script returned None / nil
    api.set_engine(eng);

    nlohmann::json response = {{"default", true}};
    api.augment_list_roles(response);

    ASSERT_EQ(eng.recorded.size(), 1u);
    EXPECT_EQ(eng.recorded[0].callback, "on_list_roles");
    EXPECT_EQ(response.value("default", false), true)
        << "null engine return value MUST keep the prepared default response";
}

TEST_F(HubApiTest, Augment_TimedOutStatus_KeepsDefaultResponse)
{
    auto api = make_api();
    MockEngine eng;
    eng.next_status = pylabhub::scripting::InvokeStatus::TimedOut;
    eng.next_value  = nlohmann::json{{"would-have-been", "ignored"}};
    api.set_engine(eng);

    nlohmann::json response = {{"default", true}};
    api.augment_query_metrics({}, response);

    ASSERT_EQ(eng.recorded.size(), 1u);
    EXPECT_EQ(response.value("default", false), true)
        << "TimedOut status MUST keep the prepared default response";
    EXPECT_EQ(response.contains("would-have-been"), false)
        << "TimedOut response value MUST NOT replace the default";
}

TEST_F(HubApiTest, Augment_ScriptErrorStatus_KeepsDefaultResponse)
{
    auto api = make_api();
    MockEngine eng;
    eng.next_status = pylabhub::scripting::InvokeStatus::ScriptError;
    api.set_engine(eng);

    nlohmann::json response = {{"default", "kept"}};
    api.augment_get_channel("ch1", response);

    ASSERT_EQ(eng.recorded.size(), 1u);
    EXPECT_EQ(eng.recorded[0].callback, "on_get_channel");
    EXPECT_EQ(eng.recorded[0].args.value("name", ""), "ch1");
    EXPECT_EQ(response.value("default", ""), "kept")
        << "ScriptError status MUST keep the default response";
}

TEST_F(HubApiTest, Augment_AllFourMethodsCarryRightCallbackName)
{
    auto api = make_api();
    MockEngine eng;
    api.set_engine(eng);

    nlohmann::json response = nlohmann::json::object();
    api.augment_query_metrics({{"x", 1}}, response);
    api.augment_list_roles(response);
    api.augment_get_channel("ch", response);
    api.augment_peer_message("hub.peer.uid", {{"k", "v"}}, response);

    ASSERT_EQ(eng.recorded.size(), 4u);
    EXPECT_EQ(eng.recorded[0].callback, "on_query_metrics");
    EXPECT_EQ(eng.recorded[1].callback, "on_list_roles");
    EXPECT_EQ(eng.recorded[2].callback, "on_get_channel");
    EXPECT_EQ(eng.recorded[3].callback, "on_peer_message");

    // Args content discrimination: each method packages its inputs
    // under the documented key (params/response, name/response,
    // peer_uid/msg/response).
    EXPECT_TRUE(eng.recorded[2].args.contains("name"));
    EXPECT_TRUE(eng.recorded[3].args.contains("peer_uid"));
    EXPECT_TRUE(eng.recorded[3].args.contains("msg"));
}
