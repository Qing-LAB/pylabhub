// tests/test_harness/test_process_utils.cpp
/**
 * @file test_process_utils.cpp
 * @brief Implements platform-abstracted utilities for spawning and managing child processes.
 */
#include <cstdio>  // For stderr
#include <fcntl.h> // For open, O_WRONLY
#include <string>
#include <vector>

#include "platform.hpp"

#include <fmt/core.h>

#include "format_tools.hpp" // For s2ws, ws2s
#include "test_process_utils.h"

namespace pylabhub::tests::helper
{
#if defined(PLATFORM_WIN64)
ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                   const std::vector<std::string> &args)
{
    // On Windows, we use the CreateProcessW API.
    // The command line must be a single, mutable, wide-character string.
    std::string cmdline = fmt::format("\"{}\" {}", exe_path, mode);
    for (const auto &a : args)
        cmdline += fmt::format(" \"{}\"", a);

    // Convert to wide string and create a mutable buffer.
    std::wstring wcmd = pylabhub::format_tools::s2ws(cmdline);
    std::vector<wchar_t> wcmd_buf(wcmd.begin(), wcmd.end());
    wcmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    // Create the process. We don't need to inherit handles or create a new console.
    BOOL ok = CreateProcessW(
        /*lpApplicationName*/ nullptr,
        /*lpCommandLine*/ wcmd_buf.data(),
        /*lpProcessAttributes*/ nullptr,
        /*lpThreadAttributes*/ nullptr,
        /*bInheritHandles*/ FALSE,
        /*dwCreationFlags*/ 0,
        /*lpEnvironment*/ nullptr,      // Inherit parent's environment
        /*lpCurrentDirectory*/ nullptr, // Inherit parent's working directory
        /*lpStartupInfo*/ &si,
        /*lpProcessInformation*/ &pi);

    if (!ok)
    {
        // If process creation fails, format and print the error message.
        DWORD err = GetLastError();
        LPWSTR msgBuf = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msgBuf, 0,
                       nullptr);
        std::string msg = pylabhub::format_tools::ws2s(msgBuf ? msgBuf : L"(no message)");
        if (msgBuf)
            LocalFree(msgBuf);
        PLH_DEBUG("ERROR: CreateProcessW failed. Code: {} - Message: {}", err, msg);
        pylabhub::platform::print_stack_trace();
        return nullptr;
    }

    // We don't need the thread handle, so close it. The caller is responsible
    // for the process handle.
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

int wait_for_worker_and_get_exit_code(ProcessHandle handle)
{
    if (handle == NULL_PROC_HANDLE)
        return -1;

    // Wait indefinitely for the process to terminate.
    DWORD waitResult = WaitForSingleObject(handle, INFINITE);
    if (waitResult == WAIT_FAILED)
    {
        CloseHandle(handle);
        return -1;
    }

    // Retrieve the exit code.
    DWORD exit_code = 0;
    GetExitCodeProcess(handle, &exit_code);
    CloseHandle(handle); // Clean up the process handle.
    return static_cast<int>(exit_code);
}
#else
// POSIX implementation using fork() and execv()
ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                   const std::vector<std::string> &args)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // In the child process.
        // Redirect stdout and stderr to a log file for debugging purposes.
        int fd = open("worker_output.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd != -1)
        {
            dup2(fd, 1); // stdout
            dup2(fd, 2); // stderr
            close(fd);
        }

        // Build the argument vector for execv.
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe_path.c_str()));
        argv.push_back(const_cast<char *>(mode.c_str()));
        for (const auto &arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // Replace the child process image with the test executable.
        execv(exe_path.c_str(), argv.data());
        _exit(127); // execv only returns on error, so exit immediately.
    }
    return pid; // In the parent process, return the child's PID.
}

int wait_for_worker_and_get_exit_code(ProcessHandle handle)
{
    if (handle == NULL_PROC_HANDLE)
        return -1;
    int status = 0;

    // Wait for the child process to change state.
    if (waitpid(handle, &status, 0) == -1)
    {
        return -1; // waitpid failed.
    }

    // Check if the process terminated normally and return its exit status.
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1; // Process did not terminate normally.
}
#endif
} // namespace pylabhub::tests::helper