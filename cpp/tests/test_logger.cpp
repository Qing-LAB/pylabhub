// tests/test_logger.cpp
//
// Test harness for the Logger.
// Each test case is executed in a separate worker process to ensure full
// isolation of the lifecycle-managed, singleton-based Logger.

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

#include "helpers/test_entrypoint.h"
#include "helpers/test_process_utils.h"
#include "platform.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;
using namespace test_utils;

namespace {

// Waits for a worker process to complete and returns its exit code.
// Handles platform-specific process management.
int wait_for_worker_and_get_exit_code(ProcessHandle handle)
{
#if defined(PLATFORM_WIN64)
    if (handle == NULL_PROC_HANDLE) return -1;
    // Wait for a reasonable time, not infinite, to prevent hangs.
    if (WaitForSingleObject(handle, 60000) == WAIT_TIMEOUT)
    {
        // If it times out, terminate it to prevent a hanging process.
        TerminateProcess(handle, 99);
        CloseHandle(handle);
        return -99; // Special exit code for timeout
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(handle, &exit_code);
    CloseHandle(handle);
    return static_cast<int>(exit_code);
#else
    if (handle <= 0) return -1;
    int status = 0;
    // A more robust wait might include a timeout mechanism here as well.
    waitpid(handle, &status, 0);
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1; // Indicate failure if not exited normally.
#endif
}

} // namespace

// The new test fixture is much simpler. It no longer manages the application
// lifecycle, as each test runs in a fresh process. It only manages the cleanup
// of temporary files created during the tests.
class LoggerTest : public ::testing::Test {
protected:
    std::vector<fs::path> paths_to_clean_;

    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            try
            {
                if (fs::exists(p)) fs::remove(p);
            }
            catch (...)
            {
                // best-effort cleanup
            }
        }
    }

    fs::path GetUniqueLogPath(const std::string &test_name)
    {
        auto p = fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
        paths_to_clean_.push_back(p);
        // Ensure the file does not exist from a previous failed run.
        try
        {
            if (fs::exists(p)) fs::remove(p);
        }
        catch (...)
        {
        }
        return p;
    }
};

// --- Test Implementation ---
// Each test now spawns a worker process to run the actual test logic.
// The main test process only asserts that the worker exited successfully.

TEST_F(LoggerTest, BasicLogging)
{
    auto log_path = GetUniqueLogPath("basic_logging");
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "logger.test_basic_logging", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, LogLevelFiltering)
{
    auto log_path = GetUniqueLogPath("log_level_filtering");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_log_level_filtering",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, BadFormatString)
{
    auto log_path = GetUniqueLogPath("bad_format_string");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_bad_format_string",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, DefaultSinkAndSwitching)
{
    auto log_path = GetUniqueLogPath("default_sink_and_switching");
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "logger.test_default_sink_and_switching", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, MultithreadStress)
{
    auto log_path = GetUniqueLogPath("multithread_stress");
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "logger.test_multithread_stress", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, FlushWaitsForQueue)
{
    auto log_path = GetUniqueLogPath("flush_waits_for_queue");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_flush_waits_for_queue",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, ShutdownIdempotency)
{
    auto log_path = GetUniqueLogPath("shutdown_idempotency");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_shutdown_idempotency",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, ReentrantErrorCallback)
{
    auto log_path = GetUniqueLogPath("reentrant_error_callback");
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "logger.test_reentrant_error_callback", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, WriteErrorCallbackAsync)
{
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "logger.test_write_error_callback_async", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, DISABLED_PlatformSinks)
{
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_platform_sinks", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LoggerTest, ConcurrentLifecycleChaos)
{
    auto log_path = GetUniqueLogPath("concurrent_lifecycle_chaos");
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "logger.test_concurrent_lifecycle_chaos", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}
