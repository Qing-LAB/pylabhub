// tests/test_filelock.cpp
//
// Unit tests for pylabhub::utils::FileLock.

#include "helpers/test_process_utils.h"
#include <gtest/gtest.h>
#include "helpers/workers.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>

#include <fmt/core.h>

#include "platform.hpp"
#include "utils/FileLock.hpp"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include "helpers/test_entrypoint.h" // provides extern std::string g_self_exe_path

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pylabhub::utils;
using namespace test_utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

} // namespace

// --- Test Fixture ---
// Creates a temporary directory for lock files and ensures the utils lifecycle
// is managed for the entire test suite.
class FileLockTest : public ::testing::Test {
protected:
    static fs::path g_temp_dir_;

    static void SetUpTestSuite() {
        g_temp_dir_ = fs::temp_directory_path() / "pylabhub_filelock_tests";
        fs::create_directories(g_temp_dir_);
        fmt::print("Using temporary directory: {}\n", g_temp_dir_.string());
        pylabhub::utils::Initialize();
    }

    static void TearDownTestSuite() {
        pylabhub::utils::Finalize();
        // Best-effort cleanup
        try {
            fs::remove_all(g_temp_dir_);
        } catch (...) {
            // ignore
        }
    }

    // Short helper to get the fixture temp dir
    fs::path temp_dir() const { return g_temp_dir_; }
};

// Test fixture for FileLock tests.
class FileLockTest : public ::testing::Test {
protected:
    // Static member to hold the path to the temporary directory.
    // Used by tests to create lock files in a clean, isolated location.
    static fs::path g_temp_dir_;

    // Per-test temporary path for resources.
    fs::path temp_dir() const { return g_temp_dir_; }

    static void SetUpTestSuite() {
        g_temp_dir_ = fs::temp_directory_path() / "pylabhub_filelock_tests";
        fs::create_directories(g_temp_dir_);
        fmt::print("Using temporary directory for FileLock tests: {}\n", g_temp_dir_.string());
    }

    static void TearDownTestSuite() {
        try { fs::remove_all(g_temp_dir_); } catch (...) {}
    }

    // SetUp and TearDown for individual tests if needed
    // void SetUp() override {}
    // void TearDown() override {}
};

fs::path FileLockTest::g_temp_dir_;



// What: Verifies basic non-blocking lock acquisition and release.
// How: It acquires a lock and verifies it is valid. It then demonstrates that
//      attempting to acquire the same lock again *within the same process* fails,
//      confirming the intra-process safety mechanism. Finally, after the first
//      lock is destroyed (by RAII), it verifies the resource can be locked again.
TEST_F(FileLockTest, BasicNonBlocking)
{
    auto resource_path = temp_dir() / "basic_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    {
        FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_FALSE(lock.error_code());

        // Second non-blocking lock in same process should fail due to intra-process registry.
        FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_FALSE(lock2.valid());
    }

    // After scope, it should be re-lockable.
    FileLock lock3(resource_path, ResourceType::File, LockMode::NonBlocking);
    ASSERT_TRUE(lock3.valid());
}

// What: Verifies that a blocking lock correctly waits for a resource to be released.
// How: The main thread acquires a blocking lock. A second thread is spawned and
//      attempts to acquire the same lock. The main thread sleeps to ensure the
//      second thread has time to block. The main thread then releases its lock.
//      The test verifies that the second thread successfully acquired the lock and
//      that it was blocked for a measurable amount of time.
TEST_F(FileLockTest, BlockingLock)
{
    auto resource_path = temp_dir() / "blocking_resource.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    std::atomic<bool> thread_valid{false};
    std::atomic<bool> thread_saw_block{false};

    auto main_lock = std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
    ASSERT_TRUE(main_lock->valid());

    std::thread t1([&]() {
        auto start = std::chrono::steady_clock::now();
        FileLock thread_lock(resource_path, ResourceType::File, LockMode::Blocking);
        auto end = std::chrono::steady_clock::now();

        if (thread_lock.valid()) thread_valid.store(true);
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        if (dur > 100ms) thread_saw_block.store(true);
    });

    std::this_thread::sleep_for(200ms);
    main_lock.reset(); // Release the lock, allowing the thread to proceed.

    t1.join();

    ASSERT_TRUE(thread_valid.load());
    ASSERT_TRUE(thread_saw_block.load());
}

// What: Verifies the behavior of a timed lock.
// How: First, it acquires a blocking lock. It then attempts to acquire a timed
//      lock on the same resource with a 100ms timeout. It asserts that this
//      attempt fails, returns a `timed_out` error code, and took at least 100ms.
//      Second, it releases the main lock and verifies that a timed lock can
//      now be acquired successfully.
TEST_F(FileLockTest, TimedLock)
{
    auto resource_path = temp_dir() / "timed.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    // Test the timeout failure case.
    {
        FileLock main_lock(resource_path, ResourceType::File, LockMode::Blocking);
        ASSERT_TRUE(main_lock.valid());

        auto start = std::chrono::steady_clock::now();
        FileLock timed_lock_fail(resource_path, ResourceType::File, 100ms);
        auto end = std::chrono::steady_clock::now();

        ASSERT_FALSE(timed_lock_fail.valid());
        ASSERT_TRUE(timed_lock_fail.error_code());
        ASSERT_EQ(timed_lock_fail.error_code().value(), static_cast<int>(std::errc::timed_out));

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        ASSERT_GE(duration.count(), 100);
        ASSERT_LT(duration.count(), 1000); // Sanity check it didn't block excessively.
    }

    // Test the success case.
    FileLock timed_lock_succeed(resource_path, ResourceType::File, 100ms);
    ASSERT_TRUE(timed_lock_succeed.valid());
    ASSERT_FALSE(timed_lock_succeed.error_code());
}

// What: Provides comprehensive testing of the FileLock's move semantics.
// How: It verifies move construction, move assignment, and self-move assignment.
//      For each operation, it checks that the source lock becomes invalid, the
//      destination lock becomes valid, and that the original underlying lock is
//      correctly released only when the final owner is destructed.
TEST_F(FileLockTest, MoveSemanticsFull)
{
    auto resource1 = temp_dir() / "move1.txt";
    auto resource2 = temp_dir() / "move2.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource1, ResourceType::File));
    fs::remove(FileLock::get_expected_lock_fullname_for(resource2, ResourceType::File));

    // Move constructor
    {
        FileLock lock1(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock1.valid());
        FileLock lock2(std::move(lock1));
        ASSERT_TRUE(lock2.valid());
        ASSERT_FALSE(lock1.valid());
    } // lock2 destructs, releases lock on resource1.

    // Verify resource1 is free.
    {
        FileLock lock1_again(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock1_again.valid());
    }

    // Move assignment.
    {
        FileLock lock_A(resource1, ResourceType::File, LockMode::NonBlocking);
        FileLock lock_B(resource2, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock_A.valid());
        ASSERT_TRUE(lock_B.valid());

        lock_B = std::move(lock_A); // lock_B should release resource2 and take resource1.
        ASSERT_TRUE(lock_B.valid());
        ASSERT_FALSE(lock_A.valid());

        // Verify resource2 is now free.
        FileLock lock_res2_again(resource2, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock_res2_again.valid());
    }

    // Self-move assignment should be a no-op and not invalidate the lock.
    {
        FileLock lock_self(resource1, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock_self.valid());

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
        lock_self = std::move(lock_self);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

        ASSERT_TRUE(lock_self.valid());
    }
}

// What: Ensures that the FileLock constructor creates the necessary parent
//       directories for the lock file if they do not exist.
// How: It defines a resource path inside a non-existent directory. It then
//      constructs a FileLock for that resource and asserts that the parent
//      directory and the lock file itself were created successfully.
TEST_F(FileLockTest, DirectoryCreation)
{
    auto new_dir = temp_dir() / "new_dir_for_lock";
    auto resource_to_lock = new_dir / "resource.txt";
    auto actual_lock_file =
        FileLock::get_expected_lock_fullname_for(resource_to_lock, ResourceType::File);

    fs::remove_all(new_dir);
    ASSERT_FALSE(fs::exists(new_dir));

    {
        FileLock lock(resource_to_lock, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_TRUE(fs::exists(new_dir));
        ASSERT_TRUE(fs::exists(actual_lock_file));
    }

    fs::remove_all(new_dir);
}

// What: Verifies the behavior of locking a directory path and edge cases like "." and "/".
// How: It locks a path with `ResourceType::Directory` and confirms the correct
//      `.dir.lock` file is created. It also confirms that this does not conflict
//      with a `ResourceType::File` lock on the same path. It then tests locking
//      the current directory (".") and, on POSIX, a path resolving to the root
//      directory ("/").
TEST_F(FileLockTest, DirectoryPathLocking)
{
    // Standard directory locking vs. file locking on the same path.
    {
        auto dir_to_lock = temp_dir() / "dir_to_lock";
        fs::create_directory(dir_to_lock);

        auto expected_dir_lock_file = FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::Directory);
        auto regular_file_lock_path = FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::File);
        fs::remove(expected_dir_lock_file);
        fs::remove(regular_file_lock_path);

        FileLock lock(dir_to_lock, ResourceType::Directory, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_TRUE(fs::exists(expected_dir_lock_file));
        ASSERT_FALSE(fs::exists(regular_file_lock_path));

        // A file-type lock on the same path should not conflict.
        FileLock non_conflicting_lock(temp_dir() / "dir_to_lock", ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(non_conflicting_lock.valid());
    }

    // Current directory locking.
    {
        auto expected_lock_file = FileLock::get_expected_lock_fullname_for(".", ResourceType::Directory);
        fs::remove(expected_lock_file);
        FileLock lock(".", ResourceType::Directory, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        ASSERT_TRUE(fs::exists(expected_lock_file));
        fs::remove(expected_lock_file);
    }

#if !defined(PLATFORM_WIN64)
    // Root directory locking on POSIX.
    {
        fs::path path_to_root = ".";
        for (const auto &part : fs::current_path()) {
            if (part.string() != "/") path_to_root /= "..";
        }
        auto generated = FileLock::get_expected_lock_fullname_for(path_to_root, ResourceType::Directory);
        ASSERT_EQ(generated, fs::path("/pylabhub_root.dir.lock"));
    }
#endif
}

// What: A stress test to verify the intra-process lock registry is thread-safe.
// How: It spawns a large number of threads that all race to acquire the same
//      non-blocking lock. The test asserts that exactly one thread succeeds,
//      proving that the internal, thread-safe registry correctly arbitrates
//      access between threads in the same process.
TEST_F(FileLockTest, MultiThreadedNonBlocking)
{
    auto resource_path = temp_dir() / "multithread.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int THREADS = 64;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([&, i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(i % 10)); // Stagger starts slightly
            FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
            if (lock.valid())
            {
                success_count.fetch_add(1);
                std::this_thread::sleep_for(50ms);
            }
        });
    }

    for (auto &t : threads) t.join();

    ASSERT_EQ(success_count.load(), 1);
}

// What: A stress test to verify cross-process non-blocking lock contention.
// How: It spawns a large number of child processes, each of which immediately
//      tries to acquire the same non-blocking lock. The test waits for all
//      children to exit and checks their exit codes, asserting that exactly one
//      process succeeded in acquiring the lock.
TEST_F(FileLockTest, MultiProcessNonBlocking)
{
    auto resource_path = temp_dir() / "multiprocess.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    const int PROCS = 32;
    int success_count = 0;

#if defined(PLATFORM_WIN64)
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        ProcessHandle h = spawn_worker_process(g_self_exe_path, "filelock.nonblocking_acquire", std::vector<std::string>{resource_path.string()});
        ASSERT_TRUE(h != nullptr);
        procs.push_back(h);
    }

    for (auto h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h, &exit_code);
        if (exit_code == 0) success_count++;
        CloseHandle(h);
    }
#else
    std::vector<ProcessHandle> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        ProcessHandle pid = spawn_worker_process(g_self_exe_path, "filelock.nonblocking_acquire", std::vector<std::string>{resource_path.string()});
        ASSERT_GT(pid, 0);
        pids.push_back(pid);
    }

    for (auto pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) success_count++;
    }
#endif

    ASSERT_EQ(success_count, 1);
}

// What: A stress test to verify the atomicity of a file-based counter across
//       multiple processes using a blocking lock.
// How: An initial counter file is created. Many child processes are spawned, and
//      each one loops, acquiring a blocking lock, reading the file, incrementing
//      the value, and writing it back. After all children complete, the test
//      asserts that the final value in the file is equal to the number of
//      processes times the number of iterations, proving that no updates were lost.
TEST_F(FileLockTest, MultiProcessBlockingContention)
{
    auto counter_path = temp_dir() / "counter.txt";
    fs::remove(counter_path);
    fs::remove(FileLock::get_expected_lock_fullname_for(counter_path, ResourceType::File));

    {
        std::ofstream ofs(counter_path);
        ofs << 0;
    }

    const int PROCS = 16;
    const int ITERS_PER_WORKER = 100;

#if defined(PLATFORM_WIN64)
    std::vector<ProcessHandle> procs;
    for (int i = 0; i < PROCS; ++i)
    {
        ProcessHandle h = spawn_worker_process(g_self_exe_path, "filelock.contention_increment",
                                        std::vector<std::string>{counter_path.string(), std::to_string(ITERS_PER_WORKER)});
        ASSERT_TRUE(h != nullptr);
        procs.push_back(h);
    }

    for (auto h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(h, &exit_code);
        CloseHandle(h);
        ASSERT_EQ(exit_code, 0);
    }
#else
    std::vector<ProcessHandle> pids;
    for (int i = 0; i < PROCS; ++i)
    {
        ProcessHandle pid = spawn_worker_process(g_self_exe_path, "filelock.contention_increment",
                                         std::vector<std::string>{counter_path.string(), std::to_string(ITERS_PER_WORKER)});
        ASSERT_GT(pid, 0);
        pids.push_back(pid);
    }

    for (auto pid : pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
#endif

    // Verify final counter value.
    int final_value = 0;
    {
        std::ifstream ifs(counter_path);
        if (ifs.is_open()) { ifs >> final_value; }
    }
    ASSERT_EQ(final_value, PROCS * ITERS_PER_WORKER);
}

// What: Verifies blocking lock behavior between a parent and child process.
// How: The parent process acquires a blocking lock. It then spawns a child worker
//      process that immediately tries to acquire the same lock. The parent sleeps,
//      then releases its lock. The worker measures how long it was blocked. The test
//      asserts that the child was successfully unblocked and that it was blocked
//      for a measurable duration.
TEST_F(FileLockTest, MultiProcessParentChildBlocking)
{
    auto resource_path = temp_dir() / "parent_child_block.txt";
    fs::remove(FileLock::get_expected_lock_fullname_for(resource_path, ResourceType::File));

    // Parent acquires the lock.
    auto parent_lock = std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
    ASSERT_TRUE(parent_lock->valid());

#if defined(PLATFORM_WIN64)
    ProcessHandle child_proc = spawn_worker_process(g_self_exe_path, "filelock.parent_child_block", std::vector<std::string>{resource_path.string()});
    ASSERT_TRUE(child_proc != nullptr);

    std::this_thread::sleep_for(200ms);
    parent_lock.reset(); // Release lock explicitly.

    WaitForSingleObject(child_proc, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(child_proc, &exit_code);
    CloseHandle(child_proc);
    ASSERT_EQ(exit_code, 0);
#else
    ProcessHandle pid = spawn_worker_process(g_self_exe_path, "filelock.parent_child_block", std::vector<std::string>{resource_path.string()});
    ASSERT_GT(pid, 0);

    std::this_thread::sleep_for(200ms);
    parent_lock.reset(); // Release lock explicitly.

    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}