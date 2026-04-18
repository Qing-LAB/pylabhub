/**
 * @file filelock_singleprocess_workers.cpp
 * @brief Worker implementations for in-process FileLock tests (Pattern 3).
 *
 * Each worker wraps its body in run_gtest_worker(); the LifecycleGuard
 * (Logger + FileLock) is owned exclusively by the subprocess.
 */
#include "filelock_singleprocess_workers.h"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/file_lock.hpp"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::Logger;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace pylabhub::tests::worker
{
namespace filelock_singleprocess
{

int basic_nonblocking(const std::string &resource_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path p(resource_path);
            {
                FileLock lock(p, /*is_directory=*/false, /*blocking=*/false);
                ASSERT_TRUE(lock.valid())
                    << "first acquire failed: " << lock.error_code().message();
            }
            FileLock lock2(p, /*is_directory=*/false, /*blocking=*/false);
            EXPECT_TRUE(lock2.valid());
        },
        "filelock_singleprocess::basic_nonblocking",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int blocking_lock_timeout(const std::string &resource_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path p(resource_path);
            FileLock main_lock(p);
            ASSERT_TRUE(main_lock.valid());

            std::atomic<bool> acquired{false};
            std::atomic<bool> timed_out{false};
            std::thread t([&]() {
                auto start = std::chrono::steady_clock::now();
                FileLock lock(p, /*is_directory=*/false,
                              std::chrono::milliseconds(100));
                auto elapsed = std::chrono::steady_clock::now() - start;
                acquired = lock.valid();
                timed_out = !acquired && elapsed >= std::chrono::milliseconds(100);
            });
            t.join();
            EXPECT_FALSE(acquired.load());
            EXPECT_TRUE(timed_out.load());
        },
        "filelock_singleprocess::blocking_lock_timeout",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int multi_threaded_contention(const std::string &resource_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path p(resource_path);
            std::atomic<int> success_count{0};
            std::atomic<int> fail_count{0};
            constexpr int    kThreads = 10;
            std::barrier     completion(kThreads + 1);

            std::vector<std::thread> threads;
            for (int i = 0; i < kThreads; ++i)
            {
                threads.emplace_back([&, i]() {
                    auto lock_opt = FileLock::try_lock(
                        p, /*is_directory=*/false, /*blocking=*/false);
                    if (lock_opt.has_value())
                    {
                        success_count++;
                        std::this_thread::sleep_for(10ms);
                    }
                    else
                    {
                        fail_count++;
                    }
                    (void)i;
                    completion.arrive_and_wait();
                });
            }
            completion.arrive_and_wait();
            for (auto &t : threads)
                t.join();

            EXPECT_EQ(success_count.load(), 1);
            EXPECT_EQ(fail_count.load(), kThreads - 1);
        },
        "filelock_singleprocess::multi_threaded_contention",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int move_semantics(const std::string &p1, const std::string &p2)
{
    return run_gtest_worker(
        [&]()
        {
            // Brace-init avoids the most-vexing-parse — `FileLock lock1(fs::path(p1));`
            // would otherwise be parsed as a function declaration.
            const fs::path path1{p1};
            const fs::path path2{p2};
            FileLock lock1{path1};
            ASSERT_TRUE(lock1.valid());

            FileLock lock2{std::move(lock1)};
            EXPECT_TRUE(lock2.valid());
            EXPECT_FALSE(lock1.valid());

            FileLock lock3{path2};
            ASSERT_TRUE(lock3.valid());

            lock3 = std::move(lock2);
            EXPECT_TRUE(lock3.valid());
            EXPECT_FALSE(lock2.valid());
        },
        "filelock_singleprocess::move_semantics",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int directory_path_locking(const std::string &dir_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path d(dir_path);
            fs::create_directories(d);

            FileLock dir_lock(d, /*is_directory=*/true);
            ASSERT_TRUE(dir_lock.valid())
                << "failed to lock dir: " << dir_lock.error_code().message();

            auto lock_opt = FileLock::try_lock(d, /*is_directory=*/true,
                                               /*blocking=*/false);
            EXPECT_FALSE(lock_opt.has_value());
        },
        "filelock_singleprocess::directory_path_locking",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int timed_lock(const std::string &resource_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path p(resource_path);
            FileLock main_lock(p);
            ASSERT_TRUE(main_lock.valid());

            std::atomic<bool> acquired{false};
            std::thread t([&]() {
                auto start    = std::chrono::steady_clock::now();
                auto lock_opt = FileLock::try_lock(p, /*is_directory=*/false,
                                                   std::chrono::milliseconds(50));
                auto elapsed  = std::chrono::steady_clock::now() - start;
                acquired      = lock_opt.has_value();
                EXPECT_FALSE(acquired);
                EXPECT_GE(elapsed, std::chrono::milliseconds(50));
                EXPECT_LT(elapsed, std::chrono::milliseconds(200));
            });
            t.join();
            EXPECT_FALSE(acquired.load());
        },
        "filelock_singleprocess::timed_lock",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int sequential_acquire_release(const std::string &resource_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path p(resource_path);
            for (int i = 0; i < 5; ++i)
            {
                FileLock lock(p);
                ASSERT_TRUE(lock.valid()) << "iteration " << i;
            }
        },
        "filelock_singleprocess::sequential_acquire_release",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int get_expected_lock_fullname_for(const std::string &file_path,
                                   const std::string &dir_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path fp(file_path);
            const fs::path dp(dir_path);
            fs::create_directories(dp);

            auto file_lock_path = FileLock::get_expected_lock_fullname_for(fp);
            EXPECT_FALSE(file_lock_path.empty());
            EXPECT_NE(file_lock_path.generic_string().find(".lock"),
                      std::string::npos);
            EXPECT_EQ(file_lock_path.generic_string().find(".dir.lock"),
                      std::string::npos);

            auto dir_lock_path =
                FileLock::get_expected_lock_fullname_for(dp, /*is_directory=*/true);
            EXPECT_FALSE(dir_lock_path.empty());
            EXPECT_NE(dir_lock_path.generic_string().find(".dir.lock"),
                      std::string::npos);
        },
        "filelock_singleprocess::get_expected_lock_fullname_for",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int get_locked_resource_and_canonical_lock_path(const std::string &resource_path)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path p(resource_path);
            FileLock lock(p);
            ASSERT_TRUE(lock.valid());

            auto locked = lock.get_locked_resource_path();
            ASSERT_TRUE(locked.has_value());
            EXPECT_FALSE(locked->empty());

            auto canonical = lock.get_canonical_lock_file_path();
            ASSERT_TRUE(canonical.has_value());
            EXPECT_FALSE(canonical->empty());
            EXPECT_NE(canonical->generic_string().find(".lock"),
                      std::string::npos);
        },
        "filelock_singleprocess::get_locked_resource_and_canonical_lock_path",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int get_paths_return_empty_when_invalid()
{
    return run_gtest_worker(
        [&]()
        {
            const char invalid_p[] = "invalid\0path.txt";
            fs::path invalid_path(std::string_view(invalid_p, sizeof(invalid_p) - 1));
            FileLock lock(invalid_path, /*is_directory=*/false,
                          /*blocking=*/false);
            ASSERT_FALSE(lock.valid());
            EXPECT_FALSE(lock.get_locked_resource_path().has_value());
            EXPECT_FALSE(lock.get_canonical_lock_file_path().has_value());
        },
        "filelock_singleprocess::get_paths_return_empty_when_invalid",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

int invalid_resource_path()
{
    return run_gtest_worker(
        [&]()
        {
            const char invalid_p[] = "invalid\0path.txt";
            fs::path invalid_path(std::string_view(invalid_p, sizeof(invalid_p) - 1));

            FileLock lock(invalid_path, /*is_directory=*/false,
                          /*blocking=*/false);
            ASSERT_FALSE(lock.valid());
            EXPECT_TRUE(lock.error_code());

            auto lock_opt = FileLock::try_lock(invalid_path,
                                               /*is_directory=*/false,
                                               /*blocking=*/false);
            ASSERT_FALSE(lock_opt.has_value());
        },
        "filelock_singleprocess::invalid_resource_path",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule());
}

} // namespace filelock_singleprocess
} // namespace pylabhub::tests::worker

namespace
{

struct FileLockSingleProcessWorkerRegistrar
{
    FileLockSingleProcessWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "filelock_singleprocess")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::filelock_singleprocess;

                auto need = [&](int n) -> bool {
                    if (argc < n) {
                        fmt::print(stderr,
                                   "filelock_singleprocess.{}: expected {} args\n",
                                   sc, n - 2);
                        return false;
                    }
                    return true;
                };

                if (sc == "basic_nonblocking" && need(3))
                    return basic_nonblocking(argv[2]);
                if (sc == "blocking_lock_timeout" && need(3))
                    return blocking_lock_timeout(argv[2]);
                if (sc == "multi_threaded_contention" && need(3))
                    return multi_threaded_contention(argv[2]);
                if (sc == "move_semantics" && need(4))
                    return move_semantics(argv[2], argv[3]);
                if (sc == "directory_path_locking" && need(3))
                    return directory_path_locking(argv[2]);
                if (sc == "timed_lock" && need(3))
                    return timed_lock(argv[2]);
                if (sc == "sequential_acquire_release" && need(3))
                    return sequential_acquire_release(argv[2]);
                if (sc == "get_expected_lock_fullname_for" && need(4))
                    return get_expected_lock_fullname_for(argv[2], argv[3]);
                if (sc == "get_locked_resource_and_canonical_lock_path" && need(3))
                    return get_locked_resource_and_canonical_lock_path(argv[2]);
                if (sc == "get_paths_return_empty_when_invalid")
                    return get_paths_return_empty_when_invalid();
                if (sc == "invalid_resource_path")
                    return invalid_resource_path();

                fmt::print(stderr,
                           "[filelock_singleprocess] ERROR: unknown scenario "
                           "'{}'\n",
                           sc);
                return 1;
            });
    }
};
static FileLockSingleProcessWorkerRegistrar g_filelock_sp_registrar;

} // namespace
