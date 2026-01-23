// tests/test_framework/test_entrypoint.h
#pragma once

#include "plh_base.hpp"
#include <gtest/gtest.h>
/**
 * @file test_entrypoint.h
 * @brief Provides an API for registering worker scenario dispatchers.
 */

// Define the global for the executable path, used by worker-spawning tests
extern std::string g_self_exe_path;

/**
 * @brief Type definition for a worker scenario dispatcher function.
 *
 * A dispatcher function takes the same arguments as `main()` and is responsible
 * for parsing worker mode arguments and calling the appropriate worker function.
 * It should return 0 on success, or an error code otherwise.
 */
using WorkerDispatchFn = int (*)(int argc, char **argv);

/**
 * @brief Registers a worker scenario dispatcher function with the test framework.
 *
 * This function allows a specific test executable to "plug in" its worker
 * handling logic into the generic test entry point provided by the framework.
 *
 * @param fn A function pointer to the dispatcher function.
 */
void register_worker_dispatcher(WorkerDispatchFn fn);