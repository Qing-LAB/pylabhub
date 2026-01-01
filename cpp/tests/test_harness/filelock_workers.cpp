// tests/test_harness/filelock_workers.cpp
/**
 * @file filelock_workers.cpp
 * @brief Implements worker functions for FileLock tests.
 *
 * These functions are executed in separate processes to test the cross-process
 * functionality of the FileLock utility. Each function encapsulates a specific
 * test scenario and is invoked by the main test runner.
 */
#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

// Third-party
#include <gtest/gtest.h>

// Project-specific
#include "filelock_workers.h"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "utils/FileLock.hpp"
#include "utils/Logger.hpp"

using namespace pylabhub::tests::helper;
using namespace pylabhub::utils;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker
{
namespace filelock
{
int test_basic_non_blocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path resource_path(resource_path_str);
            {
                // First lock should succeed
                FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock.valid());
                ASSERT_FALSE(lock.error_code());

                // Second non-blocking lock on the same resource should fail immediately
                FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
                ASSERT_FALSE(lock2.valid()) << "Second non-blocking lock should fail.";
            }
            // After the first lock is out of scope and released, a new lock should succeed
            FileLock lock3(resource_path, ResourceType::File, LockMode::NonBlocking);
            ASSERT_TRUE(lock3.valid());
        },
        "filelock::test_basic_non_blocking",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int test_blocking_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path resource_path(resource_path_str);
            std::atomic<bool> thread_valid{false};
            std::atomic<bool> thread_saw_block{false};

            // Main thread acquires a blocking lock
            auto main_lock =
                std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
            ASSERT_TRUE(main_lock->valid());

            // Spawn a second thread that will block trying to acquire the same lock
            std::thread t1([&]() {
                auto start = std::chrono::steady_clock::now();
                FileLock thread_lock(resource_path, ResourceType::File, LockMode::Blocking);
                auto end = std::chrono::steady_clock::now();
                if (thread_lock.valid()) thread_valid.store(true);
                // Verify that the lock call blocked for a significant time
                if (std::chrono::duration_cast<std::chrono::milliseconds>(end - start) > 100ms)
                    thread_saw_block.store(true);
            });

            // Wait long enough for the second thread to block
            std::this_thread::sleep_for(200ms);
            main_lock.reset(); // Release lock
            t1.join();

            // The second thread should have eventually acquired the lock and seen the block
            ASSERT_TRUE(thread_valid.load());
            ASSERT_TRUE(thread_saw_block.load());
        },
        "filelock::test_blocking_lock",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int test_timed_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path resource_path(resource_path_str);
            {
                // Acquire a lock so the timed lock will fail
                FileLock main_lock(resource_path, ResourceType::File, LockMode::Blocking);
                ASSERT_TRUE(main_lock.valid());

                // Attempt to acquire a timed lock, which should time out
                auto start = std::chrono::steady_clock::now();
                FileLock timed_lock_fail(resource_path, ResourceType::File, 100ms);
                auto end = std::chrono::steady_clock::now();

                ASSERT_FALSE(timed_lock_fail.valid());
                ASSERT_EQ(timed_lock_fail.error_code(), std::errc::timed_out);
                // Check that it waited for at least the specified timeout
                ASSERT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(end - start),
                          100ms);
            }

            // Now that the main lock is released, a timed lock should succeed
            FileLock timed_lock_succeed(resource_path, ResourceType::File, 100ms);
            ASSERT_TRUE(timed_lock_succeed.valid());
        },
        "filelock::test_timed_lock",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int test_move_semantics(const std::string &resource1_str, const std::string &resource2_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path resource1(resource1_str);
            std::filesystem::path resource2(resource2_str);

            {
                FileLock lock1(resource1, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock1.valid());
                // Move constructor: lock2 should take ownership
                FileLock lock2(std::move(lock1));
                ASSERT_TRUE(lock2.valid());
                ASSERT_FALSE(lock1.valid()); // Original lock is now invalid
            } // lock2 is destructed, releasing the lock on resource1
            
            // Verify that the lock on resource1 was released
            {
                FileLock lock1_again(resource1, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock1_again.valid());
            }
        },
        "filelock::test_move_semantics",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int test_directory_creation(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path new_dir(base_dir_str);
            auto resource_to_lock = new_dir / "resource.txt";
            auto actual_lock_file =
                FileLock::get_expected_lock_fullname_for(resource_to_lock, ResourceType::File);

            std::filesystem::remove_all(new_dir);
            ASSERT_FALSE(std::filesystem::exists(new_dir));
            {
                // Acquiring a lock for a resource in a non-existent directory should create it
                FileLock lock(resource_to_lock, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock.valid());
                ASSERT_TRUE(std::filesystem::exists(new_dir));
                ASSERT_TRUE(std::filesystem::exists(actual_lock_file));
            }
        },
        "filelock::test_directory_creation",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int test_directory_path_locking(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path base_dir(base_dir_str);
            auto dir_to_lock = base_dir / "dir_to_lock";
            std::filesystem::create_directories(dir_to_lock);

            // Test locking a directory itself, not a file within it
            auto expected_dir_lock_file =
                FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::Directory);
            FileLock lock(dir_to_lock, ResourceType::Directory, LockMode::NonBlocking);
            ASSERT_TRUE(lock.valid());
            ASSERT_TRUE(std::filesystem::exists(expected_dir_lock_file));
        },
        "filelock::test_directory_path_locking",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int test_multithreaded_non_blocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path resource_path(resource_path_str);
            const int THREADS = 32;
            std::atomic<int> success_count{0};
            std::vector<std::thread> threads;
            threads.reserve(THREADS);

            // Spawn many threads that all contend for the same non-blocking lock
            for (int i = 0; i < THREADS; ++i)
            {
                threads.emplace_back([&, i]() {
                    // Small sleep to increase chance of contention
                    std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
                    FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                    if (lock.valid())
                    {
                        success_count.fetch_add(1);
                        // Hold the lock briefly
                        std::this_thread::sleep_for(50ms);
                    }
                });
            }
            for (auto &t : threads) t.join();

            // Only one thread should have successfully acquired the lock
            ASSERT_EQ(success_count.load(), 1);
        },
        "filelock::test_multithreaded_non_blocking",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int nonblocking_acquire(const std::string &resource_path_str)
{
    return run_gtest_worker( [&]() {
        // This worker is spawned by a test that already holds the lock.
        // The non-blocking acquisition must fail.
        FileLock lock(resource_path_str, ResourceType::File, LockMode::NonBlocking);
        ASSERT_FALSE(lock.valid());
     }, "filelock::nonblocking_acquire", FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int contention_log_access(const std::string &resource_path_str,
                          const std::string &log_path_str,
                          int num_iterations)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path resource_path(resource_path_str);
            std::filesystem::path log_path(log_path_str);
            unsigned long pid =
#if defined(PLATFORM_WIN64)
                static_cast<unsigned long>(GetCurrentProcessId());
#else
                static_cast<unsigned long>(getpid());
#endif

            for (int i = 0; i < num_iterations; ++i)
            {
                // Random sleep to increase contention likelihood at different points
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 20000));

                FileLock filelock(resource_path, ResourceType::File, LockMode::Blocking);
                ASSERT_TRUE(filelock.valid()) << "Failed to acquire lock, PID: " << pid;

                // Log the timestamp and PID upon acquiring the lock
                auto now_acquire = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                {
                    std::ofstream log_stream(log_path, std::ios::app);
                    ASSERT_TRUE(log_stream.is_open());
                    log_stream << now_acquire << " " << pid << " ACQUIRE\n";
                }

                // Hold the lock for a random duration to simulate work
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 20000 + 50));

                // Log the timestamp and PID upon releasing the lock
                auto now_release = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                 {
                    std::ofstream log_stream(log_path, std::ios::app);
                    ASSERT_TRUE(log_stream.is_open());
                    log_stream << now_release << " " << pid << " RELEASE\n";
                }
            } // Lock is released here by FileLock destructor
        },
        "filelock::contention_log_access",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}

int parent_child_block(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            std::filesystem::path resource_path(resource_path_str);
            // This worker is spawned by a parent process that holds the lock.
            // This blocking call should wait until the parent releases the lock.
            auto start = std::chrono::steady_clock::now();
            FileLock lock(resource_path, ResourceType::File, LockMode::Blocking);
            auto end = std::chrono::steady_clock::now();

            ASSERT_TRUE(lock.valid());
            // Verify that the call actually blocked for a meaningful amount of time.
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            ASSERT_GE(dur.count(), 100);
        },
        "filelock::parent_child_block",
        FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule()
    );
}
} // namespace filelock
} // namespace worker
