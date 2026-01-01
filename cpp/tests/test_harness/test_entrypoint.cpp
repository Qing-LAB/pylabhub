// tests/test_harness/test_entrypoint.cpp
/**
 * @file test_entrypoint.cpp
 * @brief Main entry point for test executables using the multi-process harness.
 *
 * This file contains the `main` function that serves a dual purpose:
 * 1. If started with no specific arguments, it acts as a standard GoogleTest
 *    test runner, discovering and executing all linked tests.
 * 2. If started with a "worker mode" argument (e.g., "filelock.nonblocking_acquire"),
 *    it acts as a worker process, dispatching to the corresponding worker function.
 *
 * This design allows a single test executable to spawn copies of itself to run
 * isolated, cross-process test scenarios.
 */
#include "platform.hpp"

// Standard Library
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// Third-party
#include <fmt/core.h>
#include <gtest/gtest.h>

#include "test_entrypoint.h"

// Project-specific Worker Headers
#include "filelock_workers.h"
#include "jsonconfig_workers.h"
#include "logger_workers.h"

// Define the global for the executable path, used by worker-spawning tests
std::string g_self_exe_path;

namespace fs = std::filesystem;

using namespace pylabhub::tests::worker;

int main(int argc, char **argv) {
    // Before running tests, check if the executable was invoked in "worker mode".
    // A worker mode is specified as the first command-line argument, in the
    // format "module.scenario".
    if (argc > 1) {
        std::string mode_str = argv[1];
        size_t dot_pos = mode_str.find('.');
        if (dot_pos != std::string::npos) {
            std::string module = mode_str.substr(0, dot_pos);
            std::string scenario = mode_str.substr(dot_pos + 1);

            // Dispatch to the appropriate worker function based on the module and scenario.
            // The return value of the worker function becomes the exit code of the process.
            if (module == "filelock") {
                if (scenario == "nonblocking_acquire" && argc > 2) {
                    return filelock::nonblocking_acquire(argv[2]);
                }
                if (scenario == "contention_log_access" && argc > 4) {
                    return filelock::contention_log_access(argv[2], argv[3], std::stoi(argv[4]));
                }
                if (scenario == "parent_child_block" && argc > 2) {
                    return filelock::parent_child_block(argv[2]);
                }
                if (scenario == "test_basic_non_blocking" && argc > 2) {
                    return filelock::test_basic_non_blocking(argv[2]);
                }
                if (scenario == "test_blocking_lock" && argc > 2) {
                    return filelock::test_blocking_lock(argv[2]);
                }
                if (scenario == "test_timed_lock" && argc > 2) {
                    return filelock::test_timed_lock(argv[2]);
                }
                if (scenario == "test_move_semantics" && argc > 3) {
                    return filelock::test_move_semantics(argv[2], argv[3]);
                }
                if (scenario == "test_directory_creation" && argc > 2) {
                    return filelock::test_directory_creation(argv[2]);
                }
                if (scenario == "test_directory_path_locking" && argc > 2) {
                    return filelock::test_directory_path_locking(argv[2]);
                }
                if (scenario == "test_multithreaded_non_blocking" && argc > 2) {
                    return filelock::test_multithreaded_non_blocking(argv[2]);
                }
            } else if (module == "jsonconfig") {
                if (scenario == "write_id" && argc > 3) {
                    return jsonconfig::write_id(argv[2], argv[3]);
                }
            } else if (module == "logger") {
                if (scenario == "test_basic_logging" && argc > 2) {
                    return logger::test_basic_logging(argv[2]);
                }
                if (scenario == "test_log_level_filtering" && argc > 2) {
                    return logger::test_log_level_filtering(argv[2]);
                }
                if (scenario == "test_bad_format_string" && argc > 2) {
                    return logger::test_bad_format_string(argv[2]);
                }
                if (scenario == "test_default_sink_and_switching" && argc > 2) {
                    return logger::test_default_sink_and_switching(argv[2]);
                }
                if (scenario == "test_multithread_stress" && argc > 2) {
                    return logger::test_multithread_stress(argv[2]);
                }
                if (scenario == "test_flush_waits_for_queue" && argc > 2) {
                    return logger::test_flush_waits_for_queue(argv[2]);
                }
                if (scenario == "test_shutdown_idempotency" && argc > 2) {
                    return logger::test_shutdown_idempotency(argv[2]);
                }
                if (scenario == "test_reentrant_error_callback" && argc > 2) {
                    return logger::test_reentrant_error_callback(argv[2]);
                }
                if (scenario == "test_write_error_callback_async" && argc > 1) {
                    return logger::test_write_error_callback_async();
                }
                if (scenario == "test_platform_sinks" && argc > 1) {
                    return logger::test_platform_sinks();
                }
                if (scenario == "test_concurrent_lifecycle_chaos" && argc > 2) {
                    return logger::test_concurrent_lifecycle_chaos(argv[2]);
                }
                if (scenario == "stress_log" && argc > 3) {
                    logger::stress_log(argv[2], std::stoi(argv[3]));
                    return 0;
                }
            }
        }
    }

    // If no worker mode was matched, fall through to the standard test runner mode.
    // Store the executable path for tests that need to spawn workers.
    if (argc >= 1) g_self_exe_path = argv[0];

    // Initialize GoogleTest and run all registered tests.
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    return result;
}