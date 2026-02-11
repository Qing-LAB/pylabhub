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
#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

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
    WorkerProcess proc(g_self_exe_path, "logger.test_basic_logging", {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the LogLevelFiltering test logic to a worker process.
TEST_F(LoggerTest, LogLevelFiltering)
{
    auto log_path = GetUniqueLogPath("log_level_filtering");
    WorkerProcess proc(g_self_exe_path, "logger.test_log_level_filtering", {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the BadFormatString test logic to a worker process.
TEST_F(LoggerTest, BadFormatString)
{
    auto log_path = GetUniqueLogPath("bad_format_string");
    WorkerProcess proc(g_self_exe_path, "logger.test_bad_format_string", {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc, {"[FORMAT ERROR]"});
}

/// Delegates the DefaultSinkAndSwitching test logic to a worker process.
TEST_F(LoggerTest, DefaultSinkAndSwitching)
{
    auto log_path = GetUniqueLogPath("default_sink_and_switching");
    WorkerProcess proc(g_self_exe_path, "logger.test_default_sink_and_switching",
                       {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the MultithreadStress test logic to a worker process.
TEST_F(LoggerTest, MultithreadStress)
{
    auto log_path = GetUniqueLogPath("multithread_stress");
    WorkerProcess proc(g_self_exe_path, "logger.test_multithread_stress", {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the FlushWaitsForQueue test logic to a worker process.
TEST_F(LoggerTest, FlushWaitsForQueue)
{
    auto log_path = GetUniqueLogPath("flush_waits_for_queue");
    WorkerProcess proc(g_self_exe_path, "logger.test_flush_waits_for_queue", {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the ShutdownIdempotency test logic to a worker process.
TEST_F(LoggerTest, ShutdownIdempotency)
{
    auto log_path = GetUniqueLogPath("shutdown_idempotency");
    WorkerProcess proc(g_self_exe_path, "logger.test_shutdown_idempotency", {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the ReentrantErrorCallback test logic to a worker process.
TEST_F(LoggerTest, ReentrantErrorCallback)
{
    auto log_path = GetUniqueLogPath("reentrant_error_callback");
    WorkerProcess proc(g_self_exe_path, "logger.test_reentrant_error_callback",
                       {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the WriteErrorCallbackAsync test logic to a worker process.
TEST_F(LoggerTest, WriteErrorCallbackAsync)
{
    WorkerProcess proc(g_self_exe_path, "logger.test_write_error_callback_async", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the PlatformSinks smoke test to a worker process.
TEST_F(LoggerTest, PlatformSinks)
{
    WorkerProcess proc(g_self_exe_path, "logger.test_platform_sinks", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Delegates the ConcurrentLifecycleChaos test logic to a worker process.
TEST_F(LoggerTest, ConcurrentLifecycleChaos)
{
    auto log_path = GetUniqueLogPath("concurrent_lifecycle_chaos");
    WorkerProcess proc(g_self_exe_path, "logger.test_concurrent_lifecycle_chaos",
                       {log_path.string()});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
    // This test can be noisy on stderr due to the chaotic nature, so we don't assert empty.
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
    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "logger.stress_log",
            std::vector<std::string>{log_path.string(), std::to_string(MSGS_PER_PROC)}));
        ASSERT_TRUE(procs.back()->valid());
    }

    // Wait for all workers to complete.
    for (auto &proc : procs)
    {
        proc->wait_for_exit();
        expect_worker_ok(*proc);
    }

    // Verify the final log file.
    std::string log_contents;
    ASSERT_TRUE(read_file_contents(log_path.string(), log_contents));
    fmt::print(stderr, "Final log file size: {} bytes\n", log_contents.size());
    // fmt::print(stderr, "Final log file content:\n{}\n", log_contents);
    fmt::print(stderr, "Final log lines that contain [INFO  ]",
               count_lines(log_contents, "[INFO]"));
    ASSERT_EQ(count_lines(log_contents, "[INFO  ]"), PROCS * MSGS_PER_PROC);
}

/**
 * @brief Verifies inter-process locking via `use_flock=true`.
 *
 * Spawns multiple processes to write to the same log file concurrently with
 * flocking enabled. This test verifies that no messages are lost or corrupted
 * (torn writes), which would indicate a failure of the locking mechanism.
 */
TEST_F(LoggerTest, InterProcessFlock)
{
    auto log_path = GetUniqueLogPath("inter_process_flock");
    const int PROCS = 4;
    const int MSGS_PER_PROC = 250;

    // Spawn worker processes.
    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        std::string worker_id = "WORKER-" + std::to_string(i);
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "logger.test_inter_process_flock",
            std::vector<std::string>{log_path.string(), worker_id, std::to_string(MSGS_PER_PROC)}));
        ASSERT_TRUE(procs.back()->valid());
    }

    // Wait for all workers to complete.
    for (auto &proc : procs)
    {
        proc->wait_for_exit();
        expect_worker_ok(*proc);
    }

    // Verify the final log file.
    std::string log_contents;
    ASSERT_TRUE(read_file_contents(log_path.string(), log_contents));

    // 1. Check total line count.
    ASSERT_EQ(count_lines(log_contents, "[INFO  ]"), PROCS * MSGS_PER_PROC);

    // 2. Check for the integrity of each message from each worker.
    for (int i = 0; i < PROCS; ++i)
    {
        for (int j = 0; j < MSGS_PER_PROC; ++j)
        {
            std::string worker_id = "WORKER-" + std::to_string(i);
            std::string expected_payload = fmt::format(
                "WORKER_ID={} MSG_NUM={} PAYLOAD=[ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789]", worker_id,
                j);
            EXPECT_NE(log_contents.find(expected_payload), std::string::npos)
                << "Missing or corrupt message for " << worker_id << " message " << j;
        }
    }
}

/// Tests the RotatingFileSink functionality.
TEST_F(LoggerTest, RotatingFileSink)
{
    auto base_log_path = GetUniqueLogPath("rotating_sink_base");
    const size_t max_file_size_bytes = 256; // Small size to force rotations quickly
    const size_t max_backup_files = 2;

    WorkerProcess proc(g_self_exe_path, "logger.test_rotating_file_sink",
                       {base_log_path.string(), std::to_string(max_file_size_bytes),
                        std::to_string(max_backup_files)});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

/// Tests the failure case for setting a rotating log file in a non-writable directory.
TEST_F(LoggerTest, SetRotatingLogfileFailure)
{
    pylabhub::utils::LifecycleGuard guard({pylabhub::utils::Logger::GetLifecycleModule()});
#if PYLABHUB_IS_POSIX
    auto unwritable_dir = GetUniqueLogPath("unwritable_dir_for_rotating").parent_path() /
                          "unwritable_dir_for_rotating";
    fs::create_directories(unwritable_dir); // Ensure directory exists to set permissions
    paths_to_clean_.push_back(unwritable_dir);
    // Make directory unwritable
    fs::permissions(unwritable_dir, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);
    auto log_path = unwritable_dir / "test.log";

    std::error_code ec;
    // This should fail because of the pre-flight check in set_rotating_logfile
    ASSERT_FALSE(
        pylabhub::utils::Logger::instance().set_rotating_logfile(log_path, 1024, 5, true, ec));

    // The error code should indicate permission denied.
    ASSERT_EQ(ec, std::errc::permission_denied);

    // Restore permissions for cleanup
    fs::permissions(unwritable_dir, fs::perms::owner_all, fs::perm_options::replace);
#else
    // On Windows, we can use an invalid path name to simulate failure.
    // The pre-flight check for create_directories should fail.
    auto invalid_log_path = "C:\\*\\invalid:path.log"; // Invalid characters in path
    std::error_code ec;
    ASSERT_FALSE(pylabhub::utils::Logger::instance().set_rotating_logfile(invalid_log_path, 1024, 5,
                                                                          true, ec));
    ASSERT_TRUE(ec); // Should have an error. The specific error code might vary depending on OS
                     // version and exact invalid path.
#endif
}

/// Delegates the QueueFullAndMessageDropping test logic to a worker process.
TEST_F(LoggerTest, QueueFullAndMessageDropping)
{
    auto log_path = GetUniqueLogPath("queue_full_and_dropping");
    WorkerProcess proc(g_self_exe_path, "logger.test_queue_full_and_message_dropping",
                       {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}
