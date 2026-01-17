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

} // namespace jsonconfig
} // namespace pylabhub::tests::worker
