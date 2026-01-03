// tests/test_pylabhub_utils/test_logger.cpp
/**
 * @file test_logger.cpp
 * @brief Unit tests for the Logger utility.
 *
 * This file contains the main test runner for the `pylabhub::utils::Logger`.
 * Most test logic is encapsulated within worker functions, which are executed
 * in separate processes to ensure proper isolation of the logger's lifecycle
 * and state. This file is responsible for spawning those worker processes and
 * verifying their results.
 */
#include "platform.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"

namespace fs = std::filesystem;
using namespace pylabhub::tests::helper;

/**
 * @class LoggerTest
 * @brief Test fixture for Logger tests.
 *
 * Manages the creation of unique log file paths for each test and ensures
 * they are cleaned up afterwards.
 */
class LoggerTest : public ::testing::Test
{
  protected:
    std::vector<fs::path> paths_to_clean_;

    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            try
            {
                if (fs::exists(p))
                    fs::remove(p);
            }
            catch (...)
            {
                // best-effort cleanup
            }
        }
    }

    /// Generates a unique temporary path for a log file and registers it for cleanup.
    fs::path GetUniqueLogPath(const std::string &test_name)
    {
        auto p = fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
        paths_to_clean_.push_back(p);
        // Ensure the file does not exist from a previous failed run.
        try
        {
            if (fs::exists(p))
                fs::remove(p);
        }
        catch (...)
        {
        }
        return p;
    }
};

/// Delegates the BasicLogging test logic to a worker process.
TEST_F(LoggerTest, BasicLogging)
{
    auto log_path = GetUniqueLogPath("basic_logging");
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "logger.test_basic_logging", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the LogLevelFiltering test logic to a worker process.
TEST_F(LoggerTest, LogLevelFiltering)
{
    auto log_path = GetUniqueLogPath("log_level_filtering");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_log_level_filtering",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the BadFormatString test logic to a worker process.
TEST_F(LoggerTest, BadFormatString)
{
    auto log_path = GetUniqueLogPath("bad_format_string");
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "logger.test_bad_format_string", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the DefaultSinkAndSwitching test logic to a worker process.
TEST_F(LoggerTest, DefaultSinkAndSwitching)
{
    auto log_path = GetUniqueLogPath("default_sink_and_switching");
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "logger.test_default_sink_and_switching", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the MultithreadStress test logic to a worker process.
TEST_F(LoggerTest, MultithreadStress)
{
    auto log_path = GetUniqueLogPath("multithread_stress");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_multithread_stress",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the FlushWaitsForQueue test logic to a worker process.
TEST_F(LoggerTest, FlushWaitsForQueue)
{
    auto log_path = GetUniqueLogPath("flush_waits_for_queue");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_flush_waits_for_queue",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the ShutdownIdempotency test logic to a worker process.
TEST_F(LoggerTest, ShutdownIdempotency)
{
    auto log_path = GetUniqueLogPath("shutdown_idempotency");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_shutdown_idempotency",
                                              {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the ReentrantErrorCallback test logic to a worker process.
TEST_F(LoggerTest, ReentrantErrorCallback)
{
    auto log_path = GetUniqueLogPath("reentrant_error_callback");
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "logger.test_reentrant_error_callback", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the WriteErrorCallbackAsync test logic to a worker process.
TEST_F(LoggerTest, WriteErrorCallbackAsync)
{
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "logger.test_write_error_callback_async", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the PlatformSinks smoke test to a worker process.
TEST_F(LoggerTest, PlatformSinks)
{
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.test_platform_sinks", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Delegates the ConcurrentLifecycleChaos test logic to a worker process.
TEST_F(LoggerTest, ConcurrentLifecycleChaos)
{
    auto log_path = GetUniqueLogPath("concurrent_lifecycle_chaos");
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "logger.test_concurrent_lifecycle_chaos", {log_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/**
 * @brief Stress-tests logging from multiple processes concurrently.
 *
 * Spawns multiple worker processes that all write a large number of messages
 * to the same log file. The test then verifies that the total number of lines
 * in the log file matches the total number of messages sent.
 */
TEST_F(LoggerTest, StressLog)
{
    auto log_path = GetUniqueLogPath("stress_log");
    const int PROCS = 8;
    const int MSGS_PER_PROC = 200;

    // Spawn worker processes.
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        ProcessHandle h = spawn_worker_process(g_self_exe_path, "logger.stress_log",
                                               {log_path.string(), std::to_string(MSGS_PER_PROC)});
        ASSERT_NE(h, NULL_PROC_HANDLE);
        procs.push_back(h);
    }

    // Wait for all workers to complete.
    for (auto h : procs)
    {
        ASSERT_EQ(wait_for_worker_and_get_exit_code(h), 0);
    }

    // Verify the final log file.
    std::string log_contents;
    ASSERT_TRUE(read_file_contents(log_path.string(), log_contents));
    ASSERT_EQ(count_lines(log_contents), PROCS * MSGS_PER_PROC);
}
