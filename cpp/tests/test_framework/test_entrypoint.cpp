// tests/test_framework/test_entrypoint.cpp
/**
 * @file test_entrypoint.cpp
 * @brief Main entry point for test executables using the isolated-process harness.
 *
 * This file contains the `main` function that serves a dual purpose:
 *
 * 1. **Worker mode**: If started with a "module.scenario" argument, dispatches to
 *    the matching registered worker function. Workers manage their OWN lifecycle
 *    via `run_gtest_worker()` or `run_worker_bare()`. No lifecycle is initialized
 *    here.
 *
 * 2. **Test runner mode**: Runs GoogleTest with NO lifecycle initialization. Only
 *    Pattern 1 (PureApiTest / plain ::testing::Test) tests run safely in this mode.
 *    Any test that needs a lifecycle module must spawn a subprocess via
 *    IsolatedProcessTest::SpawnWorker().
 *
 * **Isolation guarantee**: Because main() initializes nothing, a worker process that
 * crashes, panics, or calls finalize() cannot corrupt state for other tests. Every
 * subprocess starts with a clean slate.
 *
 * **Self-registering dispatchers**: Each worker .cpp file registers its own handler
 * via a static initializer calling register_worker_dispatcher(). The dispatcher list
 * is tried in order until one returns non-(-1). This means each test executable only
 * needs to link the worker files it actually uses — no monolithic dispatcher required.
 */
#include "test_entrypoint.h"
#include "plh_base.hpp"
#include <vector>

// Define the global for the executable path, used by worker-spawning tests
std::string g_self_exe_path;

namespace fs = std::filesystem;

// List of registered worker dispatchers (appended to by static initializers).
static std::vector<WorkerDispatchFn> &worker_dispatchers()
{
    // Function-local static avoids static-init-order issues.
    static std::vector<WorkerDispatchFn> list;
    return list;
}

void register_worker_dispatcher(WorkerDispatchFn fn)
{
    worker_dispatchers().push_back(fn);
}

int main(int argc, char **argv)
{
    // Store the executable path so IsolatedProcessTest can re-spawn this binary.
    g_self_exe_path = (argc >= 1) ? argv[0] : "";

    // Check if invoked in worker mode: first arg is "module.scenario[.subsection]".
    if (argc > 1)
    {
        std::string mode_str = argv[1];
        if (mode_str.find('.') != std::string::npos)
        {
            // Try each registered dispatcher in order.
            for (auto fn : worker_dispatchers())
            {
                int r = fn(argc, argv);
                if (r != -1) // -1 means "no matching scenario here, try next"
                    return r;
            }
        }
    }

    // Test runner mode: no lifecycle, pure API tests only.
    // Tests that need lifecycle modules spawn subprocesses via IsolatedProcessTest.
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
