/**
 * @file test_shared_memory_spinlock.cpp
 * @brief Tests for SharedSpinLock (utils/shared_memory_spinlock.hpp).
 *
 * Part 0 of DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md: foundational APIs used by DataBlock.
 * Tests: try_lock_for, lock, unlock, timeout, recursion, RAII guards; multi-process acquire/release
 * and zombie reclaim.
 * Cross-platform: must run on all supported platforms (Windows, Linux, macOS, FreeBSD).
 */
#include "plh_platform.hpp"
#include "plh_service.hpp"
#include "utils/shared_memory_spinlock.hpp"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

namespace
{
std::string unique_shm_name_spinlock()
{
    static std::atomic<uint64_t> counter{0};
    uint64_t id = pylabhub::platform::get_pid() * 1000000u + counter.fetch_add(1, std::memory_order_relaxed);
#if defined(PYLABHUB_IS_POSIX)
    return "/pylabhub_test_spinlock_" + std::to_string(id);
#else
    return "pylabhub_test_spinlock_" + std::to_string(id);
#endif
}
} // namespace

// ============================================================================
// Fixture: state in process memory (single-process tests)
// ============================================================================

class SharedSpinLockTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        state_.owner_pid.store(0, std::memory_order_release);
        state_.owner_tid.store(0, std::memory_order_release);
        state_.generation.store(0, std::memory_order_release);
        state_.recursion_count.store(0, std::memory_order_release);
    }

    SharedSpinLockState state_{};
    const std::string name_{"test_spinlock"};
};

// ============================================================================
// try_lock_for / lock / unlock
// ============================================================================

TEST_F(SharedSpinLockTest, TryLockFor_WhenFree_Succeeds)
{
    SharedSpinLock lock(&state_, name_);
    EXPECT_TRUE(lock.try_lock_for(100));
    EXPECT_TRUE(lock.is_locked_by_current_process());
    lock.unlock();
}

TEST_F(SharedSpinLockTest, TryLockFor_WhenHeldBySameThread_RecursiveSucceeds)
{
    SharedSpinLock lock(&state_, name_);
    EXPECT_TRUE(lock.try_lock_for(0));
    EXPECT_TRUE(lock.try_lock_for(0)) << "Recursive lock by same process should succeed";
    lock.unlock();
    lock.unlock();
}

TEST_F(SharedSpinLockTest, Lock_Unlock_Succeeds)
{
    SharedSpinLock lock(&state_, name_);
    lock.lock();
    EXPECT_TRUE(lock.is_locked_by_current_process());
    lock.unlock();
}

TEST_F(SharedSpinLockTest, Unlock_WhenNotOwner_Throws)
{
    SharedSpinLock lock(&state_, name_);
    lock.lock();
    std::thread other([this]() {
        SharedSpinLock l(&state_, name_ + "_other");
        EXPECT_THROW(l.unlock(), std::runtime_error);
    });
    other.join();
    lock.unlock();
}

// ============================================================================
// Timeout
// ============================================================================

TEST_F(SharedSpinLockTest, TryLockFor_WhenHeldByOtherThread_Timeouts)
{
    SharedSpinLock lock(&state_, name_);
    lock.lock();

    std::atomic<bool> try_result{false};
    std::thread contender([this, &try_result]() {
        SharedSpinLock l(&state_, name_ + "_contender");
        try_result = l.try_lock_for(50); // 50 ms timeout
    });

    contender.join();
    EXPECT_FALSE(try_result.load()) << "try_lock_for should timeout when lock is held by another thread";
    lock.unlock();
}

TEST_F(SharedSpinLockTest, TryLockFor_AfterRelease_Succeeds)
{
    SharedSpinLock lock(&state_, name_);
    lock.lock();
    std::atomic<bool> acquired{false};
    std::thread contender([this, &acquired]() {
        SharedSpinLock l(&state_, name_ + "_contender");
        acquired = l.try_lock_for(2000);
    });

    std::this_thread::sleep_for(10ms);
    lock.unlock();
    contender.join();
    EXPECT_TRUE(acquired.load()) << "Contender should acquire after owner releases";
}

// ============================================================================
// SharedSpinLockGuard
// ============================================================================

TEST_F(SharedSpinLockTest, SharedSpinLockGuard_LocksOnConstruction_UnlocksOnDestruction)
{
    {
        SharedSpinLock lock(&state_, name_);
        {
            SharedSpinLockGuard guard(lock);
            EXPECT_TRUE(lock.is_locked_by_current_process());
        }
        EXPECT_FALSE(lock.is_locked_by_current_process());
    }
}

// ============================================================================
// SharedSpinLockGuardOwning
// ============================================================================

TEST_F(SharedSpinLockTest, SharedSpinLockGuardOwning_HoldsLock)
{
    SharedSpinLockGuardOwning guard(&state_, name_);
    SharedSpinLock lock(&state_, name_);
    EXPECT_TRUE(lock.is_locked_by_current_process());
}

TEST_F(SharedSpinLockTest, SharedSpinLockGuardOwning_ReleasesOnDestruction)
{
    {
        SharedSpinLockGuardOwning guard(&state_, name_);
    }
    SharedSpinLock lock(&state_, name_);
    EXPECT_TRUE(lock.try_lock_for(0)) << "Lock should be free after guard destruction";
    lock.unlock();
}

// ============================================================================
// State in shared memory (two threads, state in shm - same as cross-process pattern)
// ============================================================================

TEST(SharedSpinLockShmTest, TwoThreads_StateInShm_MutualExclusion)
{
    std::string shm_name;
#if defined(PYLABHUB_IS_POSIX)
    shm_name = "/pylabhub_test_spinlock_shm_" + std::to_string(pylabhub::platform::get_pid());
#else
    shm_name = "pylabhub_test_spinlock_shm_" + std::to_string(pylabhub::platform::get_pid());
#endif
    const size_t seg_size = sizeof(SharedSpinLockState) + 64;
    pylabhub::platform::ShmHandle h =
        pylabhub::platform::shm_create(shm_name.c_str(), seg_size, pylabhub::platform::SHM_CREATE_UNLINK_FIRST);
    if (!h.base)
    {
        GTEST_SKIP() << "shm_create failed (e.g. CI); skip SharedSpinLock shm test";
    }
    auto *state = new (h.base) SharedSpinLockState();
    state->owner_pid.store(0, std::memory_order_release);
    state->owner_tid.store(0, std::memory_order_release);
    state->generation.store(0, std::memory_order_release);
    state->recursion_count.store(0, std::memory_order_release);

    std::atomic<int> counter{0};
    const int iterations = 50;
    std::thread a([state, &counter, iterations]() {
        SharedSpinLock lock(state, "shm_a");
        for (int i = 0; i < iterations; ++i)
        {
            lock.lock();
            int v = counter.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            counter.store(v + 1, std::memory_order_relaxed);
            lock.unlock();
        }
    });
    std::thread b([state, &counter, iterations]() {
        SharedSpinLock lock(state, "shm_b");
        for (int i = 0; i < iterations; ++i)
        {
            lock.lock();
            int v = counter.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            counter.store(v + 1, std::memory_order_relaxed);
            lock.unlock();
        }
    });
    a.join();
    b.join();
    EXPECT_EQ(counter.load(), 2 * iterations);

    pylabhub::platform::shm_close(&h);
    pylabhub::platform::shm_unlink(shm_name.c_str());
}

// ============================================================================
// Multi-process tests (worker process attaches to shm, uses same SharedSpinLockState)
// ============================================================================

TEST(SharedSpinLockMultiProcessTest, MultiProcess_AcquireRelease)
{
    std::string shm_name = unique_shm_name_spinlock();
    const size_t seg_size = sizeof(SharedSpinLockState) + 64;
    pylabhub::platform::ShmHandle h = pylabhub::platform::shm_create(
        shm_name.c_str(), seg_size, pylabhub::platform::SHM_CREATE_UNLINK_FIRST);
    if (!h.base)
    {
        GTEST_SKIP() << "shm_create failed; skip multi-process test";
    }
    auto *state = new (h.base) SharedSpinLockState();
    state->owner_pid.store(0, std::memory_order_release);
    state->recursion_count.store(0, std::memory_order_release);
    state->generation.store(0, std::memory_order_release);

    WorkerProcess proc(g_self_exe_path, "spinlock.multiprocess_acquire_release", {shm_name});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);

    SharedSpinLock lock(state, "main_after_worker");
    EXPECT_TRUE(lock.try_lock_for(1000)) << "Main should acquire after worker released";
    lock.unlock();

    pylabhub::platform::shm_close(&h);
    pylabhub::platform::shm_unlink(shm_name.c_str());
}

TEST(SharedSpinLockMultiProcessTest, MultiProcess_ZombieReclaim)
{
    std::string shm_name = unique_shm_name_spinlock();
    const size_t seg_size = sizeof(SharedSpinLockState) + 64;
    pylabhub::platform::ShmHandle h = pylabhub::platform::shm_create(
        shm_name.c_str(), seg_size, pylabhub::platform::SHM_CREATE_UNLINK_FIRST);
    if (!h.base)
    {
        GTEST_SKIP() << "shm_create failed; skip multi-process test";
    }
    auto *state = new (h.base) SharedSpinLockState();
    state->owner_pid.store(0, std::memory_order_release);
    state->recursion_count.store(0, std::memory_order_release);
    state->generation.store(0, std::memory_order_release);

    WorkerProcess proc(g_self_exe_path, "spinlock.zombie_hold_lock", {shm_name});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);

    SharedSpinLock lock(state, "main_reclaim");
    EXPECT_TRUE(lock.try_lock_for(5000)) << "Main should reclaim lock after worker exited without unlocking (zombie)";
    lock.unlock();

    pylabhub::platform::shm_close(&h);
    pylabhub::platform::shm_unlink(shm_name.c_str());
}
