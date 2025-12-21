#pragma once
#include <string>

// --- Worker functions for tests ---

// Declared in test_filelock.cpp
int worker_main_nonblocking_test(const std::string& resource_path_str);
int worker_main_blocking_contention(const std::string& counter_path_str, int num_iterations);

// Declared in test_jsonconfig.cpp
int jsonconfig_worker_main(const std::string& cfgpath, const std::string& worker_id);

// Declared in test_logger.cpp
void multiproc_child_main(int msg_count);