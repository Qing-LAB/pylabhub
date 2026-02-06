#include "gtest/gtest.h"
#include "utils/data_block_mutex.hpp"
#include "test_process_utils.h" // For multi-process testing utilities
#include "test_entrypoint.h" // For g_self_exe_path

#if !defined(PYLABHUB_PLATFORM_WIN64)
#include <sys/mman.h> // For shm_unlink
#endif

// Define a unique shared memory name for these tests
#define TEST_SHM_NAME "test_management_mutex_shm"

class DataBlockManagementMutexTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Ensure the shared memory segment is unlinked before each test
        // in case a previous test crashed or failed to clean up.
        // Note: DataBlockMutex doesn't have a static unlink() method.
        // We need to unlink the shared memory segment itself.
#if !defined(PYLABHUB_PLATFORM_WIN64)
        shm_unlink(TEST_SHM_NAME);
#endif
        // On Windows, named kernel mutexes are automatically cleaned up when all handles close
    }

    void TearDown() override
    {
        // Clean up after each test
#if !defined(PYLABHUB_PLATFORM_WIN64)
        shm_unlink(TEST_SHM_NAME);
#endif
    }
};

TEST_F(DataBlockManagementMutexTest, SingleProcessLockUnlock)
{
    pylabhub::hub::DataBlockMutex mutex(TEST_SHM_NAME, nullptr, 0, true);

    {
        pylabhub::hub::DataBlockLockGuard lock(mutex);
        SUCCEED(); // Lock acquired and released
    }
    SUCCEED(); // Mutex destroyed
}

TEST_F(DataBlockManagementMutexTest, TwoProcessesAcquireSequentially)
{
    // This test ensures that two processes can acquire and release the same mutex sequentially.
    // Process 1: Acquires and holds the mutex.
    // Process 2: Waits for the mutex, then acquires and releases it.

    // Create the mutex in the main process first.
    // This ensures the shared memory for the mutex is set up before workers try to access it.
    pylabhub::hub::DataBlockMutex initial_mutex(TEST_SHM_NAME, nullptr, 0, true);

    // Spawn the first worker process that will acquire and release the mutex
    pylabhub::tests::helper::WorkerProcess worker1(
        g_self_exe_path, "datablock_management_mutex.acquire_and_release", {TEST_SHM_NAME});

    // Spawn the second worker process that will also acquire and release the mutex
    pylabhub::tests::helper::WorkerProcess worker2(
        g_self_exe_path, "datablock_management_mutex.acquire_and_release", {TEST_SHM_NAME});

    {
        // Main process acquires the mutex to ensure it's functional
        pylabhub::hub::DataBlockLockGuard main_lock(initial_mutex);

        // Let the main process hold the lock for a bit to allow workers to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    } // main_lock releases here automatically

    // Wait for worker1 to finish and check its exit code
    int exit_code1 = worker1.wait_for_exit();
    pylabhub::tests::helper::expect_worker_ok(worker1);
    ASSERT_EQ(exit_code1, 0) << "Worker 1 failed with stderr:\n" << worker1.get_stderr();

    // Wait for worker2 to finish and check its exit code
    int exit_code2 = worker2.wait_for_exit();
    pylabhub::tests::helper::expect_worker_ok(worker2);
    ASSERT_EQ(exit_code2, 0) << "Worker 2 failed with stderr:\n" << worker2.get_stderr();

    // Re-acquire the mutex in the main process to ensure it's still functional
    {
        pylabhub::hub::DataBlockLockGuard re_main_lock(initial_mutex);
        SUCCEED(); // Successfully re-acquired
    }
}