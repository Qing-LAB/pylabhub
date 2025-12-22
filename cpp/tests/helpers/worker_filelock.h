#pragma once
#include <string>

// Declares worker functions for FileLock multi-process tests.
namespace worker
{
    namespace filelock
    {
        int nonblocking_acquire(const std::string& resource_path_str);
        int contention_increment(const std::string& counter_path_str, int num_iterations);
        int parent_child_block(const std::string& resource_path_str);
    }
}
