/**
 * @file test_log_capture_fixture.cpp
 * @brief Pattern 3 driver — `LogCaptureFixture` self-tests.
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/log_capture_fixture_workers.cpp`.
 *
 * Subject under test is `pylabhub::tests::LogCaptureFixture` itself —
 * the framework helper that wave's other Pattern-3 workers use.
 * Each worker body owns its own `LogCaptureFixture` instance (no
 * outer wrap) because two LogCaptureFixture instances cannot
 * coexist: both redirect the singleton Logger sink, and the second
 * Install() would orphan the first.  See the worker file's header
 * for the full rationale.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class LogCaptureFixtureTest : public IsolatedProcessTest
{
};

// ─── Permissive allowlist ───────────────────────────────────────────────────

TEST_F(LogCaptureFixtureTest, ExpectLogWarn_PermissiveAllows_NoFail)
{
    auto w = SpawnWorker("log_capture_fixture.expect_log_warn_permissive_allows_no_fail");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, ExpectLogWarn_PermissiveSilentlyOK_WhenWarnDoesNotFire)
{
    auto w = SpawnWorker("log_capture_fixture.expect_log_warn_permissive_silently_ok_"
                         "when_warn_does_not_fire");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, ExpectLogWarn_UndeclaredWarn_Fails)
{
    auto w = SpawnWorker("log_capture_fixture.expect_log_warn_undeclared_warn_fails");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, ExpectLogError_UndeclaredError_Fails)
{
    auto w = SpawnWorker("log_capture_fixture.expect_log_error_undeclared_error_fails");
    ExpectWorkerOk(w);
}

// ─── Strict must-fire (Phase 7 Commit D1.5) ─────────────────────────────────

TEST_F(LogCaptureFixtureTest, MustFireWarn_Fired_NoFail)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_warn_fired_no_fail");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFireWarn_NotFired_Fails)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_warn_not_fired_fails");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFireError_Fired_NoFail)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_error_fired_no_fail");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFireError_NotFired_Fails)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_error_not_fired_fails");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFireWarn_AlsoSatisfiesAllowlist)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_warn_also_satisfies_allowlist");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFire_MultipleEmissionsCountAsOneMatch)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_multiple_emissions_count_as_one_match");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFire_TwoDistinctNeedlesBothMustMatch)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_two_distinct_needles_both_must_match");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFire_TwoDistinctNeedlesBothFire_NoFail)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_two_distinct_needles_both_fire_no_fail");
    ExpectWorkerOk(w);
}

TEST_F(LogCaptureFixtureTest, MustFire_DistinctNeedlesEachConsumeOneLine)
{
    auto w = SpawnWorker("log_capture_fixture.must_fire_distinct_needles_each_consume_one_line");
    ExpectWorkerOk(w);
}
