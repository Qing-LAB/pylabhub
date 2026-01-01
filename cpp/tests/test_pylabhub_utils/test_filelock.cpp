// tests/test_pylabhub_utils/test_filelock.cpp
/**
 * @file test_filelock.cpp
 * @brief Unit tests for the FileLock utility.
 *
 * This file contains tests for the `pylabhub::utils::FileLock` class.
 * Many tests delegate their logic to a worker process to ensure that
 * locking mechanisms are tested in a true multi-process environment.
 * Other tests verify behavior within a single process, such as multi-threaded
 * contention and parent-child blocking.
 */
#include "platform.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "utils/FileLock.hpp"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_process_utils.h"

using namespace pylabhub::tests::helper;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

/**
 * @class FileLockTest
 * @brief Test fixture for FileLock tests.
 *
 * Sets up a temporary directory for test resources and provides helper
 * methods for cleaning up lock files.
 */
class FileLockTest : public ::testing::Test {
protected:
    static fs::path g_temp_dir_;

    static void SetUpTestSuite()
    {
        g_temp_dir_ = fs::temp_directory_path() / "pylabhub_filelock_tests";
        fs::create_directories(g_temp_dir_);
    }

    static void TearDownTestSuite()
    {
        try
        {
            fs::remove_all(g_temp_dir_);
        }
        catch (...)
        {
            // Best-effort cleanup
        }
    }

    fs::path temp_dir() const { return g_temp_dir_; }

    /// Helper to remove a lock file for a given resource to ensure a clean state.
    void clear_lock_file(const fs::path &resource_path, pylabhub::utils::ResourceType type)
    {
        try
        {
            fs::remove(pylabhub::utils::FileLock::get_expected_lock_fullname_for(resource_path, type));
        }
        catch (...)
        {
            // Ignore errors if the file doesn't exist.
        }
    }
};

fs::path FileLockTest::g_temp_dir_;

/// Tests basic non-blocking lock acquisition logic in a worker process.
TEST_F(FileLockTest, BasicNonBlocking)
{
    auto resource_path = temp_dir() / "basic_resource.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.test_basic_non_blocking",
                                              {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Tests blocking lock behavior between threads in a worker process.
TEST_F(FileLockTest, BlockingLock)
{
    auto resource_path = temp_dir() / "blocking_resource.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "filelock.test_blocking_lock", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Tests timed lock acquisition behavior in a worker process.
TEST_F(FileLockTest, TimedLock)
{
    auto resource_path = temp_dir() / "timed.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "filelock.test_timed_lock", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Tests the move semantics (constructor and assignment) of FileLock in a worker process.
TEST_F(FileLockTest, MoveSemantics)
{
    auto resource1 = temp_dir() / "move1.txt";
    auto resource2 = temp_dir() / "move2.txt";
    clear_lock_file(resource1, pylabhub::utils::ResourceType::File);
    clear_lock_file(resource2, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_move_semantics", {resource1.string(), resource2.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Tests that parent directories for the lock file are created automatically.
TEST_F(FileLockTest, DirectoryCreation)
{
    auto new_dir = temp_dir() / "new_dir_for_lock";
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.test_directory_creation",
                                              {new_dir.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Tests that a lock can be acquired on a directory path itself.
TEST_F(FileLockTest, DirectoryPathLocking)
{
    auto dir_to_lock = temp_dir() / "dir_to_lock_parent";
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_directory_path_locking", {dir_to_lock.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/// Tests non-blocking lock contention between multiple threads in a single worker process.
TEST_F(FileLockTest, MultiThreadedNonBlocking)
{
    auto resource_path = temp_dir() / "multithread.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_multithreaded_non_blocking", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/**
 * @brief Tests non-blocking lock acquisition between two processes.
 *
 * This test acquires a lock in the main process and then spawns a worker
 * process that attempts to acquire the same lock non-blockingly. The worker
 * is expected to fail immediately.
 */
TEST_F(FileLockTest, MultiProcessNonBlocking)
{
    pylabhub::lifecycle::LifecycleGuard guard(
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::Logger::GetLifecycleModule()
    );
    auto resource_path = temp_dir() / "multiprocess.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);

    // Acquire lock in the main test process
    pylabhub::utils::FileLock main_lock(resource_path, pylabhub::utils::ResourceType::File, pylabhub::utils::LockMode::Blocking);
    ASSERT_TRUE(main_lock.valid());

    // Spawn a worker that will try to acquire the same lock non-blockingly and should fail.
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.nonblocking_acquire", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

/**
 * @brief Stress-tests blocking lock contention between multiple processes.
 *
 * Spawns multiple worker processes that all contend for the same blocking lock.
 * Each worker logs timestamps upon acquiring and releasing the lock. The test
 * verifies the resulting log file to ensure that no two processes held the
 * lock simultaneously.
 */
TEST_F(FileLockTest, MultiProcessBlockingContention)
{
    auto resource_path = temp_dir() / "contention_resource.txt";
    auto log_path = temp_dir() / "contention_log.txt";

    // Clear previous run files
    fs::remove(resource_path);
    fs::remove(log_path);
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);

    const int PROCS = 8;
    const int ITERS_PER_WORKER = 100;

    // Spawn worker processes
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        ProcessHandle h =
            spawn_worker_process(g_self_exe_path, "filelock.contention_log_access",
                                 {resource_path.string(), log_path.string(),
                                  std::to_string(ITERS_PER_WORKER)});
        ASSERT_NE(h, NULL_PROC_HANDLE);
        procs.push_back(h);
    }

    // Wait for all workers to finish
    for (auto h : procs)
    {
        ASSERT_EQ(wait_for_worker_and_get_exit_code(h), 0);
    }

    // --- Verification Phase ---
    struct LogEntry
    {
        long long timestamp;
        long pid;
        std::string action;

        bool operator<(const LogEntry &other) const { return timestamp < other.timestamp; }
    };

    std::ifstream log_stream(log_path);
    ASSERT_TRUE(log_stream.is_open()) << "Failed to open log file for verification.";

    std::vector<LogEntry> entries;
    std::string line;
    while (std::getline(log_stream, line))
    {
        std::stringstream ss(line);
        LogEntry entry;
        if (ss >> entry.timestamp >> entry.pid >> entry.action)
        {
            entries.push_back(entry);
        }
    }

    // The total number of entries should be exactly twice the total number of iterations.
    ASSERT_EQ(entries.size(), PROCS * ITERS_PER_WORKER * 2);

    std::sort(entries.begin(), entries.end());

    // Iterate through the sorted log entries to ensure no overlapping locks.
    int lock_held_count = 0;
    long last_pid_to_acquire = -1;
    for (const auto &entry : entries)
    {
        if (entry.action == "ACQUIRE")
        {
            ASSERT_EQ(lock_held_count, 0)
                << "Lock acquired while already held! PID " << entry.pid
                << " tried to acquire while PID " << last_pid_to_acquire
                << " held it. Timestamp: " << entry.timestamp;
            lock_held_count++;
            last_pid_to_acquire = entry.pid;
        }
        else if (entry.action == "RELEASE")
        {
            ASSERT_EQ(lock_held_count, 1)
                << "Lock released while not held! PID " << entry.pid
                << " tried to release. Timestamp: " << entry.timestamp;
            ASSERT_EQ(entry.pid, last_pid_to_acquire)
                << "Mismatch in lock release! PID " << last_pid_to_acquire
                << " acquired the lock, but PID " << entry.pid << " released it.";
            lock_held_count--;
        }
    }

    ASSERT_EQ(lock_held_count, 0) << "Lock was not released at the end of the test.";
}

/**
 * @brief Tests that a child process correctly blocks waiting for a lock held by its parent.
 */
TEST_F(FileLockTest, MultiProcessParentChildBlocking)
{
    pylabhub::lifecycle::LifecycleGuard guard(
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::Logger::GetLifecycleModule()
    );
    auto resource_path = temp_dir() / "parent_child_block.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    
    // Acquire lock in the parent process.
    auto parent_lock = std::make_unique<pylabhub::utils::FileLock>(resource_path, pylabhub::utils::ResourceType::File, pylabhub::utils::LockMode::Blocking);
    ASSERT_TRUE(parent_lock->valid());

    // Spawn child process, which should block trying to acquire the same lock.
    ProcessHandle child_proc = spawn_worker_process(g_self_exe_path, "filelock.parent_child_block", {resource_path.string()});
    ASSERT_NE(child_proc, NULL_PROC_HANDLE);

    // Give the child time to block.
    std::this_thread::sleep_for(200ms);
    // Release the parent lock, allowing the child to proceed.
    parent_lock.reset(); 

    // The child should now be able to acquire the lock and exit successfully.
    ASSERT_EQ(wait_for_worker_and_get_exit_code(child_proc), 0);
}
