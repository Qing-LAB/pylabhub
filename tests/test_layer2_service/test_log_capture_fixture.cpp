/**
 * @file test_log_capture_fixture.cpp
 * @brief Tests for the test framework's LogCaptureFixture itself.
 *
 * Verifies the four user-facing methods:
 *   - `ExpectLogWarn`             — permissive allowlist for WARN
 *   - `ExpectLogError`            — permissive allowlist for ERROR
 *   - `ExpectLogWarnMustFire`     — strict must-fire for WARN
 *   - `ExpectLogErrorMustFire`    — strict must-fire for ERROR
 * plus `AssertNoUnexpectedLogWarnError`'s two failure modes.
 *
 * The "fixture should fail the test" cases use gtest's
 * `EXPECT_NONFATAL_FAILURE` macro which captures an inner ADD_FAILURE
 * without propagating it as an outer test failure — exactly the
 * mechanism gtest provides for testing assertion-emitting code.
 */

#include "log_capture_fixture.h"
#include "plh_service.hpp"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>   // EXPECT_NONFATAL_FAILURE

#include <memory>

using pylabhub::utils::Logger;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::MakeModDefList;
using pylabhub::tests::LogCaptureFixture;

class LogCaptureFixtureTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(
            MakeModDefList(Logger::GetLifecycleModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    /// We use `LogCaptureFixture` directly as an aggregated member here,
    /// not as a base class — so we can call its methods explicitly and
    /// observe the failures it reports without those failures bubbling
    /// out as outer test failures.
    LogCaptureFixture cap_;

    void SetUp()    override { cap_.Install(); }
    void TearDown() override
    {
        // We DO NOT call cap_.AssertNoUnexpectedLogWarnError() here —
        // each test calls it explicitly inside an EXPECT_NONFATAL_FAILURE
        // when it expects the assertion to fail, or directly when it
        // expects success.
        cap_.Uninstall();
    }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> LogCaptureFixtureTest::s_lifecycle_;

// ─── Permissive allowlist (existing behavior) ────────────────────────────────

TEST_F(LogCaptureFixtureTest, ExpectLogWarn_PermissiveAllows_NoFail)
{
    cap_.ExpectLogWarn("expected-warn-substring");
    LOGGER_WARN("[test] expected-warn-substring fired");
    cap_.AssertNoUnexpectedLogWarnError();   // must not fail
}

TEST_F(LogCaptureFixtureTest, ExpectLogWarn_PermissiveSilentlyOK_WhenWarnDoesNotFire)
{
    // Critical legacy contract: declaring an expectation that does NOT
    // get fulfilled is silently OK.  Tests that need strict enforcement
    // must use ExpectLogWarnMustFire (verified below).
    cap_.ExpectLogWarn("expected-but-never-fired");
    cap_.AssertNoUnexpectedLogWarnError();   // must not fail
}

TEST_F(LogCaptureFixtureTest, ExpectLogWarn_UndeclaredWarn_Fails)
{
    LOGGER_WARN("[test] this WARN was not declared");
    EXPECT_NONFATAL_FAILURE(
        cap_.AssertNoUnexpectedLogWarnError(),
        "unexpected WARN");
}

TEST_F(LogCaptureFixtureTest, ExpectLogError_UndeclaredError_Fails)
{
    LOGGER_ERROR("[test] this ERROR was not declared");
    EXPECT_NONFATAL_FAILURE(
        cap_.AssertNoUnexpectedLogWarnError(),
        "unexpected ERROR");
}

// ─── Strict must-fire (new behavior — Phase 7 Commit D1.5) ───────────────────

TEST_F(LogCaptureFixtureTest, MustFireWarn_Fired_NoFail)
{
    cap_.ExpectLogWarnMustFire("strict-warn-needle");
    LOGGER_WARN("[test] strict-warn-needle fired exactly once");
    cap_.AssertNoUnexpectedLogWarnError();   // must not fail
}

TEST_F(LogCaptureFixtureTest, MustFireWarn_NotFired_Fails)
{
    cap_.ExpectLogWarnMustFire("never-emitted-needle");
    EXPECT_NONFATAL_FAILURE(
        cap_.AssertNoUnexpectedLogWarnError(),
        "ExpectLogWarnMustFire");
}

TEST_F(LogCaptureFixtureTest, MustFireError_Fired_NoFail)
{
    cap_.ExpectLogErrorMustFire("strict-error-needle");
    LOGGER_ERROR("[test] strict-error-needle fired");
    cap_.AssertNoUnexpectedLogWarnError();   // must not fail
}

TEST_F(LogCaptureFixtureTest, MustFireError_NotFired_Fails)
{
    cap_.ExpectLogErrorMustFire("never-emitted-error");
    EXPECT_NONFATAL_FAILURE(
        cap_.AssertNoUnexpectedLogWarnError(),
        "ExpectLogErrorMustFire");
}

TEST_F(LogCaptureFixtureTest, MustFireWarn_AlsoSatisfiesAllowlist)
{
    // Strict must-fire IMPLIES permissive allowlist — a matching WARN
    // line must not also be flagged as unexpected.
    cap_.ExpectLogWarnMustFire("dual-purpose-needle");
    LOGGER_WARN("[test] dual-purpose-needle line");
    // Must pass cleanly: must-fire matched + line was allowlisted.
    cap_.AssertNoUnexpectedLogWarnError();
}

TEST_F(LogCaptureFixtureTest, MustFire_MultipleEmissionsCountAsOneMatch)
{
    // Two WARN lines containing the same needle.  Must-fire is
    // satisfied (>= 1 match); both lines are allowlisted; no failure.
    cap_.ExpectLogWarnMustFire("multi-emit");
    LOGGER_WARN("[test] multi-emit first occurrence");
    LOGGER_WARN("[test] multi-emit second occurrence");
    cap_.AssertNoUnexpectedLogWarnError();
}

TEST_F(LogCaptureFixtureTest, MustFire_TwoDistinctNeedlesBothMustMatch)
{
    cap_.ExpectLogWarnMustFire("alpha");
    cap_.ExpectLogWarnMustFire("beta");
    LOGGER_WARN("[test] alpha-only");
    EXPECT_NONFATAL_FAILURE(
        cap_.AssertNoUnexpectedLogWarnError(),
        "beta");   // beta still unmatched
}

TEST_F(LogCaptureFixtureTest, MustFire_TwoDistinctNeedlesBothFire_NoFail)
{
    cap_.ExpectLogWarnMustFire("alpha");
    cap_.ExpectLogWarnMustFire("beta");
    LOGGER_WARN("[test] alpha line");
    LOGGER_WARN("[test] beta line");
    cap_.AssertNoUnexpectedLogWarnError();
}

TEST_F(LogCaptureFixtureTest, MustFire_DistinctNeedlesEachConsumeOneLine)
{
    // Two needles, two lines, each line matches exactly one needle.
    // Must-fire accounting: each declaration matches first time it
    // sees its needle; subsequent matches are no-ops on must-fire
    // (already-matched) but still consumed by allowlist.
    cap_.ExpectLogWarnMustFire("apple");
    cap_.ExpectLogWarnMustFire("orange");
    LOGGER_WARN("[test] eating apple");
    LOGGER_WARN("[test] eating orange");
    cap_.AssertNoUnexpectedLogWarnError();
}
