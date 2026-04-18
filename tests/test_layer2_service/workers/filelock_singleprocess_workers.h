#pragma once
/**
 * @file filelock_singleprocess_workers.h
 * @brief Workers for the in-process FileLock test suite (Pattern 3).
 *
 * The previous in-process suite kept a process-wide LifecycleGuard alive in
 * SetUpTestSuite. Per the test framework contract, every body that
 * constructs a FileLock now runs in its own subprocess. The parent only
 * generates a unique resource path and removes the file (and its .lock /
 * .dir.lock siblings) after wait_for_exit.
 */

#include <string>

namespace pylabhub::tests::worker
{
namespace filelock_singleprocess
{

/** Acquire then release; verify second acquire on the same path succeeds. */
int basic_nonblocking(const std::string &resource_path);

/** Holder thread keeps lock; another thread's blocking ctor must time out. */
int blocking_lock_timeout(const std::string &resource_path);

/** 10 threads race try_lock(non-blocking); exactly one succeeds. */
int multi_threaded_contention(const std::string &resource_path);

/** Move-construct + move-assign FileLock; old object becomes invalid. */
int move_semantics(const std::string &resource_path1, const std::string &resource_path2);

/** Lock a directory; subsequent same-process try_lock must fail. */
int directory_path_locking(const std::string &dir_path);

/** Holder holds lock; FileLock::try_lock with 50 ms timeout must return nullopt. */
int timed_lock(const std::string &resource_path);

/** Acquire+release in a loop on the same path. */
int sequential_acquire_release(const std::string &resource_path);

/** get_expected_lock_fullname_for produces .lock and .dir.lock variants. */
int get_expected_lock_fullname_for(const std::string &file_path,
                                   const std::string &dir_path);

/** Valid lock exposes locked_resource + canonical_lock_file paths. */
int get_locked_resource_and_canonical_lock_path(const std::string &resource_path);

/** Invalid path → invalid lock → both path getters return nullopt. */
int get_paths_return_empty_when_invalid();

/** Invalid path → ctor non-throwing, valid()==false, error_code set. */
int invalid_resource_path();

} // namespace filelock_singleprocess
} // namespace pylabhub::tests::worker
