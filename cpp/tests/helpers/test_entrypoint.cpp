#include "test_preamble.h"

// Define the global for the executable path
std::string g_self_exe_path;

namespace fs = std::filesystem;

#include "test_entrypoint.h"
#include "workers.h"
#include "test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace
using namespace test_utils;

int main(int argc, char **argv) {
    // --- DIAGNOSTIC ---
    const char* path_env = std::getenv("PATH");
    
    // Handle worker process modes first
    if (argc > 1) {
        std::string mode_str = argv[1];
        size_t dot_pos = mode_str.find('.');
        if (dot_pos != std::string::npos) {
            std::string module = mode_str.substr(0, dot_pos);
            std::string scenario = mode_str.substr(dot_pos + 1);

            if (module == "filelock") {
                /*if (path_env)
                {
                    fmt::print(stderr, "[spawned for filelock tests] PATH={}", path_env);
                }
                else
                {
                    fmt::print(stderr, "[spawned for filelock tests] PATH environment variable not found.");
                }*/

                if (scenario == "nonblocking_acquire" && argc > 2) {
                    return worker::filelock::nonblocking_acquire(argv[2]);
                }
                if (scenario == "contention_log_access" && argc > 4) { // exe, mode, resource, log, iters
                    return worker::filelock::contention_log_access(argv[2], argv[3], std::stoi(argv[4]));
                }
                if (scenario == "parent_child_block" && argc > 2) {
                    return worker::filelock::parent_child_block(argv[2]);
                }
                if (scenario == "test_basic_non_blocking" && argc > 2) {
                    return worker::filelock::test_basic_non_blocking(argv[2]);
                }
                if (scenario == "test_blocking_lock" && argc > 2) {
                    return worker::filelock::test_blocking_lock(argv[2]);
                }
                if (scenario == "test_timed_lock" && argc > 2) {
                    return worker::filelock::test_timed_lock(argv[2]);
                }
                if (scenario == "test_move_semantics" && argc > 3) {
                    return worker::filelock::test_move_semantics(argv[2], argv[3]);
                }
                if (scenario == "test_directory_creation" && argc > 2) {
                    return worker::filelock::test_directory_creation(argv[2]);
                }
                if (scenario == "test_directory_path_locking" && argc > 2) {
                    return worker::filelock::test_directory_path_locking(argv[2]);
                }
                if (scenario == "test_multithreaded_non_blocking" && argc > 2) {
                    return worker::filelock::test_multithreaded_non_blocking(argv[2]);
                }
            } else if (module == "jsonconfig") {
                /*if (path_env)
                {
                    fmt::print(stderr, "[spawned for jsonconfig tests] PATH={}", path_env);
                }
                else
                {
                    fmt::print(stderr,
                               "[spawned for jsonconfig tests] PATH environment variable not found.");
                }*/

                if (scenario == "write_id" && argc > 3) {
                    return worker::jsonconfig::write_id(argv[2], argv[3]);
                }
            } else if (module == "logger") {
                if (path_env)
                {
                    fmt::print(stderr, "[spawned for logger tests] PATH={}", path_env);
                }
                else
                {
                    fmt::print(stderr,
                               "[spawned for logger tests] PATH environment variable not found.");
                }

                if (scenario == "stress_log" && argc > 3) {
                    worker::logger::stress_log(argv[2], std::stoi(argv[3]));
                    return 0;
                }
                if (scenario == "test_basic_logging" && argc > 2) {
                    return worker::logger::test_basic_logging(argv[2]);
                }
                if (scenario == "test_log_level_filtering" && argc > 2) {
                    return worker::logger::test_log_level_filtering(argv[2]);
                }
                if (scenario == "test_bad_format_string" && argc > 2) {
                    return worker::logger::test_bad_format_string(argv[2]);
                }
                if (scenario == "test_default_sink_and_switching" && argc > 2) {
                    return worker::logger::test_default_sink_and_switching(argv[2]);
                }
                if (scenario == "test_multithread_stress" && argc > 2) {
                    return worker::logger::test_multithread_stress(argv[2]);
                }
                if (scenario == "test_flush_waits_for_queue" && argc > 2) {
                    return worker::logger::test_flush_waits_for_queue(argv[2]);
                }
                if (scenario == "test_shutdown_idempotency" && argc > 2) {
                    return worker::logger::test_shutdown_idempotency(argv[2]);
                }
                if (scenario == "test_reentrant_error_callback" && argc > 2) {
                    return worker::logger::test_reentrant_error_callback(argv[2]);
                }
                if (scenario == "test_write_error_callback_async" && argc > 1) {
                    return worker::logger::test_write_error_callback_async();
                }
                if (scenario == "test_platform_sinks" && argc > 1) {
                    return worker::logger::test_platform_sinks();
                }
                if (scenario == "test_concurrent_lifecycle_chaos" && argc > 2) {
                    return worker::logger::test_concurrent_lifecycle_chaos(argv[2]);
                }
            }
        }
        // If mode not recognized, fall through to running tests, which will likely fail
        // in a way that indicates an incorrect worker mode was passed.
    }

    // If not in worker mode, or if worker dispatch fails, run the tests.
    /*if (path_env)
    {
        fmt::print(stderr, "[main] PATH={}", path_env);
    }
    else
    {
        fmt::print(stderr, "[main] PATH environment variable not found.");
    }*/

    if (argc >= 1) g_self_exe_path = argv[0];
    LifecycleManager::instance().initialize();
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    LifecycleManager::instance().finalize();
    return result;
}