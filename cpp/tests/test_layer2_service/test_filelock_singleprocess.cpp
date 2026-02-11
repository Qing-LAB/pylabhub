// tests/test_layer2_service/test_filelock_singleprocess.cpp
/**
 * @file test_filelock_singleprocess.cpp
 * @brief Pattern 2 examples for FileLock tests that don't need multi-process.
 *
 * These tests demonstrate the more efficient Pattern 2 approach for testing
 * FileLock functionality that doesn't require true inter-process communication.
 *
 * Key differences from test_pylabhub_utils/test_filelock.cpp:
 * - No WorkerProcess spawning (faster execution)
 * - Tests run in same process with lifecycle initialized in main()
 * - Suitable for thread safety, basic API, and single-process scenarios
 *
 * For true multi-process IPC tests, see test_filelock.cpp (Pattern 3).
 */

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace pylabhub::utils;
using namespace std::chrono_literals;

/**
 * @brief Test fixture for single-process FileLock tests.
 * @details Initializes the FileLock and Logger lifecycle modules once for the
 *          entire suite (SetUpTestSuite/TearDownTestSuite), then each test runs
 *          in the same process with those modules active.
 */
class FileLockSingleProcessTest : public ::testing::Test
{
  protected:
    static std::unique_ptr<LifecycleGuard> s_lifecycle;

    static void SetUpTestSuite()
    {
        s_lifecycle = std::make_unique<LifecycleGuard>(
            MakeModDefList(FileLock::GetLifecycleModule(), Logger::GetLifecycleModule()));
    }

    static void TearDownTestSuite() { s_lifecycle.reset(); }

    std::vector<fs::path> paths_to_clean_;

    void TearDown() override
    {
        // Clean up test files and lock files
        for (const auto &p : paths_to_clean_)
        {
            try
            {
                if (fs::exists(p))
                    fs::remove(p);

                // Also try to remove .lock file
                auto lock_file = p.parent_path() / (".lock." + p.filename().string());
                if (fs::exists(lock_file))
                    fs::remove(lock_file);
            }
            catch (...)
            {
                // Best-effort cleanup
            }
        }
    }

    fs::path GetTempLockPath(const std::string &test_name)
    {
        auto p = fs::temp_directory_path() / ("pylabhub_filelock_sp_" + test_name + ".txt");
        paths_to_clean_.push_back(p);

        // Ensure clean state
        try
        {
            if (fs::exists(p))
                fs::remove(p);

            auto lock_file = p.parent_path() / (".lock." + p.filename().string());
            if (fs::exists(lock_file))
                fs::remove(lock_file);
        }
        catch (...)
        {
        }

        return p;
    }
};

// Static member definition
std::unique_ptr<LifecycleGuard> FileLockSingleProcessTest::s_lifecycle;

// ============================================================================
// Pattern 2: Single-Process Tests
// ============================================================================

/**
 * @brief Tests basic non-blocking lock acquire/release.
 * @details Pattern 2 - No multi-process needed, just testing basic API.
 */
TEST_F(FileLockSingleProcessTest, BasicNonBlocking)
{
    auto resource_path = GetTempLockPath("basic_nonblocking");

    // Acquire lock
    {
        FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid()) << "Failed to acquire lock: " << lock.error_code().message();
        EXPECT_TRUE(lock.valid());
    } // Lock released here

    // Can acquire again after release
    FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
    ASSERT_TRUE(lock2.valid());
    EXPECT_TRUE(lock2.valid());
}

/**
 * @brief Tests blocking lock with timeout.
 * @details Pattern 2 - Uses thread to test timeout, no separate process needed.
 */
TEST_F(FileLockSingleProcessTest, BlockingLockTimeout)
{
    auto resource_path = GetTempLockPath("blocking_timeout");

    // Main thread holds lock
    FileLock main_lock(resource_path, ResourceType::File);
    ASSERT_TRUE(main_lock.valid());

    // Spawn thread that tries to acquire with timeout
    std::atomic<bool> acquired{false};
    std::atomic<bool> timed_out{false};

    std::thread t(
        [&]()
        {
            auto start = std::chrono::steady_clock::now();

            FileLock lock(resource_path, ResourceType::File, std::chrono::milliseconds(100));

            auto elapsed = std::chrono::steady_clock::now() - start;

            acquired = lock.valid();
            timed_out = !acquired && elapsed >= std::chrono::milliseconds(100);

            EXPECT_FALSE(acquired) << "Lock should not be acquired (main thread holds it)";
            EXPECT_TRUE(timed_out) << "Lock should timeout after 100ms";
        });

    t.join();

    EXPECT_FALSE(acquired.load());
    EXPECT_TRUE(timed_out.load());
}

/**
 * @brief Tests multi-threaded lock contention within single process.
 * @details Pattern 2 - Thread safety test, not multi-process IPC.
 */
TEST_F(FileLockSingleProcessTest, MultiThreadedContention)
{
    auto resource_path = GetTempLockPath("multithread_contention");

    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    // Spawn 10 threads that all try to acquire lock non-blocking
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i)
    {
        threads.emplace_back(
            [&, i]()
            {
                auto lock_opt =
                    FileLock::try_lock(resource_path, ResourceType::File, LockMode::NonBlocking);

                if (lock_opt.has_value())
                {
                    success_count++;
                    LOGGER_DEBUG("Thread {} acquired lock", i);
                    std::this_thread::sleep_for(10ms); // Hold briefly
                }
                else
                {
                    fail_count++;
                    LOGGER_DEBUG("Thread {} failed to acquire lock", i);
                }
            });
    }

    // Wait for all threads
    for (auto &t : threads)
    {
        t.join();
    }

    // Exactly one thread should succeed, others should fail
    EXPECT_EQ(success_count.load(), 1) << "Only one thread should acquire lock";
    EXPECT_EQ(fail_count.load(), 9) << "Other 9 threads should fail";
}

/**
 * @brief Tests move semantics of FileLock.
 * @details Pattern 2 - Testing RAII and move behavior, no IPC needed.
 */
TEST_F(FileLockSingleProcessTest, MoveSemantics)
{
    auto resource_path1 = GetTempLockPath("move_semantics1");
    auto resource_path2 = GetTempLockPath("move_semantics2");

    // Create lock1
    FileLock lock1(resource_path1, ResourceType::File);
    ASSERT_TRUE(lock1.valid());

    // Move construction: lock2 takes ownership from lock1
    FileLock lock2(std::move(lock1));
    EXPECT_TRUE(lock2.valid());

    // Original lock should be invalid after move
    EXPECT_FALSE(lock1.valid());

    // Move assignment: create lock3 on different path, then move lock2 into it
    FileLock lock3(resource_path2, ResourceType::File);
    ASSERT_TRUE(lock3.valid());

    lock3 = std::move(lock2);
    EXPECT_TRUE(lock3.valid());
    EXPECT_FALSE(lock2.valid());
}

/**
 * @brief Tests directory path locking.
 * @details Pattern 2 - Basic directory lock test, no multi-process needed.
 */
TEST_F(FileLockSingleProcessTest, DirectoryPathLocking)
{
    auto dir_path = fs::temp_directory_path() / "pylabhub_test_dir_lock";
    paths_to_clean_.push_back(dir_path);

    // Create directory if it doesn't exist
    fs::create_directories(dir_path);

    // Lock directory
    FileLock dir_lock(dir_path, ResourceType::Directory);
    ASSERT_TRUE(dir_lock.valid()) << "Failed to lock directory: "
                                  << dir_lock.error_code().message();

    // Try to acquire again in same process (should fail)
    auto lock_opt = FileLock::try_lock(dir_path, ResourceType::Directory, LockMode::NonBlocking);
    EXPECT_FALSE(lock_opt.has_value()) << "Should not acquire same directory lock twice";
}

/**
 * @brief Tests timed lock behavior.
 * @details Pattern 2 - Testing timeout with threads, not processes.
 */
TEST_F(FileLockSingleProcessTest, TimedLock)
{
    auto resource_path = GetTempLockPath("timed_lock");

    // Main thread holds lock
    FileLock main_lock(resource_path, ResourceType::File);
    ASSERT_TRUE(main_lock.valid());

    // Thread tries timed acquisition
    std::atomic<bool> acquired{false};

    std::thread t(
        [&]()
        {
            auto start = std::chrono::steady_clock::now();

            // Try to acquire with 50ms timeout
            auto lock_opt = FileLock::try_lock(resource_path, ResourceType::File,
                                               std::chrono::milliseconds(50));

            auto elapsed = std::chrono::steady_clock::now() - start;

            acquired = lock_opt.has_value();

            EXPECT_FALSE(acquired);
            EXPECT_GE(elapsed, std::chrono::milliseconds(50));
            EXPECT_LT(elapsed, std::chrono::milliseconds(200)); // Shouldn't wait too long
        });

    t.join();
    EXPECT_FALSE(acquired.load());
}

/**
 * @brief Tests lock acquire after release within same process.
 * @details Pattern 2 - Testing lock lifecycle, no IPC needed.
 */
TEST_F(FileLockSingleProcessTest, SequentialAcquireRelease)
{
    auto resource_path = GetTempLockPath("sequential");

    for (int i = 0; i < 5; ++i)
    {
        FileLock lock(resource_path, ResourceType::File);
        ASSERT_TRUE(lock.valid()) << "Iteration " << i << " failed to acquire";
        // Lock released at end of scope
    }
}

// ============================================================================
// Pattern 1: Pure API Tests (no I/O, no lifecycle)
// ============================================================================

/**
 * @brief Tests invalid resource path handling.
 * @details Pattern 1 - Pure API test, no actual I/O or logging.
 */
TEST_F(FileLockSingleProcessTest, InvalidResourcePath)
{
    // Path with null character (invalid on most filesystems)
    const char invalid_p[] = "invalid\0path.txt";
    fs::path invalid_path(std::string_view(invalid_p, sizeof(invalid_p) - 1));

    // Constructor should not throw but lock should be invalid
    FileLock lock(invalid_path, ResourceType::File, LockMode::NonBlocking);
    ASSERT_FALSE(lock.valid());
    EXPECT_TRUE(lock.error_code()) << "Expected a non-empty error code";

    // try_lock should return nullopt
    auto lock_opt = FileLock::try_lock(invalid_path, ResourceType::File, LockMode::NonBlocking);
    ASSERT_FALSE(lock_opt.has_value());
}
