// tests/test_filelock.cpp
//
// Test harness for pylabhub::utils::FileLock.
// Each test case is executed in a separate worker process to ensure full
// isolation of the lifecycle-managed components.

#include "platform.hpp"



#include <gtest/gtest.h>

#include <filesystem>

#include <thread>

#include <chrono>

#include <fstream>



#include "utils/FileLock.hpp"



#include "test_entrypoint.h"



#include "shared_test_helpers.h"



// Specific includes for this test file that are not covered by the preamble

#include "test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace

#include <algorithm>

#include <iostream>

#include <sstream>

#include <string>

#include <vector>

using namespace pylabhub::tests::helper;

namespace fs = std::filesystem;

using namespace std::chrono_literals;



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
        }
    }

    fs::path temp_dir() const { return g_temp_dir_; }

    void clear_lock_file(const fs::path &resource_path, pylabhub::utils::ResourceType type)
    {
        try
        {
            fs::remove(pylabhub::utils::FileLock::get_expected_lock_fullname_for(resource_path, type));
        }
        catch (...)
        {
        }
    }
};

fs::path FileLockTest::g_temp_dir_;

TEST_F(FileLockTest, BasicNonBlocking)
{
    auto resource_path = temp_dir() / "basic_resource.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.test_basic_non_blocking",
                                              {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, BlockingLock)
{
    auto resource_path = temp_dir() / "blocking_resource.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "filelock.test_blocking_lock", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, TimedLock)
{
    auto resource_path = temp_dir() / "timed.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "filelock.test_timed_lock", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

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

TEST_F(FileLockTest, DirectoryCreation)
{
    auto new_dir = temp_dir() / "new_dir_for_lock";
    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.test_directory_creation",
                                              {new_dir.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, DirectoryPathLocking)
{
    auto dir_to_lock = temp_dir() / "dir_to_lock_parent";
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_directory_path_locking", {dir_to_lock.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, MultiThreadedNonBlocking)
{
    auto resource_path = temp_dir() / "multithread.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "filelock.test_multithreaded_non_blocking", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(FileLockTest, MultiProcessNonBlocking)
{
    pylabhub::lifecycle::LifecycleGuard guard(
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::Logger::GetLifecycleModule()
    );
    auto resource_path = temp_dir() / "multiprocess.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);

    pylabhub::utils::FileLock main_lock(resource_path, pylabhub::utils::ResourceType::File, pylabhub::utils::LockMode::Blocking);
    ASSERT_TRUE(main_lock.valid());

    ProcessHandle proc = spawn_worker_process(g_self_exe_path, "filelock.nonblocking_acquire", {resource_path.string()});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

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

    // The total number of entries should be exactly twice the total number of iterations
    ASSERT_EQ(entries.size(), PROCS * ITERS_PER_WORKER * 2);

    std::sort(entries.begin(), entries.end());

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

TEST_F(FileLockTest, MultiProcessParentChildBlocking)
{
    pylabhub::lifecycle::LifecycleGuard guard(
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::Logger::GetLifecycleModule()
    );
    auto resource_path = temp_dir() / "parent_child_block.txt";
    clear_lock_file(resource_path, pylabhub::utils::ResourceType::File);
    
    auto parent_lock = std::make_unique<pylabhub::utils::FileLock>(resource_path, pylabhub::utils::ResourceType::File, pylabhub::utils::LockMode::Blocking);
    ASSERT_TRUE(parent_lock->valid());

    ProcessHandle child_proc = spawn_worker_process(g_self_exe_path, "filelock.parent_child_block", {resource_path.string()});
    ASSERT_NE(child_proc, NULL_PROC_HANDLE);

    std::this_thread::sleep_for(200ms);
    parent_lock.reset(); 

    ASSERT_EQ(wait_for_worker_and_get_exit_code(child_proc), 0);
}
