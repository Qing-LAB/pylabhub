#pragma once
#include <string>
#include <vector>
#include "platform.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace test_utils
{
    #if defined(PLATFORM_WIN64)
    using ProcessHandle = HANDLE;
    #else
    using ProcessHandle = pid_t;
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

} // namespace test_utils
