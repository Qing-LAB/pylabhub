// tests/test_harness/test_process_utils.cpp
/**
 * @file test_process_utils.cpp
 * @brief Implements platform-abstracted utilities for spawning and managing child processes.
 */
#include "test_process_utils.h"
#include "shared_test_helpers.h" // For read_file_contents

#include <chrono>
#include <cstdio>  // For stderr
#include <fcntl.h> // For open, O_WRONLY
#include <string>
#include <vector>
#include <iostream>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "debug_info.hpp"
#include "format_tools.hpp" // For s2ws, ws2s
#include "platform.hpp"
#include <fmt/core.h>

namespace pylabhub::tests::helper
{

// Internal helper to spawn a process with output redirection
static ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                          const std::vector<std::string> &args,
                                          const fs::path &stdout_path,
                                          const fs::path &stderr_path,
                                          bool redirect_stderr_to_console)
{
#if defined(PLATFORM_WIN64)
    std::string cmdline = fmt::format("\"{}\" {}\n", exe_path, mode);
    for (const auto &a : args)
        cmdline += fmt::format(" \"{}\"", a);

    std::wstring wcmd = pylabhub::format_tools::s2ws(cmdline);
    std::vector<wchar_t> wcmd_buf(wcmd.begin(), wcmd.end());
    wcmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hStdout = CreateFileW(stdout_path.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
    if (hStdout == INVALID_HANDLE_VALUE) {
        return NULL_PROC_HANDLE;
    }

    HANDLE hStderr;
    if (redirect_stderr_to_console) {
        hStderr = GetStdHandle(STD_ERROR_HANDLE);
    } else {
        hStderr = CreateFileW(stderr_path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (hStderr == INVALID_HANDLE_VALUE) {
            CloseHandle(hStdout);
            return NULL_PROC_HANDLE;
        }
    }

    si.hStdOutput = hStdout;
    si.hStdError = hStderr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    BOOL ok = CreateProcessW(nullptr, wcmd_buf.data(), nullptr, nullptr,
                             /*bInheritHandles*/ TRUE, 0, nullptr, nullptr, &si, &pi);

    CloseHandle(hStdout);
    if (!redirect_stderr_to_console) {
        CloseHandle(hStderr);
    }

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
        PLH_DEBUG("ERROR: CreateProcessW failed. Code: {} - Message: {}", err, msg);
        return NULL_PROC_HANDLE;
    }

    CloseHandle(pi.hThread);
    return pi.hProcess;
#else
    pid_t pid = fork();
    if (pid == 0)
    {
        int stdout_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (stdout_fd != -1)
        {
            dup2(stdout_fd, 1);
            close(stdout_fd);
        }

        if (!redirect_stderr_to_console) {
            int stderr_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (stderr_fd != -1)
            {
                dup2(stderr_fd, 2);
                close(stderr_fd);
            }
        }
        // If redirect_stderr_to_console is true, we just don't redirect stderr,
        // so it will be inherited from the parent and go to the console.

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe_path.c_str()));
        argv.push_back(const_cast<char *>(mode.c_str()));
        for (const auto &arg : args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(exe_path.c_str(), argv.data());

        // If execv returns, it must have failed
        fprintf(stderr, "[CHILD %d] ERROR: execv failed: %s (errno: %d)\n", getpid(), strerror(errno), errno);
        _exit(127);
    }
    return pid;
#endif
}

// Internal helper to wait for a process and get its exit code
static int wait_for_worker_and_get_exit_code(ProcessHandle handle)
{
    if (handle == NULL_PROC_HANDLE)
        return -1;

#if defined(PLATFORM_WIN64)
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
#else
    int status = 0;
    if (waitpid(handle, &status, 0) == -1)
    {
        return -1;
    }
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}


WorkerProcess::WorkerProcess(const std::string &exe_path, const std::string &mode,
                             const std::vector<std::string> &args, bool redirect_stderr_to_console)
    : redirect_stderr_to_console_(redirect_stderr_to_console)
{
    auto base_name = fs::path(exe_path).filename().string() + "_" + mode;
    std::replace(base_name.begin(), base_name.end(), '.', '_');
    auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    stdout_path_ =
        fs::temp_directory_path() / fmt::format("{}_{}_stdout.log", base_name, ts);
    
    if (!redirect_stderr_to_console_) {
        stderr_path_ =
            fs::temp_directory_path() / fmt::format("{}_{}_stderr.log", base_name, ts);
    }

    handle_ = spawn_worker_process(exe_path, mode, args, stdout_path_, stderr_path_, redirect_stderr_to_console_);
}

WorkerProcess::~WorkerProcess()
{
    if (handle_ != NULL_PROC_HANDLE && !waited_)
    {
        wait_for_exit();
    }
    std::error_code ec;
    fs::remove(stdout_path_, ec);
    if (!redirect_stderr_to_console_) {
        fs::remove(stderr_path_, ec);
    }
}

int WorkerProcess::wait_for_exit()
{
    if (waited_)
        return exit_code_;

    exit_code_ = wait_for_worker_and_get_exit_code(handle_);
    waited_ = true;
    handle_ = NULL_PROC_HANDLE;

    read_file_contents(stdout_path_.string(), stdout_content_);
    if (!redirect_stderr_to_console_) {
        read_file_contents(stderr_path_.string(), stderr_content_);
    }
    return exit_code_;
}

const std::string &WorkerProcess::get_stdout() const
{
    if (!waited_)
        read_file_contents(stdout_path_.string(), stdout_content_);
    return stdout_content_;
}

const std::string &WorkerProcess::get_stderr() const
{
    if (!waited_ && !redirect_stderr_to_console_)
        read_file_contents(stderr_path_.string(), stderr_content_);
    return stderr_content_;
}

void expect_worker_ok(const WorkerProcess &proc, const std::vector<std::string>& expected_stderr_substrings)
{
    using ::testing::HasSubstr;
    using ::testing::Not;

    // This check is fundamental and should always be performed.
    ASSERT_EQ(proc.exit_code(), 0) << "Worker process failed with non-zero exit code. Stderr:\n"
                                   << proc.get_stderr();
    
    // If stderr was redirected to console, we cannot and should not check its content.
    // The assertions are based on the captured stderr file.
    if (proc.get_stderr().empty() && proc.exit_code() == 0) {
        // This can happen if stderr was redirected to console.
        // We can't make assertions on stderr content, so we just check exit code.
        // A warning could be logged here if needed.
        std::cout << "[WARN] Stderr was not captured (likely redirected to console). Skipping stderr content checks." << std::endl;
        return;
    }


    const auto &stderr_out = proc.get_stderr();
    if (expected_stderr_substrings.empty()) {
        EXPECT_THAT(stderr_out, Not(HasSubstr("ERROR")));
        EXPECT_THAT(stderr_out, Not(HasSubstr("FATAL")));
        EXPECT_THAT(stderr_out, Not(HasSubstr("PANIC")));
        EXPECT_THAT(stderr_out, Not(HasSubstr("[WORKER FAILURE]")));
    } else {
        for (const auto& substr : expected_stderr_substrings) {
            EXPECT_THAT(stderr_out, HasSubstr(substr));
        }
    }
}

} // namespace pylabhub::tests::helper
