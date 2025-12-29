#pragma once
#include <string>
#include <vector>
#include "platform.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace test_utils
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
    inline int wait_for_worker_and_get_exit_code(ProcessHandle handle)
    {
    #if defined(PLATFORM_WIN64)
        if (handle == NULL_PROC_HANDLE) return -1;
        // Wait for a reasonable time, not infinite, to prevent hangs.
        if (WaitForSingleObject(handle, 60000) == WAIT_TIMEOUT)
        {
            // If it times out, terminate it to prevent a hanging process.
            TerminateProcess(handle, 99);
            CloseHandle(handle);
            return -99; // Special exit code for timeout
        }
        DWORD exit_code = 1;
        GetExitCodeProcess(handle, &exit_code);
        CloseHandle(handle);
        return static_cast<int>(exit_code);
    #else
        if (handle <= 0) return -1;
        int status = 0;
        // A more robust wait might include a timeout mechanism here as well.
        waitpid(handle, &status, 0);
        if (WIFEXITED(status))
        {
            return WEXITSTATUS(status);
        }
        return -1; // Indicate failure if not exited normally.
    #endif
    }

} // namespace test_utils
