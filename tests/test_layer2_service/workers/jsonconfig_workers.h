// tests/test_harness/jsonconfig_workers.h
#pragma once
#include <string>

/**
 * @file jsonconfig_workers.h
 * @brief Declares worker functions for JsonConfig multi-process tests.
 *
 * These functions are designed to be executed in separate processes to test
 * the cross-process functionality of the JsonConfig utility, particularly
 * its file-locking mechanism for safe concurrent writes.
 */
namespace pylabhub::tests::worker
{
namespace jsonconfig
{
/**
 * @brief Worker function invoked as a child process by multi-process tests.
 *
 * This worker attempts to acquire a write lock on a JSON configuration file,
 * add its unique ID to the file, and save it. This is used to test
 * whether multiple processes can safely write to the same file without data loss.
 *
 * @param cfgpath Path to the JSON configuration file.
 * @param worker_id A unique identifier for this worker, used as a key in the JSON.
 * @return 0 on success, non-zero on failure.
 */
int write_id(const std::string &cfgpath, const std::string &worker_id);

/**
 * @brief Worker function to test the behavior of an uninitialized JsonConfig object.
 *
 * This worker verifies that all operations on a default-constructed, uninitialized
 * `JsonConfig` object fail gracefully as expected.
 *
 * @return 0 on success, non-zero on failure.
 */
int uninitialized_behavior();

/**
 * @brief Worker to test the unconsumed transaction proxy warning.
 *
 * This worker constructs a JsonConfig object, creates a transaction proxy,
 * and immediately destroys it without calling .read() or .write(). This is
 * expected to trigger a warning in debug builds.
 *
 * @return Returns 0, but is expected to write a warning to stderr in debug builds.
 */
int not_consuming_proxy();

// ── Pattern 3 conversions of the previously in-process JsonConfigTest ──
// Each takes a parent-provided unique temp directory; the parent removes
// it after wait_for_exit. Workers own their LifecycleGuard via
// run_gtest_worker (Logger + FileLock + JsonConfig).

int init_and_create(const std::string &dir);
int init_with_empty_path_fails();
int init_with_non_existent_file(const std::string &dir);
int basic_accessors(const std::string &dir);
int reload_on_disk_change(const std::string &dir);
int simplified_api_overloads(const std::string &dir);
int recursion_guard(const std::string &dir);
int write_transaction_rolls_back_on_exception(const std::string &dir);
int load_malformed_file(const std::string &dir);
int multi_thread_file_contention(const std::string &dir);
int symlink_attack_prevention_posix(const std::string &dir);
int symlink_attack_prevention_windows(const std::string &dir);
int multi_thread_shared_object_contention(const std::string &dir);
int manual_locking_api(const std::string &dir);
int move_semantics(const std::string &dir);
int overwrite_method(const std::string &dir);
int dirty_flag_logic(const std::string &dir);
int write_veto_commit(const std::string &dir);
int write_produces_invalid_json(const std::string &dir);

} // namespace jsonconfig
} // namespace pylabhub::tests::worker
