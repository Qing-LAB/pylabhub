#pragma once
#include <string>

// Declares worker functions for FileLock multi-process tests.
namespace pylabhub::tests::worker
{
    namespace filelock
    {
        // --- Existing workers for multiprocess tests ---
        int nonblocking_acquire(const std::string& resource_path_str);
        int contention_log_access(const std::string &resource_path_str,
                                  const std::string &log_path_str,
                                  int num_iterations);
        int parent_child_block(const std::string& resource_path_str);

        // --- NEW workers for single-process tests converted to multiprocess ---
        int test_basic_non_blocking(const std::string& resource_path_str);
        int test_blocking_lock(const std::string& resource_path_str);
        int test_timed_lock(const std::string& resource_path_str);
        int test_move_semantics(const std::string& resource1_str, const std::string& resource2_str);
        int test_directory_creation(const std::string& base_dir_str);
        int test_directory_path_locking(const std::string& base_dir_str);
        int test_multithreaded_non_blocking(const std::string& resource_path_str);
    }
}