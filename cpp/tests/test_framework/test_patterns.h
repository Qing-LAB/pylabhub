#pragma once
/**
 * @file test_patterns.h
 * @brief Three standard test patterns for the PyLabHub test suite.
 *
 * ## Why three patterns?
 *
 * Lifecycle modules (Logger, FileLock, JsonConfig, CryptoUtils) are process-global
 * singletons. A test that panics, calls finalize(), or crashes will corrupt the
 * lifecycle state for every subsequent test in the same process. CTest hides this
 * because it spawns a fresh executable per suite; running an executable directly
 * for debugging will fail.
 *
 * The solution: `main()` initializes NOTHING. Every test that needs a lifecycle
 * spawns a subprocess. Each subprocess starts with a clean slate.
 *
 * ---
 *
 * ## Pattern 1 — PureApiTest
 *
 * In-process, no lifecycle, no module dependencies.
 * For: pure functions, data structures, algorithms, compile-time traits.
 *
 *   class MyTest : public pylabhub::tests::PureApiTest { ... };
 *   TEST_F(MyTest, SomeFunction) { EXPECT_EQ(add(1,2), 3); }
 *
 * ---
 *
 * ## Pattern 2 — plain ::testing::Test (in-process, thread-racing only)
 *
 * Use this ONLY for thread-racing tests that do NOT need lifecycle modules.
 * The test runs in the main process. Use ThreadRacer from shared_test_helpers.h
 * for concurrent execution.
 *
 * If your threading test DOES need a lifecycle module (Logger, FileLock, etc.),
 * use Pattern 3: put the threading logic inside a worker subprocess.
 *
 * ---
 *
 * ## Pattern 3 — IsolatedProcessTest
 *
 * Spawns one or more subprocesses. Each subprocess owns its lifecycle.
 * For: any test that needs lifecycle modules, crash/panic testing, true IPC,
 * lifecycle finalize/shutdown testing, threading tests that need module state.
 *
 *   class MyTest : public pylabhub::tests::IsolatedProcessTest {
 *   protected:
 *       void SetUp() override { IsolatedProcessTest::SetUp(); }
 *   };
 *
 *   TEST_F(MyTest, BasicLogging) {
 *       auto w = SpawnWorker("logger.basic_logging", {log_path});
 *       ExpectWorkerOk(w);
 *   }
 *
 *   TEST_F(MyTest, TwoProcessContention) {
 *       auto workers = SpawnWorkers({
 *           {"filelock.writer", {path}},
 *           {"filelock.reader", {path}},
 *       });
 *       for (auto& w : workers) ExpectWorkerOk(w);
 *   }
 *
 * Workers define their own lifecycle inside the worker function body using
 * run_gtest_worker() (standard) or run_worker_bare() (manual lifecycle control).
 */

#include "gtest/gtest.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include <list>
#include <string>
#include <vector>
#include <utility>

namespace pylabhub::tests
{

// ============================================================================
// Pattern 1: Pure API / Function Tests
// ============================================================================

/**
 * @brief Base class for pure API/function tests.
 *
 * No lifecycle initialization, no module dependencies. Fast, isolated.
 * These tests run in-process in the main GTest runner.
 */
class PureApiTest : public ::testing::Test
{
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Pattern 3: Isolated Process Tests
// ============================================================================

/**
 * @brief Base class for tests that spawn isolated worker subprocesses.
 *
 * Each call to SpawnWorker() re-executes the current test binary as a child
 * process in "worker mode". The worker initializes its own lifecycle (via
 * run_gtest_worker or run_worker_bare), runs the test logic, and exits.
 * The parent then inspects the exit code and captured output.
 *
 * This guarantees complete lifecycle isolation: crashes, panics, finalize(),
 * and shutdown() in a worker cannot affect any other test.
 */
class IsolatedProcessTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Verify that this executable knows its own path (set by test_entrypoint main()).
        ASSERT_FALSE(g_self_exe_path.empty())
            << "g_self_exe_path is empty — test_entrypoint.cpp must set it in main()";
    }

    /**
     * @brief Spawns a single worker subprocess for a named scenario.
     *
     * @param scenario Worker mode string, e.g. "logger.basic_logging"
     * @param args      Additional positional arguments passed after the scenario name
     * @param redirect_stderr_to_console  If true, worker stderr appears in test output
     * @return WorkerProcess handle (call wait_for_exit() or ExpectWorkerOk())
     */
    helper::WorkerProcess SpawnWorker(const std::string &scenario,
                                      std::vector<std::string> args = {},
                                      bool redirect_stderr_to_console = false)
    {
        return helper::WorkerProcess(g_self_exe_path, scenario, args, redirect_stderr_to_console,
                                     false);
    }

    /**
     * @brief Spawns a worker that signals "ready" via pipe when init is complete.
     *
     * The worker receives PLH_TEST_READY_FD (POSIX) or PLH_TEST_READY_HANDLE (Windows).
     * Call signal_test_ready() from the worker when init is done; parent blocks on
     * wait_for_ready() until then. Use for deterministic parent-child ordering without sleeps.
     *
     * @param scenario Worker mode string
     * @param args     Additional arguments for the worker
     * @return WorkerProcess handle; call wait_for_ready() before proceeding, then wait_for_exit()
     */
    helper::WorkerProcess SpawnWorkerWithReadySignal(const std::string &scenario,
                                                      std::vector<std::string> args = {})
    {
        return helper::WorkerProcess(g_self_exe_path, scenario, args, false, true);
    }

    /**
     * @brief Spawns multiple worker subprocesses simultaneously.
     *
     * Workers are launched concurrently (before any are waited on), making
     * this suitable for IPC contention tests.
     *
     * std::list is used because WorkerProcess is neither copyable nor movable;
     * list nodes are never relocated so emplace_back constructs in-place safely.
     *
     * @param scenarios List of (scenario, args) pairs
     * @return List of WorkerProcess handles
     */
    std::list<helper::WorkerProcess>
    SpawnWorkers(std::vector<std::pair<std::string, std::vector<std::string>>> scenarios,
                 bool redirect_stderr_to_console = false)
    {
        std::list<helper::WorkerProcess> workers;
        for (auto &[scenario, args] : scenarios)
            workers.emplace_back(g_self_exe_path, scenario, args, redirect_stderr_to_console, false);
        return workers;
    }

    /**
     * @brief Waits for a worker and asserts it succeeded.
     *
     * @param proc                    Worker to wait on
     * @param expected_stderr_substrings  Optional: strings that must appear in stderr
     * @param allow_expected_logger_errors If true, do not assert absence of "ERROR" in stderr
     *        (for tests that intentionally trigger ERROR-level logs, e.g. timeout paths).
     */
    void ExpectWorkerOk(helper::WorkerProcess &proc,
                        std::vector<std::string> expected_stderr_substrings = {},
                        bool allow_expected_logger_errors = false)
    {
        proc.wait_for_exit();
        helper::expect_worker_ok(proc, expected_stderr_substrings, allow_expected_logger_errors);
    }

    /**
     * @brief Waits for all workers and asserts all succeeded.
     */
    void ExpectAllWorkersOk(std::list<helper::WorkerProcess> &workers)
    {
        for (auto &w : workers)
        {
            w.wait_for_exit();
            helper::expect_worker_ok(w);
        }
    }
};

// ============================================================================
// Type trait: determine which pattern a test class uses
// ============================================================================

template <typename TestClass> struct test_pattern
{
    static constexpr bool is_pure_api = std::is_base_of_v<PureApiTest, TestClass>;
    static constexpr bool is_isolated = std::is_base_of_v<IsolatedProcessTest, TestClass>;
    static constexpr bool is_in_process = !is_pure_api && !is_isolated;
};

} // namespace pylabhub::tests
