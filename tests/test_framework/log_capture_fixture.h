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

    /// Declare an expected WARN substring.  Captured WARN entries
    /// containing this substring will not count as failures.  Multiple
    /// expectations are OR-combined.
    void ExpectLogWarn(std::string substring)
    {
        expected_warns_.push_back(std::move(substring));
    }

    /// Declare an expected ERROR substring (same semantics as
    /// ExpectLogWarn but for ERROR-level entries).
    void ExpectLogError(std::string substring)
    {
        expected_errors_.push_back(std::move(substring));
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

    /// Read the capture file, fail the test for every WARN or ERROR
    /// line that does not match a previously-declared expectation.
    /// Call this from `TearDown` (or directly at the end of a test).
    void AssertNoUnexpectedLogWarnError()
    {
        ::pylabhub::utils::Logger::instance().flush();
        std::ifstream f(log_path_);
        if (!f.is_open())
            return;  // No file -> no entries; treat as empty.

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
};

} // namespace pylabhub::tests
