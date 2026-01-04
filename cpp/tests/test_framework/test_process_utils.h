// tests/test_harness/test_process_utils.h
#pragma once

#include <string>
#include <vector>

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
// Platform-specific definition for a process handle.
#if defined(PLATFORM_WIN64)
using ProcessHandle = HANDLE;
static constexpr HANDLE NULL_PROC_HANDLE = NULL;
#else
using ProcessHandle = pid_t;
static constexpr pid_t NULL_PROC_HANDLE = 0;
#endif

/**
 * @brief Spawns the current test executable as a child process in a specific worker mode.
 *
 * This function abstracts the platform differences for process creation (CreateProcessW on
 * Windows, fork/execv on POSIX).
 *
 * @param exe_path The path to this executable (from g_self_exe_path).
 * @param mode The worker mode string (e.g., "filelock.nonblocking_acquire").
 * @param args A vector of additional string arguments for the worker.
 * @return A platform-specific handle to the new process, or NULL_PROC_HANDLE on failure.
 */
ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                   const std::vector<std::string> &args);

/**
 * @brief Waits for a worker process to complete and returns its exit code.
 *
 * This function abstracts the platform differences for waiting on a process
 * and retrieving its exit code (WaitForSingleObject on Windows, waitpid on POSIX).
 *
 * @param handle The handle of the process to wait for.
 * @return The exit code of the process, or -1 on failure.
 */
int wait_for_worker_and_get_exit_code(ProcessHandle handle);
} // namespace pylabhub::tests::helper
