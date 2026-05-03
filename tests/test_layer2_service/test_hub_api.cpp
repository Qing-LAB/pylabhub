/**
 * @file test_hub_api.cpp
 * @brief HubAPI unit tests — Phase 7 Commit D1.
 *
 * Tests the script-visible API surface in isolation, without spinning
 * up HubHost or any script engine.  Construction, log(), and uid() are
 * fully testable here; metrics() requires a wired HubHost backref +
 * started broker and is exercised by Phase 7 Commit D L3 integration
 * tests instead.
 */

#include "utils/hub_api.hpp"

#include "utils/role_host_core.hpp"
#include "plh_service.hpp"
#include "log_capture_fixture.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>

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
