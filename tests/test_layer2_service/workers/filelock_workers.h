#pragma once
#include <string>

// tests/test_harness/filelock_workers.h
#pragma once
#include <string>

/**
 * @file filelock_workers.h
 * @brief Declares worker functions for FileLock tests.
 *
 * These functions are designed to be executed in separate processes to test
 * the cross-process functionality of the FileLock utility. They are invoked
 * by the main test executable based on command-line arguments.
 */
namespace pylabhub::tests::worker
{
namespace filelock
{
// --- Multi-process contention and blocking scenarios ---

/**
 * @brief Worker that attempts to acquire a non-blocking lock.
 * Fails if the lock is already held by another process.
 */
int nonblocking_acquire(const std::string &resource_path_str);

/**
 * @brief Worker that repeatedly acquires and releases a blocking lock, logging timestamps.
 * Used to verify that locks are mutually exclusive under contention.
 */
int contention_log_access(const std::string &resource_path_str, const std::string &log_path_str,
                          int num_iterations);

/**
 * @brief Worker that attempts to acquire a blocking lock held by a parent process.
 * Used to test parent-child lock blocking behavior.
 */
int parent_child_block(const std::string &resource_path_str);

// --- Basic FileLock functionality tests, executed in a worker process ---

/**
 * @brief Tests basic non-blocking lock acquisition in a worker process.
 */
int test_basic_non_blocking(const std::string &resource_path_str);

/**
 * @brief Tests blocking lock behavior between threads in a worker process.
 */
int test_blocking_lock(const std::string &resource_path_str);

/**
 * @brief Tests timed lock acquisition in a worker process.
 */
int test_timed_lock(const std::string &resource_path_str);

/**
 * @brief Tests move constructor and move assignment for FileLock in a worker process.
 */
int test_move_semantics(const std::string &resource1_str, const std::string &resource2_str);

/**
 * @brief Tests that the lock directory is created if it doesn't exist.
 */
int test_directory_creation(const std::string &base_dir_str);

/**
 * @brief Tests locking a directory path.
 */
int test_directory_path_locking(const std::string &base_dir_str);

/**

 * @brief Tests non-blocking lock acquisition with multiple threads in a worker process.

 */

int test_multithreaded_non_blocking(const std::string &resource_path_str);

/**

 * @brief Worker that attempts a non-blocking try_lock on a resource.

 * Used to test multi-process contention with the try_lock API.

 */

int try_lock_nonblocking(const std::string &resource_path_str);

/**
 * @brief Worker that creates FileLock without lifecycle initialized.
 * Expected to abort with PLH_PANIC.
 */
int use_without_lifecycle_aborts();

// ── Pattern 3 conversions that replace parent-side FileLock holders ────────
//
// The original suite kept a process-wide LifecycleGuard alive so several
// tests could do `FileLock main_lock(...)` in the gtest runner, then
// spawn a contender worker. Per HEP-CORE-0001 § "Testing implications",
// the parent must not own the lifecycle. The parent now spawns TWO
// workers: lock_holder acquires + signals ready + holds for hold_ms,
// and the existing contender worker (nonblocking_acquire, try_lock_-
// nonblocking, parent_child_block) races the still-held lock.

/**
 * @brief Holder worker: acquires a blocking lock, signals ready, holds
 *        it for @p hold_ms milliseconds, then releases on scope exit.
 *
 * Must be spawned via SpawnWorkerWithReadySignal so the parent can
 * deterministically spawn the contender only after the lock is held.
 */
int lock_holder(const std::string &resource_path_str, int hold_ms);

/**
 * @brief Single-process TryLockPattern body: success + non-blocking
 *        failure + timed failure, all in this subprocess.
 */
int try_lock_pattern(const std::string &resource_path_str);

/**
 * @brief Verifies FileLock rejects paths containing a null byte (returns
 *        invalid lock with error_code == invalid_argument), and the
 *        static try_lock returns nullopt for the same path.
 */
int invalid_resource_path();

} // namespace filelock

} // namespace pylabhub::tests::worker
