#include "test_preamble.h" // New common preamble
#include <fstream>
#include <iomanip>

#include "worker_filelock.h"     // Keep this specific header
#include "shared_test_helpers.h" // Keep this specific helper header
#include "test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace
using namespace test_utils;

namespace worker
{
namespace filelock
{

// Prototypes for helpers

int test_basic_non_blocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            {
                FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock.valid());
                ASSERT_FALSE(lock.error_code());

                FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
                ASSERT_FALSE(lock2.valid());
            }
            FileLock lock3(resource_path, ResourceType::File, LockMode::NonBlocking);
            ASSERT_TRUE(lock3.valid());
        },
        "filelock::test_basic_non_blocking");
}

int test_blocking_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            std::atomic<bool> thread_valid{false};
            std::atomic<bool> thread_saw_block{false};

            auto main_lock =
                std::make_unique<FileLock>(resource_path, ResourceType::File, LockMode::Blocking);
            ASSERT_TRUE(main_lock->valid());

            std::thread t1([&]() {
                auto start = std::chrono::steady_clock::now();
                FileLock thread_lock(resource_path, ResourceType::File, LockMode::Blocking);
                auto end = std::chrono::steady_clock::now();
                if (thread_lock.valid()) thread_valid.store(true);
                if (std::chrono::duration_cast<std::chrono::milliseconds>(end - start) > 100ms)
                    thread_saw_block.store(true);
            });

            std::this_thread::sleep_for(200ms);
            main_lock.reset(); // Release lock
            t1.join();

            ASSERT_TRUE(thread_valid.load());
            ASSERT_TRUE(thread_saw_block.load());
        },
        "filelock::test_blocking_lock");
}

int test_timed_lock(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            {
                FileLock main_lock(resource_path, ResourceType::File, LockMode::Blocking);
                ASSERT_TRUE(main_lock.valid());

                auto start = std::chrono::steady_clock::now();
                FileLock timed_lock_fail(resource_path, ResourceType::File, 100ms);
                auto end = std::chrono::steady_clock::now();

                ASSERT_FALSE(timed_lock_fail.valid());
                ASSERT_EQ(timed_lock_fail.error_code(), std::errc::timed_out);
                ASSERT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(end - start),
                          100ms);
            }

            FileLock timed_lock_succeed(resource_path, ResourceType::File, 100ms);
            ASSERT_TRUE(timed_lock_succeed.valid());
        },
        "filelock::test_timed_lock");
}

int test_move_semantics(const std::string &resource1_str, const std::string &resource2_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource1(resource1_str);
            fs::path resource2(resource2_str);

            {
                FileLock lock1(resource1, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock1.valid());
                FileLock lock2(std::move(lock1));
                ASSERT_TRUE(lock2.valid());
                ASSERT_FALSE(lock1.valid());
            }
            {
                FileLock lock1_again(resource1, ResourceType::File, LockMode::NonBlocking);
                ASSERT_TRUE(lock1_again.valid());
            }
        },
        "filelock::test_move_semantics");
}

int test_directory_creation(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path new_dir(base_dir_str);
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
        },
        "filelock::test_directory_creation");
}

int test_directory_path_locking(const std::string &base_dir_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path base_dir(base_dir_str);
            auto dir_to_lock = base_dir / "dir_to_lock";
            fs::create_directories(dir_to_lock);

            auto expected_dir_lock_file =
                FileLock::get_expected_lock_fullname_for(dir_to_lock, ResourceType::Directory);
            FileLock lock(dir_to_lock, ResourceType::Directory, LockMode::NonBlocking);
            ASSERT_TRUE(lock.valid());
            ASSERT_TRUE(fs::exists(expected_dir_lock_file));
        },
        "filelock::test_directory_path_locking");
}

int test_multithreaded_non_blocking(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            const int THREADS = 32;
            std::atomic<int> success_count{0};
            std::vector<std::thread> threads;
            threads.reserve(THREADS);
            for (int i = 0; i < THREADS; ++i)
            {
                threads.emplace_back([&, i]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
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
        },
        "filelock::test_multithreaded_non_blocking");
}

int nonblocking_acquire(const std::string &resource_path_str)
{
    return run_gtest_worker( [&]() {
        FileLock lock(resource_path_str, ResourceType::File, LockMode::NonBlocking);
        ASSERT_FALSE(lock.valid());
     }, "filelock::nonblocking_acquire");
}

int contention_log_access(const std::string &resource_path_str,
                          const std::string &log_path_str,
                          int num_iterations)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            fs::path log_path(log_path_str);
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

                auto now_acquire = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                {
                    std::ofstream log_stream(log_path, std::ios::app);
                    ASSERT_TRUE(log_stream.is_open());
                    log_stream << now_acquire << " " << pid << " ACQUIRE\n";
                }

                // Hold the lock for a bit
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 20000 + 50));

                auto now_release = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                 {
                    std::ofstream log_stream(log_path, std::ios::app);
                    ASSERT_TRUE(log_stream.is_open());
                    log_stream << now_release << " " << pid << " RELEASE\n";
                }
            } // Lock is released here by FileLock destructor
        },
        "filelock::contention_log_access");
}

int parent_child_block(const std::string &resource_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path resource_path(resource_path_str);
            auto start = std::chrono::steady_clock::now();
            FileLock lock(resource_path, ResourceType::File, LockMode::Blocking);
            auto end = std::chrono::steady_clock::now();
            ASSERT_TRUE(lock.valid());
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            ASSERT_GE(dur.count(), 100);
        },
        "filelock::parent_child_block");
}

// --- Helper implementations that are not tests themselves ---

} // namespace filelock
} // namespace worker
