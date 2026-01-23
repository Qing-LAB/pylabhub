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
     */
    WorkerProcess(const std::string &exe_path, const std::string &mode,
                  const std::vector<std::string> &args, bool redirect_stderr_to_console = false);
    ~WorkerProcess();

    WorkerProcess(const WorkerProcess &) = delete;
    WorkerProcess &operator=(const WorkerProcess &) = delete;
    WorkerProcess(WorkerProcess &&) = delete;
    WorkerProcess &operator=(WorkerProcess &&) = delete;

    /**
     * @brief Waits for the worker process to complete and captures its results.
     * @return The exit code of the process.
     */
    int wait_for_exit();

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
     * @brief Checks if the worker process was successfully spawned.
     * @return True if the process handle is valid, false otherwise.
     */
    bool valid() const { return handle_ != NULL_PROC_HANDLE; }

  private:
    ProcessHandle handle_ = NULL_PROC_HANDLE;
    int exit_code_ = -1;
    fs::path stdout_path_;
    fs::path stderr_path_;
    mutable std::string stdout_content_;
    mutable std::string stderr_content_;
    bool waited_ = false;
    bool redirect_stderr_to_console_ = false;
};

/**
 * @brief Asserts that a worker process completed successfully.
 *
 * This function checks that the worker's exit code is 0 and that its
 * stderr stream does not contain common error markers. This provides a
 * robust check for success while allowing for legitimate debug output.
 *
 * @param proc The WorkerProcess instance to check.
 */
void expect_worker_ok(const WorkerProcess &proc,
                      const std::vector<std::string> &expected_stderr_substrings = {});

} // namespace pylabhub::tests::helper
