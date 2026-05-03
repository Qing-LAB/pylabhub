#pragma once
/**
 * @file log_capture_fixture.h
 * @brief Captures Logger output during a test and asserts no unexpected
 *        WARN / ERROR entries were emitted.
 *
 * Mandatory for happy-path tests per `docs/IMPLEMENTATION_GUIDANCE.md` §
 * "Tests must verify the absence of unexpected log warnings/errors".
 *
 * Mechanism: the fixture installs a per-test temp log file (via
 * `Logger::set_logfile`) at SetUp, drives the test against that file,
 * and on TearDown calls `Logger::flush()` then scans the file for
 * `[WARN]` / `[ERROR]` markers.  Any entry whose body does not match
 * a registered "expected" substring is reported via `ADD_FAILURE`.
 *
 * Usage:
 *
 * @code
 * class MyTest : public ::testing::Test, public LogCaptureFixture {
 *   protected:
 *     void SetUp()    override { LogCaptureFixture::Install(); }
 *     void TearDown() override {
 *         AssertNoUnexpectedLogWarnError();
 *         LogCaptureFixture::Uninstall();
 *     }
 * };
 *
 * TEST_F(MyTest, ErrorPath_LogsAsExpected)
 * {
 *     ExpectLogWarn("inbox_schema_json invalid");  // declare up-front
 *     run_action_that_warns();                     // produces the warn
 * }
 * @endcode
 *
 * Limitations:
 *   - Async logger.  `flush()` waits for the worker queue to drain;
 *     captures are reliable as long as no thread emits AFTER flush()
 *     returns and BEFORE the file is read.  Tests that race the
 *     logger thread should join their workers first.
 *   - Single-active-sink Logger.  Install() switches the sink to the
 *     temp file for the duration of the test; the previous sink (if
 *     any) is NOT restored automatically because Logger has no
 *     get-current-sink accessor.  Uninstall() reverts to console.
 *     Tests that need a different sink must coordinate.
 */

#include "utils/logger.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pylabhub::tests
{

class LogCaptureFixture
{
public:
    /// Install per-test capture.  Switches Logger to a temp file under
    /// `/tmp/plh_logcap_<pid>_<seq>.log`.  Idempotent on re-Install
    /// within the same fixture (rotates to a fresh file).
    void Install()
    {
        std::error_code ec;
        std::filesystem::remove(log_path_, ec);
        log_path_ = next_log_path_();
        ASSERT_TRUE(::pylabhub::utils::Logger::instance()
                        .set_logfile(log_path_.string()))
            << "LogCaptureFixture: set_logfile failed for " << log_path_;
        expected_warns_.clear();
        expected_errors_.clear();
        must_fire_warns_.clear();
        must_fire_errors_.clear();
    }

    /// Stop capture, revert Logger to console, remove the temp file.
    void Uninstall()
    {
        ::pylabhub::utils::Logger::instance().flush();
        // Best effort: switch back to console so subsequent tests in
        // the same suite without LogCaptureFixture do not write into
        // a stale file.
        (void) ::pylabhub::utils::Logger::instance().set_console();
        std::error_code ec;
        std::filesystem::remove(log_path_, ec);
    }

    /// Declare an expected WARN substring (PERMISSIVE allowlist).
    /// Captured WARN entries containing this substring will not count
    /// as failures.  No requirement that the WARN ACTUALLY fire — if
    /// no matching WARN appears, the expectation is silently
    /// unfulfilled.  Use this for warns that MAY-OR-MAY-NOT fire
    /// depending on race outcome (timing-dependent paths, optional
    /// fallbacks).  For warns that MUST fire — use
    /// `ExpectLogWarnMustFire` instead.
    void ExpectLogWarn(std::string substring)
    {
        expected_warns_.push_back(std::move(substring));
    }

    /// Declare an expected ERROR substring (PERMISSIVE allowlist).
    /// Same semantics as ExpectLogWarn but for ERROR-level entries.
    void ExpectLogError(std::string substring)
    {
        expected_errors_.push_back(std::move(substring));
    }

    /// Declare an expected WARN substring (STRICT must-fire).  At
    /// least one captured WARN line must contain this substring;
    /// `AssertNoUnexpectedLogWarnError` fails if no match is found.
    /// Implies the permissive allowlist behavior — matching lines
    /// won't be flagged as unexpected.
    ///
    /// Use for tests that GATE on the warn actually firing — level-
    /// routing tests, error-path discrimination, regression guards
    /// for warns that document a real production-emitted condition.
    /// Per audit §1.1 Class A: outcome-only assertions silently
    /// accept regressions that take the wrong path; affirming the
    /// warn fired is the path-pinning equivalent at the log boundary.
    void ExpectLogWarnMustFire(std::string substring)
    {
        expected_warns_.push_back(substring);
        must_fire_warns_.push_back({std::move(substring), false});
    }

    /// Declare an expected ERROR substring (STRICT must-fire).
    /// Same semantics as ExpectLogWarnMustFire but for ERROR-level.
    void ExpectLogErrorMustFire(std::string substring)
    {
        expected_errors_.push_back(substring);
        must_fire_errors_.push_back({std::move(substring), false});
    }

    /// Path to the per-test capture file.  Exposed so tests can
    /// affirmatively verify a WARN/ERROR / INFO line was emitted —
    /// `ExpectLogWarn`/`Error` only DECLARE an allowlist; they don't
    /// enforce the warning actually fired.  For tests that need to
    /// gate on emission (per audit §1.1 Class A path-discrimination)
    /// open this file and grep for the expected substring directly.
    [[nodiscard]] const std::filesystem::path &log_path() const noexcept
    {
        return log_path_;
    }

    /// Read the capture file.  Two failure modes:
    ///   1. **Unexpected** WARN/ERROR — any line not matching a
    ///      previously-declared `ExpectLog*` entry (permissive or
    ///      strict).  Same behavior as before.
    ///   2. **Unfulfilled must-fire** — any `ExpectLogWarnMustFire` /
    ///      `ExpectLogErrorMustFire` declaration that matched zero
    ///      lines.  Catches the audit Class A gap where the test
    ///      declared an expected warn but production code stopped
    ///      emitting it.
    /// Call this from `TearDown` (or directly at the end of a test).
    void AssertNoUnexpectedLogWarnError()
    {
        ::pylabhub::utils::Logger::instance().flush();

        // Reset must-fire match flags — calls to AssertNoUnexpected
        // are not expected to be repeated within one test, but if
        // they are, each scan starts from a fresh accounting.
        for (auto &m : must_fire_warns_)  m.matched = false;
        for (auto &m : must_fire_errors_) m.matched = false;

        std::ifstream f(log_path_);
        if (f.is_open())
        {
            // Logger format: `[LOGGER] [WARN  ] [time] [PID:... TID:...] body`
            // (level is left-padded to 6 chars via `{:<6}` in
            // logger_sinks/sink.cpp::format_logmsg).  Match the boundary
            // `] [WARN ` / `] [ERROR ` so we can't false-match on bodies
            // that happen to contain the substring "WARN" or "ERROR".
            std::string line;
            while (std::getline(f, line))
            {
                const bool is_warn  = line.find("] [WARN ")  != std::string::npos;
                const bool is_error = line.find("] [ERROR ") != std::string::npos;
                if (!is_warn && !is_error)
                    continue;

                const auto &expects = is_error ? expected_errors_ : expected_warns_;
                bool matched = false;
                for (const auto &needle : expects)
                {
                    if (line.find(needle) != std::string::npos) { matched = true; break; }
                }
                if (!matched)
                {
                    ADD_FAILURE() << "LogCaptureFixture: unexpected "
                                  << (is_error ? "ERROR" : "WARN")
                                  << " log line — declare it via Expect"
                                  << (is_error ? "LogError" : "LogWarn")
                                  << "() if intended.\n  line: " << line;
                }
                // Mark must-fire matches.  A line can match at most
                // one must-fire substring (first-match-wins) so a
                // single emission won't satisfy two distinct
                // declarations.  Permissive allowlist consumption is
                // independent of must-fire match accounting.
                auto &mf = is_error ? must_fire_errors_ : must_fire_warns_;
                for (auto &m : mf)
                {
                    if (!m.matched && line.find(m.substring) != std::string::npos)
                    {
                        m.matched = true;
                        break;
                    }
                }
            }
        }
        // Fail for any must-fire substring that didn't match.  Empty
        // file = no matches = all must-fires unfulfilled (correct).
        for (const auto &m : must_fire_warns_)
        {
            if (!m.matched)
                ADD_FAILURE() << "LogCaptureFixture: ExpectLogWarnMustFire(\""
                              << m.substring << "\") was declared but no "
                              "matching WARN line was emitted.  Either the "
                              "production code path that should have emitted "
                              "the warn was not exercised, or the warn was "
                              "silently dropped (regression).";
        }
        for (const auto &m : must_fire_errors_)
        {
            if (!m.matched)
                ADD_FAILURE() << "LogCaptureFixture: ExpectLogErrorMustFire(\""
                              << m.substring << "\") was declared but no "
                              "matching ERROR line was emitted.";
        }
    }

private:
    static std::filesystem::path next_log_path_()
    {
        static std::atomic<int> seq{0};
        const int n = seq.fetch_add(1, std::memory_order_relaxed);
        return std::filesystem::temp_directory_path() /
               ("plh_logcap_" + std::to_string(::getpid()) + "_" +
                std::to_string(n) + ".log");
    }

    std::filesystem::path     log_path_;
    std::vector<std::string>  expected_warns_;
    std::vector<std::string>  expected_errors_;

    /// Strict must-fire declarations.  Each entry tracks whether at
    /// least one captured WARN/ERROR line matched its substring.
    /// Reset to false at AssertNoUnexpectedLogWarnError() entry so
    /// a single test calling it multiple times gets fresh accounting
    /// each time (rare; typical usage is one call from TearDown).
    struct MustFireEntry { std::string substring; bool matched; };
    std::vector<MustFireEntry> must_fire_warns_;
    std::vector<MustFireEntry> must_fire_errors_;
};

} // namespace pylabhub::tests
