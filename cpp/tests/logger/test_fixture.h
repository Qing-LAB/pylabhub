#pragma once

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

#include "helpers/test_entrypoint.h"
#include "helpers/test_process_utils.h"
#include "platform.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;
using namespace test_utils;

namespace
{

// Waits for a worker process to complete and returns its exit code.
// Handles platform-specific process management.
int wait_for_worker_and_get_exit_code(ProcessHandle handle)
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

} // namespace

// The new test fixture is much simpler. It no longer manages the application
// lifecycle, as each test runs in a fresh process. It only manages the cleanup
// of temporary files created during the tests.
class LoggerTest : public ::testing::Test
{
protected:
    std::vector<fs::path> paths_to_clean_;

    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            try
            {
                if (fs::exists(p)) fs::remove(p);
            }
            catch (...)
            {
                // best-effort cleanup
            }
        }
    }

    fs::path GetUniqueLogPath(const std::string &test_name)
    {
        auto p = fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
        paths_to_clean_.push_back(p);
        // Ensure the file does not exist from a previous failed run.
        try
        {
            if (fs::exists(p)) fs::remove(p);
        }
        catch (...)
        {
        }
        return p;
    }
};
