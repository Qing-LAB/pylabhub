#pragma once

#include <string>

/**
 * @file spinlock_workers.h
 * @brief Declares worker functions for SharedSpinLock multi-process tests.
 */
namespace pylabhub::tests::worker
{
namespace spinlock
{
/**
 * @brief Worker: attach to shm, acquire lock, release, exit.
 * Used to verify cross-process mutual exclusion (main creates shm, worker attaches and uses lock).
 */
int multiprocess_acquire_release(const std::string &shm_name);

/**
 * @brief Worker: attach to shm, acquire lock, exit without releasing (zombie).
 * Main process should be able to reclaim the lock via is_process_alive + force reclaim.
 */
int zombie_hold_lock(const std::string &shm_name);
} // namespace spinlock
} // namespace pylabhub::tests::worker
