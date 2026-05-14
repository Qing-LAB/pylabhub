/**
 * @file log_capture_fixture_workers.cpp
 * @brief Worker bodies for `LogCaptureFixture` self-tests (Pattern 3).
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.
 *
 * ─────────────────────────────────────────────────────────────────
 * Deviation from wave default worker shape — INTENTIONAL.
 * ─────────────────────────────────────────────────────────────────
 *
 * Other workers in the wave follow this prologue/epilogue:
 *
 *     LogCaptureFixture log_cap;
 *     log_cap.Install();
 *     // ... body ...
 *     log_cap.AssertNoUnexpectedLogWarnError();
 *     log_cap.Uninstall();
 *
 * That pattern WOULD BREAK this file because the **subject under
 * test IS LogCaptureFixture itself**.  The test body constructs its
 * own `LogCaptureFixture cap`, calls `cap.Install()`, and probes
 * cap's contract via `EXPECT_NONFATAL_FAILURE(cap.AssertNoUnexpected
 * LogWarnError(), …)`.  An outer wrapper `log_cap` would call
 * `log_cap.Install()` first, redirecting Logger's singleton sink to
 * its file; then the inner `cap.Install()` would re-redirect to a
 * DIFFERENT file, orphaning the outer.  The outer's
 * `AssertNoUnexpectedLogWarnError()` reads from its now-empty file
 * and trivially passes — providing no signal at all, while masking
 * any real warns the inner cap missed.
 *
 * Correct shape: worker body owns LogCaptureFixture exclusively.
 * No outer wrap.
 *
 * ─────────────────────────────────────────────────────────────────
 * Second deviation: try/catch INSTEAD of `EXPECT_NONFATAL_FAILURE`.
 * ─────────────────────────────────────────────────────────────────
 *
 * The original in-process tests used gtest-spi's
 * `EXPECT_NONFATAL_FAILURE` to verify that the inner code
 * (`cap.AssertNoUnexpectedLogWarnError()`) emits the expected
 * failure.  That macro works by installing a `TestPartResultReporter`
 * that captures non-fatal failures emitted via the reporter chain.
 *
 * `run_gtest_worker` sets `throw_on_failure=true` so that any
 * `ADD_FAILURE()` / `EXPECT_*`/`ASSERT_*` failure throws
 * `GoogleTestFailureException` instead of going through the reporter
 * chain (silent-shortcircuit guard — see shared_test_helpers.h:241-246).
 * Under that flag, the reporter chain is bypassed entirely, so
 * `EXPECT_NONFATAL_FAILURE` cannot capture the inner failure —
 * the exception escapes the macro and lands in `run_gtest_worker`'s
 * catch block, marking the worker failed.
 *
 * Rewriting to a plain try/catch is the idiomatic pattern in a
 * `throw_on_failure=true` environment: catch the
 * `GoogleTestFailureException`, inspect `e.what()` for the expected
 * substring, and `EXPECT_TRUE(threw)` + `EXPECT_NE(... find(needle))`
 * pin the same contract (failure-must-fire + substring-pin).  The
 * what() string includes the full LogCaptureFixture failure message
 * including the line text, so the substring assertion is equivalent.
 *
 * Module surface: Logger only (matches the original
 * SetUpTestSuite).  Smallest module list in the wave.
 *
 * @see tests/test_framework/log_capture_fixture.h  (subject under test)
 */

#include "log_capture_fixture_workers.h"

#include "log_capture_fixture.h"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/logger.hpp"

#include <gtest/gtest.h>

#include <exception>
#include <string>
#include <string_view>

using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace log_capture_fixture
{

// ─── Permissive allowlist ───────────────────────────────────────────────────

int expect_log_warn_permissive_allows_no_fail()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            cap.ExpectLogWarn("expected-warn-substring");
            LOGGER_WARN("[test] expected-warn-substring fired");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::expect_log_warn_permissive_allows_no_fail",
        Logger::GetLifecycleModule());
}

int expect_log_warn_permissive_silently_ok_when_warn_does_not_fire()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            // Critical legacy contract: a permissive declaration that
            // does NOT get fulfilled is silently OK.  Tests that need
            // strict enforcement must use ExpectLogWarnMustFire (below).
            cap.ExpectLogWarn("expected-but-never-fired");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::expect_log_warn_permissive_silently_ok_"
        "when_warn_does_not_fire",
        Logger::GetLifecycleModule());
}

int expect_log_warn_undeclared_warn_fails()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            LOGGER_WARN("[test] this WARN was not declared");
            bool threw = false;
            std::string msg;
            try
            {
                cap.AssertNoUnexpectedLogWarnError();
            }
            catch (const ::testing::internal::GoogleTestFailureException &e)
            {
                threw = true;
                msg = e.what();
            }
            EXPECT_TRUE(threw)
                << "AssertNoUnexpectedLogWarnError() should have thrown "
                   "on an undeclared WARN line";
            EXPECT_NE(msg.find("unexpected WARN"), std::string::npos)
                << "failure message did not pin 'unexpected WARN'; got:\n"
                << msg;
            cap.Uninstall();
        },
        "log_capture_fixture::expect_log_warn_undeclared_warn_fails",
        Logger::GetLifecycleModule());
}

int expect_log_error_undeclared_error_fails()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            LOGGER_ERROR("[test] this ERROR was not declared");
            bool threw = false;
            std::string msg;
            try
            {
                cap.AssertNoUnexpectedLogWarnError();
            }
            catch (const ::testing::internal::GoogleTestFailureException &e)
            {
                threw = true;
                msg = e.what();
            }
            EXPECT_TRUE(threw)
                << "AssertNoUnexpectedLogWarnError() should have thrown "
                   "on an undeclared ERROR line";
            EXPECT_NE(msg.find("unexpected ERROR"), std::string::npos)
                << "failure message did not pin 'unexpected ERROR'; got:\n"
                << msg;
            cap.Uninstall();
        },
        "log_capture_fixture::expect_log_error_undeclared_error_fails",
        Logger::GetLifecycleModule());
}

// ─── Strict must-fire (Phase 7 Commit D1.5) ─────────────────────────────────

int must_fire_warn_fired_no_fail()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            cap.ExpectLogWarnMustFire("strict-warn-needle");
            LOGGER_WARN("[test] strict-warn-needle fired exactly once");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_warn_fired_no_fail",
        Logger::GetLifecycleModule());
}

int must_fire_warn_not_fired_fails()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            cap.ExpectLogWarnMustFire("never-emitted-needle");
            bool threw = false;
            std::string msg;
            try
            {
                cap.AssertNoUnexpectedLogWarnError();
            }
            catch (const ::testing::internal::GoogleTestFailureException &e)
            {
                threw = true;
                msg = e.what();
            }
            EXPECT_TRUE(threw)
                << "AssertNoUnexpectedLogWarnError() should have thrown "
                   "on an unfulfilled must-fire WARN declaration";
            EXPECT_NE(msg.find("ExpectLogWarnMustFire"), std::string::npos)
                << "failure message did not pin "
                   "'ExpectLogWarnMustFire'; got:\n" << msg;
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_warn_not_fired_fails",
        Logger::GetLifecycleModule());
}

int must_fire_error_fired_no_fail()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            cap.ExpectLogErrorMustFire("strict-error-needle");
            LOGGER_ERROR("[test] strict-error-needle fired");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_error_fired_no_fail",
        Logger::GetLifecycleModule());
}

int must_fire_error_not_fired_fails()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            cap.ExpectLogErrorMustFire("never-emitted-error");
            bool threw = false;
            std::string msg;
            try
            {
                cap.AssertNoUnexpectedLogWarnError();
            }
            catch (const ::testing::internal::GoogleTestFailureException &e)
            {
                threw = true;
                msg = e.what();
            }
            EXPECT_TRUE(threw)
                << "AssertNoUnexpectedLogWarnError() should have thrown "
                   "on an unfulfilled must-fire ERROR declaration";
            EXPECT_NE(msg.find("ExpectLogErrorMustFire"), std::string::npos)
                << "failure message did not pin "
                   "'ExpectLogErrorMustFire'; got:\n" << msg;
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_error_not_fired_fails",
        Logger::GetLifecycleModule());
}

int must_fire_warn_also_satisfies_allowlist()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            // Strict must-fire IMPLIES permissive allowlist — a matching
            // WARN line must not also be flagged as unexpected.
            cap.ExpectLogWarnMustFire("dual-purpose-needle");
            LOGGER_WARN("[test] dual-purpose-needle line");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_warn_also_satisfies_allowlist",
        Logger::GetLifecycleModule());
}

int must_fire_multiple_emissions_count_as_one_match()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            // Two WARN lines containing the same needle.  Must-fire is
            // satisfied (>= 1 match); both lines are allowlisted; no
            // failure.
            cap.ExpectLogWarnMustFire("multi-emit");
            LOGGER_WARN("[test] multi-emit first occurrence");
            LOGGER_WARN("[test] multi-emit second occurrence");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_multiple_emissions_count_as_one_match",
        Logger::GetLifecycleModule());
}

int must_fire_two_distinct_needles_both_must_match()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            cap.ExpectLogWarnMustFire("alpha");
            cap.ExpectLogWarnMustFire("beta");
            LOGGER_WARN("[test] alpha-only");
            bool threw = false;
            std::string msg;
            try
            {
                cap.AssertNoUnexpectedLogWarnError();
            }
            catch (const ::testing::internal::GoogleTestFailureException &e)
            {
                threw = true;
                msg = e.what();
            }
            EXPECT_TRUE(threw)
                << "AssertNoUnexpectedLogWarnError() should have thrown "
                   "because 'beta' must-fire never matched";
            EXPECT_NE(msg.find("beta"), std::string::npos)
                << "failure message did not name the unmatched needle "
                   "'beta'; got:\n" << msg;
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_two_distinct_needles_both_must_match",
        Logger::GetLifecycleModule());
}

int must_fire_two_distinct_needles_both_fire_no_fail()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            cap.ExpectLogWarnMustFire("alpha");
            cap.ExpectLogWarnMustFire("beta");
            LOGGER_WARN("[test] alpha line");
            LOGGER_WARN("[test] beta line");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_two_distinct_needles_both_fire_no_fail",
        Logger::GetLifecycleModule());
}

int must_fire_distinct_needles_each_consume_one_line()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture cap;
            cap.Install();
            // Two needles, two lines, each line matches exactly one
            // needle.  Must-fire accounting: each declaration matches
            // first time it sees its needle; subsequent matches are
            // no-ops on must-fire (already-matched) but still consumed
            // by allowlist.
            cap.ExpectLogWarnMustFire("apple");
            cap.ExpectLogWarnMustFire("orange");
            LOGGER_WARN("[test] eating apple");
            LOGGER_WARN("[test] eating orange");
            cap.AssertNoUnexpectedLogWarnError();
            cap.Uninstall();
        },
        "log_capture_fixture::must_fire_distinct_needles_each_consume_one_line",
        Logger::GetLifecycleModule());
}

} // namespace log_capture_fixture
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct LogCaptureFixtureRegistrar
{
    LogCaptureFixtureRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "log_capture_fixture")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::log_capture_fixture;

                if (sc == "expect_log_warn_permissive_allows_no_fail")
                    return expect_log_warn_permissive_allows_no_fail();
                if (sc == "expect_log_warn_permissive_silently_ok_when_"
                          "warn_does_not_fire")
                    return expect_log_warn_permissive_silently_ok_when_warn_does_not_fire();
                if (sc == "expect_log_warn_undeclared_warn_fails")
                    return expect_log_warn_undeclared_warn_fails();
                if (sc == "expect_log_error_undeclared_error_fails")
                    return expect_log_error_undeclared_error_fails();
                if (sc == "must_fire_warn_fired_no_fail")
                    return must_fire_warn_fired_no_fail();
                if (sc == "must_fire_warn_not_fired_fails")
                    return must_fire_warn_not_fired_fails();
                if (sc == "must_fire_error_fired_no_fail")
                    return must_fire_error_fired_no_fail();
                if (sc == "must_fire_error_not_fired_fails")
                    return must_fire_error_not_fired_fails();
                if (sc == "must_fire_warn_also_satisfies_allowlist")
                    return must_fire_warn_also_satisfies_allowlist();
                if (sc == "must_fire_multiple_emissions_count_as_one_match")
                    return must_fire_multiple_emissions_count_as_one_match();
                if (sc == "must_fire_two_distinct_needles_both_must_match")
                    return must_fire_two_distinct_needles_both_must_match();
                if (sc == "must_fire_two_distinct_needles_both_fire_no_fail")
                    return must_fire_two_distinct_needles_both_fire_no_fail();
                if (sc == "must_fire_distinct_needles_each_consume_one_line")
                    return must_fire_distinct_needles_each_consume_one_line();
                return -1;
            });
    }
} g_registrar;

} // namespace
