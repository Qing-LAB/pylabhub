// tests/test_harness/test_process_utils.cpp
/**
 * @file test_process_utils.cpp
 * @brief Implements platform-abstracted utilities for spawning and managing child processes.
 */

#include <algorithm>
#include <chrono>
#include <list>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#include "test_process_utils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "shared_test_helpers.h" // For read_file_contents

#if !defined(PLATFORM_WIN64)
#include <fcntl.h>
#endif

namespace pylabhub::tests::helper
{

// Internal helper to spawn a process with output redirection
static ProcessHandle spawn_worker_process(const std::string &exe_path, const std::string &mode,
                                          const std::vector<std::string> &args,
                                          const fs::path &stdout_path, const fs::path &stderr_path,
                                          bool redirect_stderr_to_console)
{
#if defined(PLATFORM_WIN64)
    std::string cmdline = fmt::format("\"{}\" \"{}\"", exe_path, mode);
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

    HANDLE hStdout =
        CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hStdout == INVALID_HANDLE_VALUE)
    {
        return NULL_PROC_HANDLE;
    }

    HANDLE hStderr;
    if (redirect_stderr_to_console)
    {
        hStderr = GetStdHandle(STD_ERROR_HANDLE);
    }
    else
    {
        hStderr =
            CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hStderr == INVALID_HANDLE_VALUE)
        {
            CloseHandle(hStdout);
            return NULL_PROC_HANDLE;
        }
    }

    si.hStdOutput = hStdout;
    si.hStdError = hStderr;
    // Always redirect stdin from NUL so the child process is never interactive,
    // regardless of whether the parent is running in a terminal.
    si.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    BOOL ok = CreateProcessW(nullptr, wcmd_buf.data(), nullptr, nullptr,
                             /*bInheritHandles*/ TRUE, 0, nullptr, nullptr, &si, &pi);

    CloseHandle(hStdout);
    if (!redirect_stderr_to_console)
    {
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
        // Always redirect stdin to /dev/null so isatty(0) returns false in the child.
        // This ensures CLI binaries never block waiting for interactive input,
        // regardless of whether the parent process is running in a terminal.
        int devnull_fd = open("/dev/null", O_RDONLY);
        if (devnull_fd != -1)
        {
            dup2(devnull_fd, 0);
            close(devnull_fd);
        }

        int stdout_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (stdout_fd != -1)
        {
            dup2(stdout_fd, 1);
            close(stdout_fd);
        }

        if (!redirect_stderr_to_console)
        {
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
        fprintf(stderr, "[CHILD %d] ERROR: execv failed: %s (errno: %d)\n", getpid(),
                strerror(errno), errno);
        _exit(127);
    }
    return pid;
#endif
}

#if defined(PLATFORM_WIN64)
static ProcessHandle spawn_worker_process_with_ready_pipe(const std::string &exe_path,
                                                          const std::string &mode,
                                                          const std::vector<std::string> &args,
                                                          const fs::path &stdout_path,
                                                          const fs::path &stderr_path,
                                                          bool redirect_stderr_to_console,
                                                          HANDLE *out_ready_pipe_read)
{
    *out_ready_pipe_read = NULL;
    HANDLE hRead = NULL;
    HANDLE hWrite = NULL;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return NULL_PROC_HANDLE;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0); // Parent read end not inherited
    std::string handle_val = std::to_string(reinterpret_cast<uintptr_t>(hWrite));
    SetEnvironmentVariableA("PLH_TEST_READY_HANDLE", handle_val.c_str());

    std::string cmdline = fmt::format("\"{}\" \"{}\"", exe_path, mode);
    for (const auto &a : args)
        cmdline += fmt::format(" \"{}\"", a);
    std::wstring wcmd = pylabhub::format_tools::s2ws(cmdline);
    std::vector<wchar_t> wcmd_buf(wcmd.begin(), wcmd.end());
    wcmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    HANDLE hStdout =
        CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hStdout == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        SetEnvironmentVariableA("PLH_TEST_READY_HANDLE", NULL);
        return NULL_PROC_HANDLE;
    }
    HANDLE hStderr;
    if (redirect_stderr_to_console)
        hStderr = GetStdHandle(STD_ERROR_HANDLE);
    else
    {
        hStderr = CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hStderr == INVALID_HANDLE_VALUE)
        {
            CloseHandle(hStdout);
            CloseHandle(hRead);
            CloseHandle(hWrite);
            SetEnvironmentVariableA("PLH_TEST_READY_HANDLE", NULL);
            return NULL_PROC_HANDLE;
        }
    }
    si.hStdOutput = hStdout;
    si.hStdError = hStderr;
    // Always redirect stdin from NUL so the child process is never interactive,
    // regardless of whether the parent is running in a terminal.
    si.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    BOOL ok = CreateProcessW(nullptr, wcmd_buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                             &si, &pi);
    CloseHandle(hStdout);
    if (!redirect_stderr_to_console)
        CloseHandle(hStderr);
    CloseHandle(hWrite);
    SetEnvironmentVariableA("PLH_TEST_READY_HANDLE", NULL);

    if (!ok)
    {
        CloseHandle(hRead);
        return NULL_PROC_HANDLE;
    }
    CloseHandle(pi.hThread);
    *out_ready_pipe_read = hRead;
    return pi.hProcess;
}
#else
static ProcessHandle spawn_worker_process_with_ready_pipe(const std::string &exe_path,
                                                          const std::string &mode,
                                                          const std::vector<std::string> &args,
                                                          const fs::path &stdout_path,
                                                          const fs::path &stderr_path,
                                                          bool redirect_stderr_to_console,
                                                          int *out_ready_pipe_read)
{
    *out_ready_pipe_read = -1;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
        return NULL_PROC_HANDLE;

    pid_t pid = fork();
    if (pid == 0)
    {
        close(pipe_fds[0]);
        std::string fd_str = std::to_string(pipe_fds[1]);
        setenv("PLH_TEST_READY_FD", fd_str.c_str(), 1);

        // Always redirect stdin to /dev/null (same reason as in the simple spawn path).
        int devnull_fd = open("/dev/null", O_RDONLY);
        if (devnull_fd != -1)
        {
            dup2(devnull_fd, 0);
            close(devnull_fd);
        }

        int stdout_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (stdout_fd != -1)
        {
            dup2(stdout_fd, 1);
            close(stdout_fd);
        }
        if (!redirect_stderr_to_console)
        {
            int stderr_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (stderr_fd != -1)
            {
                dup2(stderr_fd, 2);
                close(stderr_fd);
            }
        }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe_path.c_str()));
        argv.push_back(const_cast<char *>(mode.c_str()));
        for (const auto &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execv(exe_path.c_str(), argv.data());
        fprintf(stderr, "[CHILD %d] ERROR: execv failed: %s (errno: %d)\n", getpid(), strerror(errno),
                errno);
        _exit(127);
    }
    close(pipe_fds[1]);
    *out_ready_pipe_read = pipe_fds[0];
    return pid;
}
#endif

// Internal helper to wait for a process and get its exit code.
// timeout_s: -1 = infinite wait (original behavior), >= 0 = poll with timeout.
// If timeout expires: SIGTERM → wait 2s → SIGKILL.
static int wait_for_worker_and_get_exit_code(ProcessHandle handle, int timeout_s = -1)
{
    if (handle == NULL_PROC_HANDLE)
        return -1;

#if defined(PLATFORM_WIN64)
    DWORD wait_ms = (timeout_s < 0) ? INFINITE : static_cast<DWORD>(timeout_s * 1000);
    DWORD waitResult = WaitForSingleObject(handle, wait_ms);
    if (waitResult == WAIT_TIMEOUT)
    {
        TerminateProcess(handle, 1);
        WaitForSingleObject(handle, 5000);
    }
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
    if (timeout_s < 0)
    {
        // Infinite wait (original behavior)
        int status = 0;
        if (waitpid(handle, &status, 0) == -1)
            return -1;
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        return -1;
    }

    // Poll with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline)
    {
        int status = 0;
        pid_t result = waitpid(handle, &status, WNOHANG);
        if (result > 0)
        {
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            return -1;
        }
        if (result == -1)
            return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Timeout expired — escalate: SIGTERM → wait 2s → SIGKILL
    ::kill(handle, SIGTERM);
    auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < kill_deadline)
    {
        int status = 0;
        pid_t result = waitpid(handle, &status, WNOHANG);
        if (result > 0)
        {
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            return -1;
        }
        if (result == -1)
            return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // SIGKILL as last resort
    ::kill(handle, SIGKILL);
    int status = 0;
    waitpid(handle, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
#endif
}

WorkerProcess::WorkerProcess(const std::string &exe_path, const std::string &mode,
                             const std::vector<std::string> &args, bool redirect_stderr_to_console,
                             bool with_ready_signal)
    : mode_(mode),
      redirect_stderr_to_console_(redirect_stderr_to_console),
      with_ready_signal_(with_ready_signal)
{
    auto base_name = fs::path(exe_path).filename().string() + "_" + fs::path(mode).filename().string();
    // Sanitize: replace characters invalid in Windows filenames.
    for (char &c : base_name)
    {
        if (c == '.' || c == ':' || c == '\\' || c == '/' || c == ' ')
            c = '_';
    }
    auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    stdout_path_ = fs::temp_directory_path() / fmt::format("{}_{}_stdout.log", base_name, ts);

    if (!redirect_stderr_to_console_)
    {
        stderr_path_ = fs::temp_directory_path() / fmt::format("{}_{}_stderr.log", base_name, ts);
    }

    if (with_ready_signal_)
    {
        init_with_ready_signal(exe_path, mode, args, redirect_stderr_to_console_);
    }
    else
    {
        handle_ = spawn_worker_process(exe_path, mode, args, stdout_path_, stderr_path_,
                                       redirect_stderr_to_console_);
    }
}

void WorkerProcess::init_with_ready_signal(const std::string &exe_path, const std::string &mode,
                                            const std::vector<std::string> &args,
                                            bool redirect_stderr_to_console)
{
#if defined(PLATFORM_WIN64)
    handle_ = spawn_worker_process_with_ready_pipe(exe_path, mode, args, stdout_path_, stderr_path_,
                                                   redirect_stderr_to_console, &ready_pipe_read_);
#else
    handle_ = spawn_worker_process_with_ready_pipe(exe_path, mode, args, stdout_path_, stderr_path_,
                                                   redirect_stderr_to_console, &ready_pipe_read_);
#endif
}

void WorkerProcess::send_signal(int sig)
{
    if (handle_ == NULL_PROC_HANDLE || waited_)
        return;
#if defined(PLATFORM_WIN64)
    // Windows: TerminateProcess for SIGTERM equivalent.
    if (sig == SIGTERM)
        TerminateProcess(handle_, 1);
#else
    ::kill(handle_, sig);
#endif
}

WorkerProcess::~WorkerProcess()
{
#if defined(PLATFORM_WIN64)
    if (ready_pipe_read_ != NULL)
    {
        CloseHandle(ready_pipe_read_);
        ready_pipe_read_ = NULL;
    }
#else
    if (ready_pipe_read_ >= 0)
    {
        close(ready_pipe_read_);
        ready_pipe_read_ = -1;
    }
#endif
    if (handle_ != NULL_PROC_HANDLE && !waited_)
    {
        send_signal(SIGTERM);
        wait_for_exit(5);
    }
    std::error_code ec;
    fs::remove(stdout_path_, ec);
    if (!redirect_stderr_to_console_)
    {
        fs::remove(stderr_path_, ec);
    }
}

void WorkerProcess::wait_for_ready()
{
    if (!with_ready_signal_)
        return;
#if defined(PLATFORM_WIN64)
    if (ready_pipe_read_ == NULL)
        return;
    char buf[1];
    DWORD read = 0;
    if (ReadFile(ready_pipe_read_, buf, 1, &read, nullptr))
    {
        CloseHandle(ready_pipe_read_);
        ready_pipe_read_ = NULL;
    }
#else
    if (ready_pipe_read_ < 0)
        return;
    char buf[1];
    if (read(ready_pipe_read_, buf, 1) >= 0)
    {
        close(ready_pipe_read_);
        ready_pipe_read_ = -1;
    }
#endif
}

int WorkerProcess::wait_for_exit(int timeout_s)
{
    if (waited_)
        return exit_code_;

    exit_code_ = wait_for_worker_and_get_exit_code(handle_, timeout_s);
    waited_ = true;
    handle_ = NULL_PROC_HANDLE;

    read_file_contents(stdout_path_.string(), stdout_content_);
    if (!redirect_stderr_to_console_)
    {
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

void expect_worker_ok(const WorkerProcess &proc,
                      const std::vector<std::string> &required_substrings,
                      const std::vector<std::string> &expected_error_substrings,
                      bool require_completion_markers)
{
    using ::testing::HasSubstr;
    using ::testing::Not;

    // This check is fundamental and should always be performed.
    ASSERT_EQ(proc.exit_code(), 0) << "Worker process failed with non-zero exit code. Stderr:\n"
                                   << proc.get_stderr();

    // If stderr was redirected to console, we cannot and should not check its content.
    // The assertions are based on the captured stderr file.
    if (proc.get_stderr().empty() && proc.exit_code() == 0)
    {
        // This can happen if stderr was redirected to console.
        // We can't make assertions on stderr content, so we just check exit code.
        // A warning could be logged here if needed.
        std::cout << "[WARN] Stderr was not captured (likely redirected to console). Skipping "
                     "stderr content checks."
                  << std::endl;
        return;
    }

    const auto &stderr_out = proc.get_stderr();

    // ── Worker completion milestones (silent-shortcircuit catch) ────────────
    //
    // run_gtest_worker / run_worker_bare emit three stderr markers, in
    // order, around the test body:
    //
    //   [WORKER_BEGIN] <test_name>      — dispatcher routed correctly and the
    //                                     worker function entered run_*_worker
    //   [WORKER_END_OK] <test_name>     — test body returned WITHOUT throwing
    //                                     (no failed EXPECT_/ASSERT_; no
    //                                     std::exception escape)
    //   [WORKER_FINALIZED] <test_name>  — LifecycleGuard finalize completed
    //                                     cleanly (no module shutdown hang)
    //
    // We require all three *prefixes* (not the full test_name) so the check
    // works uniformly across legacy workers (which use "module::scenario"
    // naming) and new workers (which use "module.scenario"). What matters
    // for safety is that all three milestones appeared in order — that means
    // the body began, completed without throwing, and the lifecycle
    // finalized cleanly.
    //
    // Closes the "test passed because the body silently skipped its asserts"
    // loophole: an early return or unreachable lambda yields exit_code == 0
    // but no [WORKER_END_OK] → this check fails the parent test.
    //
    // Tests that intentionally exit non-zero (the PLH_PANIC abort tests) do
    // not call ExpectWorkerOk; they use a dedicated abort-checker that
    // asserts exit_code != 0 + panic text in stderr.
    if (require_completion_markers)
    {
        EXPECT_THAT(stderr_out, HasSubstr("[WORKER_BEGIN]"))
            << "worker did not emit [WORKER_BEGIN] — dispatcher may not have "
               "matched scenario '"
            << proc.mode() << "'.";
        EXPECT_THAT(stderr_out, HasSubstr("[WORKER_END_OK]"))
            << "worker did not reach end-of-body for '" << proc.mode()
            << "' — body short-circuited (early return, skip, or unreachable code).";
        EXPECT_THAT(stderr_out, HasSubstr("[WORKER_FINALIZED]"))
            << "worker did not complete LifecycleGuard finalize for '"
            << proc.mode() << "' — module shutdown hung or aborted.";
    }

    // expected_error_substrings has MULTISET semantics (established
    // 2026-04-20). Each entry in the list consumes EXACTLY ONE matching
    // [ERROR ] line in stderr:
    //
    //   - List `{"foo"}`   → exactly 1 ERROR line containing "foo"
    //   - List `{"foo", "foo"}` → exactly 2 ERROR lines containing "foo"
    //   - List `{"foo", "bar"}` → exactly 1 line with "foo" AND 1 line with "bar"
    //
    // Matching is greedy in stderr order: each error line consumes the
    // first remaining substring that matches it. Failures are:
    //   - an ERROR line with no remaining substring matching   (unexpected)
    //   - a substring with no ERROR line matching              (missing)
    //
    // When expected_error_substrings is empty: no [ERROR ]-level log
    // lines permitted.  Check line-by-line (NOT a loose
    // HasSubstr("ERROR")) — debug prints and raw stderr text may
    // legitimately contain the word "ERROR" (e.g. "ERROR: failed to
    // load" from a PLH_DEBUG line) without being a logger-ERROR event.
    if (expected_error_substrings.empty())
    {
        std::istringstream lines(stderr_out);
        std::string        line;
        while (std::getline(lines, line))
        {
            if (line.find("[ERROR ]") != std::string::npos)
                ADD_FAILURE()
                    << "Unexpected ERROR-level log in worker stderr "
                       "(expected_error_substrings was empty):\n  " << line;
        }
    }
    else
    {
        // Collect all [ERROR ] lines in stderr order.
        std::vector<std::string> error_lines;
        std::istringstream       lines(stderr_out);
        std::string              line;
        while (std::getline(lines, line))
        {
            if (line.find("[ERROR ]") != std::string::npos)
                error_lines.push_back(line);
        }

        // Multiset pairing: each error line consumes one remaining
        // substring entry. Walk error lines in stderr order and remove
        // the first matching substring from the work list.
        std::vector<std::string> remaining = expected_error_substrings;
        for (const auto &err_line : error_lines)
        {
            auto it = std::find_if(remaining.begin(), remaining.end(),
                [&](const std::string &sub) {
                    return err_line.find(sub) != std::string::npos;
                });
            if (it != remaining.end())
            {
                remaining.erase(it);
            }
            else
            {
                ADD_FAILURE()
                    << "Unexpected ERROR in worker stderr — no remaining "
                       "expected substring matches this line (either the "
                       "error is genuinely unexpected, or the substring "
                       "list undercounts — each entry consumes exactly "
                       "one line):\n  " << err_line;
            }
        }

        // Leftover substrings = expected errors that never appeared.
        for (const auto &unmatched : remaining)
        {
            ADD_FAILURE()
                << "Expected error substring not found in worker stderr (or "
                   "not as many occurrences as entries in the list): \""
                << unmatched << "\"";
        }
    }
    // Always forbid FATAL, PANIC, and worker assertion failure.
    EXPECT_THAT(stderr_out, Not(HasSubstr("FATAL")));
    EXPECT_THAT(stderr_out, Not(HasSubstr("PANIC")));
    EXPECT_THAT(stderr_out, Not(HasSubstr("[WORKER FAILURE]")));

    for (const auto &substr : required_substrings)
    {
        EXPECT_THAT(stderr_out, HasSubstr(substr));
    }
}

} // namespace pylabhub::tests::helper
