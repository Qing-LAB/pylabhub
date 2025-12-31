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


namespace pylabhub::tests::helper
{
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
     * @param exe_path The path to this executable (from g_self_exe_path).
     * @param mode The worker mode string (e.g., "filelock.nonblocking_acquire").
     * @param args A vector of additional string arguments for the worker.
     * @return A platform-specific handle to the new process.
     */
    ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                       const std::vector<std::string> &args);

    // Waits for a worker process to complete and returns its exit code.
    // Handles platform-specific process management.
    int wait_for_worker_and_get_exit_code(ProcessHandle handle);
} // namespace pylabhub::tests::helper
