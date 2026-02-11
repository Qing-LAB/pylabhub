// tests/test_harness/filelock_workers.cpp
/**
 * @file filelock_workers.cpp
 * @brief Implements worker functions for FileLock tests.
 *
 * These functions are executed in separate processes to test the cross-process
 * functionality of the FileLock utility. Each function encapsulates a specific
 * test scenario and is invoked by the main test runner.
 *
 * FileLock::GetLifecycleModule() is used without a cleanup parameter; the library
 * does not remove .lock files on shutdown (they are harmless if left on disk).
 */
#include <barrier>
#include <fstream>

#include "filelock_workers.h"
#include "test_entrypoint.h"
#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "gtest/gtest.h"

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
        [&]()
        {
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
        "filelock::test_basic_non_blocking", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int test_blocking_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            std::filesystem::path resource_path(resource_path_str);
            std::atomic<bool> thread_valid{false};
            std::atomic<bool> thread_saw_block{false};

            // Main thread acquires a blocking lock
            auto main_lock =
                std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
            ASSERT_TRUE(main_lock->valid());

            // Spawn a second thread that will block trying to acquire the same lock
            std::thread t1(
                [&]()
                {
                    auto start = std::chrono::steady_clock::now();
                    FileLock thread_lock(resource_path, ResourceType::File, LockMode::Blocking);
                    auto end = std::chrono::steady_clock::now();
                    if (thread_lock.valid())
                        thread_valid.store(true);
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
        "filelock::test_blocking_lock", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int test_timed_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]()
        {
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
        "filelock::test_timed_lock", FileLock::GetLifecycleModule(), Logger::GetLifecycleModule());
}

int test_move_semantics(const std::string &resource1_str, const std::string &resource2_str)
{
    return run_gtest_worker(
        [&]()
        {
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
        "filelock::test_move_semantics", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int test_directory_creation(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]()
        {
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
        "filelock::test_directory_creation", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int test_directory_path_locking(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]()
        {
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
        "filelock::test_directory_path_locking", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int test_multithreaded_non_blocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            std::filesystem::path resource_path(resource_path_str);

            constexpr int THREADS = 64;
            constexpr int ITERS = 1000; // outer iterations to expose rare races

            // A reusable barrier for THREADS participants.
            // This barrier synchronizes the start of each iteration's lock attempt.
            std::barrier start_of_iteration_barrier(THREADS);
            // This barrier synchronizes the end of each iteration's lock attempt and recording.
            std::barrier end_of_iteration_barrier(THREADS);

            // Track how many successes occurred in the current iteration.
            std::atomic<int> iter_success_count{0};

            // Vector that records if any iteration had bad result (for debugging)
            std::vector<int> per_iter_success(ITERS, 0);

            // Launch worker threads once; each will loop over iterations
            std::vector<std::thread> threads;
            threads.reserve(THREADS);

            for (int tid = 0; tid < THREADS; ++tid)
            {
                threads.emplace_back(
                    [&, tid]()
                    {
                        for (int iter = 0; iter < ITERS; ++iter)
                        {
                            // Synchronize all threads at the start of each iteration
                            start_of_iteration_barrier.arrive_and_wait();

                            // Attempt the non-blocking lock exactly once this phase
                            FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                            if (lock.valid())
                            {
                                // Record success for this iteration
                                iter_success_count.fetch_add(1, std::memory_order_relaxed);

                                // Hold lock briefly so other threads must fail now
                                std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Reduce sleep to 1ms
                            }
                            // Release the lock at the end of scope
                            // Implicitly released when 'lock' goes out of scope here.

                            // Synchronize all threads at the end of each iteration to ensure
                            // iter_success_count is finalized before being read by tid 0.
                            end_of_iteration_barrier.arrive_and_wait();

                            if (tid == 0) // choose thread 0 as recorder
                            {
                                int observed = iter_success_count.exchange(0, std::memory_order_acq_rel);
                                per_iter_success[iter] = observed;
                            }
                        }
                    });
            }

            // Wait for workers to finish
            for (auto &t : threads)
                t.join();

            // Verify: each iteration should have exactly 1 success
            for (int iter = 0; iter < ITERS; ++iter)
            {
                ASSERT_EQ(per_iter_success[iter], 1)
                    << "iteration " << iter << " had " << per_iter_success[iter] << " successes";
            }
        },
        "filelock::test_multithreaded_non_blocking", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int nonblocking_acquire(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            // This worker is spawned by a test that already holds the lock.
            // The non-blocking acquisition must fail.
            FileLock lock(resource_path_str, ResourceType::File, LockMode::NonBlocking);
            ASSERT_FALSE(lock.valid());
        },
        "filelock::nonblocking_acquire", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int contention_log_access(const std::string &resource_path_str, const std::string &log_path_str,
                          int num_iterations)
{
    return run_gtest_worker(
        [&]()
        {
            pylabhub::utils::Logger::instance().set_level(pylabhub::utils::Logger::Level::L_INFO);
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
                auto now_acquire =
                    std::chrono::high_resolution_clock::now().time_since_epoch().count();
                {
                    std::ofstream log_stream(log_path, std::ios::app);
                    ASSERT_TRUE(log_stream.is_open());
                    log_stream << now_acquire << " " << pid << " ACQUIRE\n";
                }

                // Hold the lock for a random duration to simulate work
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 20000 + 50));

                // Log the timestamp and PID upon releasing the lock
                auto now_release =
                    std::chrono::high_resolution_clock::now().time_since_epoch().count();
                {
                    std::ofstream log_stream(log_path, std::ios::app);
                    ASSERT_TRUE(log_stream.is_open());
                    log_stream << now_release << " " << pid << " RELEASE\n";
                }
            } // Lock is released here by FileLock destructor
        },
        "filelock::contention_log_access", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int parent_child_block(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]()
        {
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
        "filelock::parent_child_block", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

int try_lock_nonblocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            // This worker is spawned by a test that already holds the lock.
            // The non-blocking try_lock must fail and return nullopt.
            auto lock_opt =
                FileLock::try_lock(resource_path_str, ResourceType::File, LockMode::NonBlocking);
            ASSERT_FALSE(lock_opt.has_value());
        },
        "filelock::try_lock_nonblocking", FileLock::GetLifecycleModule(),
        Logger::GetLifecycleModule());
}

} // namespace filelock
} // namespace pylabhub::tests::worker

// Self-registering dispatcher — no separate dispatcher file needed.
namespace
{
struct FileLockWorkerRegistrar
{
    FileLockWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "filelock")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::filelock;
                if (scenario == "nonblocking_acquire" && argc > 2)
                    return nonblocking_acquire(argv[2]);
                if (scenario == "contention_log_access" && argc > 4)
                    return contention_log_access(argv[2], argv[3], std::stoi(argv[4]));
                if (scenario == "parent_child_block" && argc > 2)
                    return parent_child_block(argv[2]);
                if (scenario == "test_basic_non_blocking" && argc > 2)
                    return test_basic_non_blocking(argv[2]);
                if (scenario == "test_blocking_lock" && argc > 2)
                    return test_blocking_lock(argv[2]);
                if (scenario == "test_timed_lock" && argc > 2)
                    return test_timed_lock(argv[2]);
                if (scenario == "test_move_semantics" && argc > 3)
                    return test_move_semantics(argv[2], argv[3]);
                if (scenario == "test_directory_creation" && argc > 2)
                    return test_directory_creation(argv[2]);
                if (scenario == "test_directory_path_locking" && argc > 2)
                    return test_directory_path_locking(argv[2]);
                if (scenario == "test_multithreaded_non_blocking" && argc > 2)
                    return test_multithreaded_non_blocking(argv[2]);
                if (scenario == "try_lock_nonblocking" && argc > 2)
                    return try_lock_nonblocking(argv[2]);
                fmt::print(stderr, "ERROR: Unknown filelock scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static FileLockWorkerRegistrar g_filelock_registrar;
} // namespace
