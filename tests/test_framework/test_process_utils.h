// tests/test_harness/test_process_utils.h
#pragma once

#include "plh_datahub.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h> // For fork, execv, _exit
#endif

/**
 * @file test_process_utils.h
 * @brief Provides platform-abstracted utilities for spawning and managing child processes in tests.
 *
 * This is essential for multi-process testing, allowing a main test runner to
 * spawn worker processes to test inter-process communication and resource locking.
 */
namespace pylabhub::tests::helper
{
namespace fs = std::filesystem;
// Platform-specific definition for a process handle.
#if defined(PLATFORM_WIN64)
using ProcessHandle = HANDLE;
static constexpr HANDLE NULL_PROC_HANDLE = NULL;
#else
using ProcessHandle = pid_t;
static constexpr pid_t NULL_PROC_HANDLE = 0;
#endif

/**
 * @class WorkerProcess
 * @brief Manages a worker process, including its creation, termination, and output capture.
 *
 * This class provides a robust, RAII-style mechanism for handling worker processes
 * in tests. It automatically captures stdout and stderr to temporary files and
 * provides methods to inspect them after the process completes. This ensures
 * that no output is lost and that tests can be more rigorous by asserting on
 * the worker's output.
 */
class WorkerProcess
{
  public:
    /**
     * @brief Spawns the current test executable as a child process in a specific worker mode.
     *
     * @param exe_path The path to this executable (from g_self_exe_path).
     * @param mode The worker mode string (e.g., "filelock.nonblocking_acquire").
     * @param args A vector of additional string arguments for the worker.
     * @param redirect_stderr_to_console If true, worker stderr goes to console.
     * @param with_ready_signal If true, creates a pipe; child signals via PLH_TEST_READY_FD/HANDLE.
     */
    WorkerProcess(const std::string &exe_path, const std::string &mode,
                  const std::vector<std::string> &args, bool redirect_stderr_to_console = false,
                  bool with_ready_signal = false);
    ~WorkerProcess();

    WorkerProcess(const WorkerProcess &) = delete;
    WorkerProcess &operator=(const WorkerProcess &) = delete;
    WorkerProcess(WorkerProcess &&) = delete;
    WorkerProcess &operator=(WorkerProcess &&) = delete;

    /**
     * @brief Waits for the worker process to complete with a timeout.
     * @param timeout_s Maximum seconds to wait. -1 = infinite (old behavior).
     *                  If timeout expires, sends SIGTERM, waits 2s, then SIGKILL.
     * @return The exit code of the process, or -1 if killed by timeout.
     */
    int wait_for_exit(int timeout_s = 60);

    /**
     * @brief Returns the captured standard output of the worker process.
     * @return A const reference to the stdout content.
     */
    const std::string &get_stdout() const;

    /**
     * @brief Returns the captured standard error of the worker process.
     * @return A const reference to the stderr content.
     */
    const std::string &get_stderr() const;

    /**
     * @brief Returns the exit code of the process after it has completed.
     * @return The exit code, or -1 if not yet waited for.
     */
    int exit_code() const { return exit_code_; }

    /**
     * @brief Returns the worker scenario mode (the "module.scenario" string
     *        that selects the dispatcher inside the worker subprocess).
     *
     * Used by expect_worker_ok to auto-derive the [WORKER_BEGIN] /
     * [WORKER_END_OK] / [WORKER_FINALIZED] markers it requires in stderr.
     * Those markers are emitted by run_gtest_worker / run_worker_bare and
     * close the "test passed because the body silently short-circuited"
     * loophole.
     */
    const std::string &mode() const { return mode_; }

    /**
     * @brief Checks if the worker process was successfully spawned.
     * @return True if the process handle is valid, false otherwise.
     */
    bool valid() const { return handle_ != NULL_PROC_HANDLE; }

    /**
     * @brief Blocks until the child signals "ready" via PLH_TEST_READY_FD (POSIX) or
     * PLH_TEST_READY_HANDLE (Windows). Only valid when constructed with with_ready_signal=true.
     * Call before wait_for_exit() when using SpawnWorkerWithReadySignal.
     */
    void wait_for_ready();

    /**
     * @brief Sends a signal to the child process (POSIX only).
     * On Windows this is a no-op.
     * @param sig The signal number (e.g. SIGTERM).
     */
    void send_signal(int sig);

  private:
    void init_with_ready_signal(const std::string &exe_path, const std::string &mode,
                                const std::vector<std::string> &args,
                                bool redirect_stderr_to_console);
    ProcessHandle handle_ = NULL_PROC_HANDLE;
    int exit_code_ = -1;
    std::string mode_;     ///< worker scenario string ("module.scenario")
    fs::path stdout_path_;
    fs::path stderr_path_;
    mutable std::string stdout_content_;
    mutable std::string stderr_content_;
    bool waited_ = false;
    bool redirect_stderr_to_console_ = false;
    bool with_ready_signal_ = false;
#if defined(PLATFORM_WIN64)
    HANDLE ready_pipe_read_ = NULL;
#else
    int ready_pipe_read_ = -1;
#endif
};

/**
 * @brief Asserts that a worker process completed successfully.
 *
 * This function checks that the worker's exit code is 0 and that its
 * stderr stream does not contain unexpected error markers.
 *
 * **Worker completion milestones** (silent-shortcircuit catch).  When
 * @p require_completion_markers is true (default), this function also
 * requires three markers in the worker's stderr that
 * run_gtest_worker / run_worker_bare emit automatically:
 *   [WORKER_BEGIN]      — proves the dispatcher routed correctly
 *   [WORKER_END_OK]     — proves the body returned without throwing
 *   [WORKER_FINALIZED]  — proves LifecycleGuard finalize completed
 * A worker whose body short-circuited (early return, GTEST_SKIP, or an
 * unreachable-after-helper-failure scenario) still exits 0 but does not
 * emit [WORKER_END_OK], so this catches the failure mode that pure
 * exit-code checking misses.
 *
 * @param proc The WorkerProcess instance to check.
 * @param required_substrings Informational/operational strings that MUST appear in stderr.
 *        Pure positive assertions — do NOT suppress the "no ERROR" check.
 *        Use to verify expected operational log output (e.g., "DataBlock", "Messenger").
 * @param expected_error_substrings Error-level strings that MUST appear in stderr.
 *        When non-empty: the broad "no ERROR" check is replaced by an exhaustive check —
 *        each named string must appear, AND every [ERROR ] line in stderr must be
 *        accounted for by at least one named string.  Additional unexpected ERRORs
 *        are not silently ignored.  Name the specific expected ERROR message, not
 *        just the generic string "ERROR".
 *        FATAL, PANIC, and [WORKER FAILURE] are always forbidden regardless.
 * @param require_completion_markers When true (default), require all three
 *        [WORKER_*] markers in stderr.  Set to false ONLY for legacy
 *        multi-process workers that bypass run_gtest_worker /
 *        run_worker_bare — typically process-shared mutex / FileLock
 *        tests where the worker's whole point is to die holding a
 *        resource.  New tests should never opt out.
 */
void expect_worker_ok(const WorkerProcess &proc,
                      const std::vector<std::string> &required_substrings = {},
                      const std::vector<std::string> &expected_error_substrings = {},
                      bool require_completion_markers = true);

} // namespace pylabhub::tests::helper
