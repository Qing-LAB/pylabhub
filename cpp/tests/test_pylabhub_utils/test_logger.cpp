#include "platform.hpp"
#include "test_entrypoint.h"   // Keep this specific header
#include "test_process_utils.h"
// Standard Library
#include <string>
#include <vector>

using namespace pylabhub::tests::helper;

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

TEST_F(LoggerTest, PlatformSinks)
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

TEST_F(LoggerTest, StressLog)
{
    auto log_path = GetUniqueLogPath("stress_log");
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "logger.stress_log",
                                              {log_path.string(), "1000"});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}
