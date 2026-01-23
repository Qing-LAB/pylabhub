// tests/test_framework/test_entrypoint.cpp
/**
 * @file test_entrypoint.cpp
 * @brief Main entry point for test executables using the multi-process harness.
 *
 * This file contains the `main` function that serves a dual purpose:
 * 1. If started with no specific arguments, it acts as a standard GoogleTest
 *    test runner, discovering and executing all linked tests.
 * 2. If started with a "worker mode" argument (e.g., "module.scenario"),
 *    it acts as a worker process, dispatching to the corresponding worker function
 *    if a dispatcher has been registered.
 *
 * This design allows a single test executable to spawn copies of itself to run
 * isolated, cross-process test scenarios.
 */
#include "test_entrypoint.h"
#include "plh_datahub.hpp"


// Define the global for the executable path, used by worker-spawning tests
std::string g_self_exe_path;

namespace fs = std::filesystem;

// Global function pointer for the worker dispatcher. Initialized to nullptr.
static WorkerDispatchFn g_worker_dispatcher = nullptr;

void register_worker_dispatcher(WorkerDispatchFn fn)
{
    g_worker_dispatcher = fn;
}

int main(int argc, char **argv)
{
    // Before running tests, check if the executable was invoked in "worker mode".
    // A worker mode is specified as the first command-line argument, in the
    // format "module.scenario".
    if (argc > 1)
    {
        std::string mode_str = argv[1];
        size_t dot_pos = mode_str.find('.');
        if (dot_pos != std::string::npos && g_worker_dispatcher)
        {
            // A dispatcher is registered, so attempt to dispatch.
            int dispatch_result = g_worker_dispatcher(argc, argv);
            if (dispatch_result != -1)
            { // -1 means no matching worker found by dispatcher
                return dispatch_result;
            }
        }
    }

    // If no worker mode was matched, or no dispatcher was registered, or dispatcher
    // didn't handle it, fall through to the standard test runner mode.
    // Store the executable path for tests that need to spawn workers.
    if (argc >= 1)
        g_self_exe_path = argv[0];

    bool use_modules = true;
    for (int i = 1; i < argc; ++i) {
        // If ctest is run with a filter like --gtest_filter=*NoLifecycle*, this
        // string will appear in argv and we can use it as a flag to disable
        // the lifecycle modules that cause threading conflicts with TSan.
        if (std::string(argv[i]).find("NoLifecycle") != std::string::npos) {
            use_modules = false;
            break;
        }
    }

    if (use_modules) {
        pylabhub::utils::LifecycleGuard guard(pylabhub::utils::MakeModDefList(
            pylabhub::utils::Logger::GetLifecycleModule(),
            pylabhub::utils::FileLock::GetLifecycleModule(),
            pylabhub::utils::JsonConfig::GetLifecycleModule())
        );
        // Initialize GoogleTest and run all registered tests.
        ::testing::InitGoogleTest(&argc, argv);
        int result = RUN_ALL_TESTS();
        return result;
    } else {
        pylabhub::utils::LifecycleGuard guard;
        // Initialize GoogleTest and run all registered tests.
        ::testing::InitGoogleTest(&argc, argv);
        int result = RUN_ALL_TESTS();
        return result;
    }
}
