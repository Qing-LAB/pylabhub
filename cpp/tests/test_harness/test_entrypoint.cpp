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

// Define the global for the executable path
std::string g_self_exe_path;

namespace fs = std::filesystem;

#include "test_entrypoint.h"
// Project-specific Worker Headers
#include "filelock_workers.h"
#include "jsonconfig_workers.h"
#include "logger_workers.h"


using namespace pylabhub::tests::worker; // Added to simplify worker function calls

int main(int argc, char **argv) {
    // Handle worker process modes first
    if (argc > 1) {
        std::string mode_str = argv[1];
        size_t dot_pos = mode_str.find('.');
        if (dot_pos != std::string::npos) {
            std::string module = mode_str.substr(0, dot_pos);
            std::string scenario = mode_str.substr(dot_pos + 1);

            if (module == "filelock") {
                if (scenario == "nonblocking_acquire" && argc > 2) {
                    return filelock::nonblocking_acquire(argv[2]);
                }
                if (scenario == "contention_log_access" && argc > 4) { // exe, mode, resource, log, iters
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
    // If mode not recognized, fall through to running tests.
    // Wrap the main test run in a lifecycle guard to ensure proper init/shutdown.
    // This is the parent test runner process. It needs all modules that could
    // be used by any of its child worker processes.

    if (argc >= 1) g_self_exe_path = argv[0];
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    return result;
}