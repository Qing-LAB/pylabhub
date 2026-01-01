// Standard Library
#include <cstdio> // For stderr
#include <string>
#include <vector>
#include <fcntl.h> // For open, O_WRONLY

#include "platform.hpp"

#include <fmt/core.h> // For fmt::print

#include "test_process_utils.h"
#include "format_tools.hpp" // For s2ws, ws2s

namespace pylabhub::tests::helper
{
#if defined(PLATFORM_WIN64)
    ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                       const std::vector<std::string> &args)
    {
        // Build ASCII command line: "<exe_path>" <mode> "arg1" "arg2" ...
        std::string cmdline = fmt::format("\"{}\" {}", exe_path, mode);
        for (const auto &a : args)
            cmdline += fmt::format(" \"{}\"", a);

        // Convert to wide string once and create a mutable buffer (CreateProcessW may modify it)
        std::wstring wcmd = pylabhub::format_tools::s2ws(cmdline);
        std::vector<wchar_t> wcmd_buf(wcmd.begin(), wcmd.end());
        wcmd_buf.push_back(L'\0');

        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        // Use default environment (nullptr) and default working directory (nullptr).
        // Do NOT pass CREATE_UNICODE_ENVIRONMENT when lpEnvironment is nullptr.
        DWORD creation_flags = 0;

        BOOL ok = CreateProcessW(
            /*lpApplicationName*/ nullptr,
            /*lpCommandLine*/ wcmd_buf.data(),
            /*lpProcessAttributes*/ nullptr,
            /*lpThreadAttributes*/ nullptr,
            /*bInheritHandles*/ FALSE,
            /*dwCreationFlags*/ creation_flags,
            /*lpEnvironment*/ nullptr,      // inherit parent's environment
            /*lpCurrentDirectory*/ nullptr, // inherit parent's working directory
            /*lpStartupInfo*/ &si,
            /*lpProcessInformation*/ &pi);

        if (!ok)
        {
            DWORD err = GetLastError();
            LPWSTR msgBuf = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msgBuf, 0,
                           nullptr);
            std::string msg = pylabhub::format_tools::ws2s(msgBuf ? msgBuf : L"(no message)");
            if (msgBuf)
                LocalFree(msgBuf);
            fmt::print(stderr, "ERROR: CreateProcessW failed. Code: {} - Message: {}\n", err, msg);
            return nullptr;
        }

        CloseHandle(pi.hThread); // caller is responsible for closing the process handle
        return pi.hProcess;
    }

    int wait_for_worker_and_get_exit_code(ProcessHandle handle)
    {
        if (handle == NULL_PROC_HANDLE) return -1;
        DWORD waitResult = WaitForSingleObject(handle, INFINITE);
        if (waitResult == WAIT_FAILED)
        {
            CloseHandle(handle);
            return -1;
        }
        DWORD exit_code = 0;
        GetExitCodeProcess(handle, &exit_code);
        CloseHandle(handle);
        return static_cast<int>(exit_code);
    }
#else
    ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                       const std::vector<std::string> &args)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            // In child process.
            // Redirect stdout and stderr to a file to capture any output for debugging.
            int fd = open("worker_output.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd != -1)
            {
                dup2(fd, 1); // stdout
                dup2(fd, 2); // stderr
                close(fd);
            }

            std::vector<char *> argv;
            argv.push_back(const_cast<char *>(exe_path.c_str()));
            argv.push_back(const_cast<char *>(mode.c_str()));
            for (const auto &arg : args)
            {
                argv.push_back(const_cast<char *>(arg.c_str()));
            }
            argv.push_back(nullptr);
            execv(exe_path.c_str(), argv.data());
            _exit(127); // execv only returns on error
        }
        return pid;
    }

    int wait_for_worker_and_get_exit_code(ProcessHandle handle)
    {
        if (handle == NULL_PROC_HANDLE) return -1;
        int status = 0;
        if (waitpid(handle, &status, 0) == -1)
        {
            return -1; // waitpid failed
        }
        if (WIFEXITED(status))
        {
            return WEXITSTATUS(status);
        }
        return -1; // Process did not terminate normally
    }
#endif
} // namespace test_utils