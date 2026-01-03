// tests/test_harness/test_entrypoint.h
#pragma once
#include <filesystem>
#include <string>

/**
 * @file test_entrypoint.h
 * @brief Declares global variables used by the test harness.
 */

/**
 * @brief Holds the path to the current running test executable.
 *
 * This global variable is initialized in `main()` and used by test cases that
 * need to spawn the test executable itself as a worker process for multi-process
 * testing. This allows the child process to know which binary to execute.
 */
extern std::string g_self_exe_path;
