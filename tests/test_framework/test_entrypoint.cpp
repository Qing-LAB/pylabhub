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
#include "utils/thread_manager.hpp"

#include <gtest/gtest.h>

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
    // Worker mode is identified by the presence of '.' in the first argument.
    // If a worker argument is given but no dispatcher claims it, this is a fatal
    // configuration error — do NOT silently fall through to the gtest runner, as
    // that would cause the parent test to hang waiting for a well-behaved subprocess.
    if (argc > 1)
    {
        std::string mode_str = argv[1];
        if (mode_str.find('.') != std::string::npos && mode_str[0] != '-')
        {
            // Try each registered dispatcher in order.
            for (auto fn : worker_dispatchers())
            {
                int r = fn(argc, argv);
                if (r != -1) // -1 means "no matching scenario here, try next"
                    return r;
            }
            // No dispatcher claimed this scenario — hard error to prevent silent hang.
            fmt::print(stderr,
                       "[test_entrypoint] ERROR: No dispatcher matched worker scenario '{}'. "
                       "Check that the worker file is linked into this test binary.\n",
                       mode_str);
            return 127; // Conventionally: command not found
        }
    }

    // Test runner mode: no lifecycle, pure API tests only.
    // Tests that need lifecycle modules spawn subprocesses via IsolatedProcessTest.
    ::testing::InitGoogleTest(&argc, argv);

    // ── ThreadManager detach-leak listener ──────────────────────────────
    //
    // If any ThreadManager instance inside the test process had to detach a
    // stuck thread during a test, fail that test explicitly. Without this,
    // tests that hit the bounded-join timeout path would appear to pass
    // (the detach doesn't throw; it just logs + returns), masking a real
    // shutdown-path regression.
    //
    // The listener takes a baseline snapshot at each test start and checks
    // for an increment at each test end. Tests that DELIBERATELY exercise
    // the timeout-detach path (e.g., the bounded-join unit test itself)
    // should call ThreadManager::reset_process_detached_count_for_testing()
    // in their TearDown() to clear the counter after their scoped exercise.
    class ThreadLeakListener : public ::testing::EmptyTestEventListener
    {
      public:
        void OnTestStart(const ::testing::TestInfo & /*info*/) override
        {
            baseline_ = pylabhub::utils::ThreadManager::process_detached_count();
        }
        void OnTestEnd(const ::testing::TestInfo &info) override
        {
            const auto current =
                pylabhub::utils::ThreadManager::process_detached_count();
            if (current > baseline_)
            {
                const auto leaked = current - baseline_;
                ADD_FAILURE_AT(info.file() ? info.file() : "<unknown>",
                               info.line())
                    << "ThreadManager detached " << leaked
                    << " thread(s) during test '" << info.test_suite_name()
                    << "." << info.name()
                    << "'. See ERROR log entries tagged [ThreadManager:*]"
                       " for the owner+name of each leaked thread. A test"
                       " that deliberately exercises the timeout path must"
                       " call ThreadManager::"
                       "reset_process_detached_count_for_testing() in its"
                       " TearDown().";
            }
        }

      private:
        std::size_t baseline_{0};
    };

    ::testing::UnitTest::GetInstance()->listeners().Append(new ThreadLeakListener);
    return RUN_ALL_TESTS();
}
