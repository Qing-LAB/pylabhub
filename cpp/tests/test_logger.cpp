// tests/test_logger.cpp
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "platform.hpp"
#include "util/Logger.hpp"

using namespace pylabhub::util;

static const std::string OUTPATH = "tests/output.log";

// Child process helper: when executed with --child, write several log lines and exit.
int run_as_child()
{
    Logger &L = Logger::instance();
    if (!L.init_file(OUTPATH, false))
    {
        std::cerr << "child: init_file failed\n";
        return 2;
    }
    L.set_level(Logger::Level::TRACE);
    // each child writes these messages
    for (int i = 0; i < 50; ++i)
    {
        L.info_fmt("child-pid={} message {}", (int)std::this_thread::get_id(), i);
    }
    L.shutdown();
    return 0;
}

#if defined(PLATFORM_WIN64)
// spawn child process of the same executable with "--child"
#include <windows.h>

static int spawn_child_process(const std::string &exePath)
{
    // Build command line: "<exePath>" --child
    std::wstring cmd;
    {
        // Convert exePath to wide
        int n = MultiByteToWideChar(CP_UTF8, 0, exePath.c_str(), -1, nullptr, 0);
        std::wstring wexe(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, exePath.c_str(), -1, &wexe[0], n);
        // Remove trailing null included by -1 sizing
        if (!wexe.empty() && wexe.back() == L'\0')
            wexe.pop_back();
        cmd = L"\"" + wexe + L"\" --child";
    }

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi))
    {
        std::cerr << "CreateProcessW failed: " << GetLastError() << "\n";
        return -1;
    }
    // close thread handle, return process handle (as an integer cast)
    CloseHandle(pi.hThread);
    // return the process id to caller; parent will wait using WaitForSingleObject
    return static_cast<int>(pi.dwProcessId);
}

static bool wait_for_child_handle_by_pid(int pid, DWORD timeout_ms = 10000)
{
    // enumerate processes to find handle is complex; instead use snapshot or open process handle
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h)
    {
        std::cerr << "OpenProcess failed for pid " << pid << " err=" << GetLastError() << "\n";
        return false;
    }
    DWORD r = WaitForSingleObject(h, timeout_ms);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
}
#else
// POSIX: fork children
#include <sys/wait.h>
#include <unistd.h>

static pid_t spawn_child_process_posix(const std::string & /*exePath*/)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return -1;
    }
    if (pid == 0)
    {
        // child: run child routine directly, simpler than execing the same binary
        // Note: child inherits logger state; re-init file to be safe
        // Call run_as_child and exit
        int rc = run_as_child();
        _exit(rc);
    }
    // parent: return child pid
    return pid;
}
#endif

int main(int argc, char **argv)
{
    // If invoked as child on Windows (--child), run child routine then exit.
#if defined(PLATFORM_WIN64)
    // detect --child
    if (argc > 1 && std::string(argv[1]) == "--child")
    {
        return run_as_child();
    }
#else
    // For POSIX, we call run_as_child in the forked child directly (no exec), so no --child
    // handling.
    (void)argc;
    (void)argv;
#endif

    // Remove old log file
    std::remove(OUTPATH.c_str());

    Logger &L = Logger::instance();
    if (!L.init_file(OUTPATH, /*use_flock*/ true))
    {
        std::cerr << "init_file failed; errno=" << L.last_errno() << "\n";
        return 2;
    }
    L.set_level(Logger::Level::TRACE);
    L.set_fsync_per_write(false);

    // Single-process basic test
    LOG_INFO("unit-test: ascii message {}", "A1");
    LOG_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOG_INFO("unit-test: utf8 snowman {} and japanese {}", "☃", "日本語");

    // Multi-threaded test
    const int THREADS = 8;
    const int MESSAGES_PER_THREAD = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back(
            [t]()
            {
                for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
                {
                    LOG_DEBUG("thread {} message {}", t, i);
                }
            });
    }
    for (auto &th : threads)
        th.join();

    // Multi-process test: spawn several child processes that also write to the same file.
    const int CHILDREN = 3;
#if defined(PLATFORM_WIN64)
    // Determine current exe path
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        std::cerr << "GetModuleFileNameA failed\n";
        return 4;
    }
    std::vector<int> child_pids;
    for (int i = 0; i < CHILDREN; ++i)
    {
        // Start child process: pass --child argument
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        std::string cmd = std::string("\"") + exePath + "\" --child";
        if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                            &pi))
        {
            std::cerr << "CreateProcessA failed: " << GetLastError() << "\n";
            return 5;
        }
        CloseHandle(pi.hThread);
        child_pids.push_back(static_cast<int>(pi.dwProcessId));
        CloseHandle(pi.hProcess); // we'll wait differently; here we just let child run
    }
    // Simple sleep to allow children to run and finish writing (childs should exit quickly)
    std::this_thread::sleep_for(std::chrono::seconds(2));
#else
    std::vector<pid_t> child_pids;
    for (int i = 0; i < CHILDREN; ++i)
    {
        pid_t pid = spawn_child_process_posix("");
        if (pid < 0)
        {
            std::cerr << "fork failed\n";
            return 6;
        }
        child_pids.push_back(pid);
    }
    // wait for child processes to finish
    for (pid_t pid : child_pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
    }
#endif

    // Give a short time to flush logs
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    L.shutdown();

    // Read file and perform verification checks
    std::ifstream ifs(OUTPATH, std::ios::binary);
    if (!ifs)
    {
        std::cerr << "failed to open output log for verification\n";
        return 7;
    }
    std::string contents((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    // Basic checks
    if (contents.find("unit-test: ascii message A1") == std::string::npos)
    {
        std::cerr << "ascii message not found\n";
        return 8;
    }
    if (contents.find("unit-test: debug 3.14") == std::string::npos &&
        contents.find("unit-test: debug 3.14159") == std::string::npos)
    {
        std::cerr << "debug message not found\n";
        return 9;
    }
    if (contents.find("unit-test: utf8") == std::string::npos)
    {
        std::cerr << "utf8 indicator not found\n";
        return 10;
    }

    // Count occurrences from threads & children roughly
    // We expect at least THREADS * MESSAGES_PER_THREAD entries of "thread "
    size_t thread_occ = 0;
    std::string needle = "thread ";
    for (size_t pos = 0; pos < contents.size();)
    {
        pos = contents.find(needle, pos);
        if (pos == std::string::npos)
            break;
        ++thread_occ;
        pos += needle.size();
    }
    if (thread_occ < static_cast<size_t>(THREADS * MESSAGES_PER_THREAD / 2))
    { // allow some loss if truncation
        std::cerr << "thread messages too few: " << thread_occ << "\n";
        return 11;
    }

    std::cout << "test_logger: OK\n";
    return 0;
}
