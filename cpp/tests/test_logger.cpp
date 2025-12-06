// tests/test_logger.cpp
//
// Unit test for pylabhub::util::Logger
//
// This test exercises:
//  - basic single-process logging (ASCII + UTF-8)
//  - multi-threaded logging (multiple threads writing concurrently)
//  - multi-process logging: POSIX forks; Windows spawn same exe with "--child"
//  - file sink verification by reading back the produced file.
//
// Behavior:
//  - When invoked as the child process (Windows), it does child-only logging and exits.
//  - Otherwise the parent runs the full test: spawns threads, spawns children, waits,
//    then shuts down the logger and verifies the file contains expected log lines.
//
// Notes:
//  - The test uses the Logger API and LOG_* macros you defined in Logger.hpp.
//  - The test expects the test binary to run from the build directory so CreateProcess
//    (Windows) can exec the same binary by path returned from GetModuleFileNameA.
//  - If your project places tests elsewhere, adjust OUTPATH accordingly.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "platform.hpp"
#include "utils/Logger.hpp"

using namespace pylabhub::utils;

static const std::string OUTPATH = "tests/output.log";

// Child entrypoint used on Windows (parent will spawn the same exe with "--child")
// On POSIX we fork and run this logic in child directly.
int run_as_child_main()
{
    Logger &L = Logger::instance();

    // Make sure child writes to same file (append)
    if (!L.init_file(OUTPATH, /*use_flock*/ true))
    {
        std::cerr << "child: init_file failed (errno=" << L.last_errno() << ")\n";
        return 2;
    }

    // Use trace level so everything is logged
    L.set_level(Logger::Level::L_TRACE);

    // Each child writes a small set of messages
    for (int i = 0; i < 50; ++i)
    {
        L.info_fmt("child-msg pid={} idx={}",
                   static_cast<int>(std::hash<std::thread::id>()(std::this_thread::get_id())), i);
    }

    // UTF-8 sanity message
    L.info_fmt("child utf8 {}", "☃");

    L.shutdown();
    return 0;
}

#if defined(PLATFORM_WIN64)
#include <windows.h>

// Spawn the same executable with "--child" argument. Returns child process handle or nullptr on
// error.
static PROCESS_INFORMATION spawn_child_windows(const std::string &exePath)
{
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::string cmd = std::string("\"") + exePath + "\" --child";
    // CreateProcess modifies the command buffer, so we need a mutable char array
    std::vector<char> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                             &si, &pi);
    if (!ok)
    {
        std::cerr << "CreateProcessA failed: " << GetLastError() << "\n";
        // Return zeroed PROCESS_INFORMATION
        PROCESS_INFORMATION empty{};
        return empty;
    }
    // parent: close thread handle but keep process handle to wait on it
    CloseHandle(pi.hThread);
    return pi;
}
#else
#include <sys/wait.h>
#include <unistd.h>

static pid_t spawn_child_posix()
{
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return -1;
    }
    if (pid == 0)
    {
        // child: run child logic
        int rc = run_as_child_main();
        _exit(rc);
    }
    // parent: return child's pid
    return pid;
}
#endif

// Read file contents into a string (binary mode)
static bool read_file_contents(const std::string &path, std::string &out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

int main(int argc, char **argv)
{
    // Windows child runner: invoked as "<exe> --child"
#if defined(PLATFORM_WIN64)
    if (argc > 1 && std::string(argv[1]) == "--child")
    {
        return run_as_child_main();
    }
#endif

    // Remove any previous test file
    std::remove(OUTPATH.c_str());

    Logger &L = Logger::instance();

    // Try to init several sinks to ensure those code paths are covered.
    // Primary verification will be done on file sink which is cross-platform.
    if (!L.init_file(OUTPATH, /*use_flock*/ true))
    {
        std::cerr << "parent: init_file failed (errno=" << L.last_errno() << ")\n";
        return 2;
    }

#if !defined(PLATFORM_WIN64)
    // POSIX: try opening syslog to exercise that path (no failure if not allowed)
    L.init_syslog("test_logger", LOG_PID | LOG_CONS, LOG_USER);
#else
    // Windows: try register event log source (non-fatal if fails)
    // Note: we avoid requiring admin rights; if init_eventlog fails, continue.
    L.init_eventlog(L"TestLoggerSource");
#endif

    L.set_level(Logger::Level::L_TRACE);

    // Basic single-process messages
    LOGGER_INFO("unit-test: ascii message {}", 42);
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    // Multi-threaded test
    const int THREADS = 8;
    const int MESSAGES_PER_THREAD = 200;
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back(
            [t]()
            {
                for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
                {
                    LOGGER_DEBUG("thread {} message {}", t, i);
                }
            });
    }
    for (auto &th : threads)
        th.join();

    // Multi-process test: spawn several child processes that also write to the same file.
    const int CHILDREN = 3;
#if defined(PLATFORM_WIN64)
    // Determine exe path
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        std::cerr << "GetModuleFileNameA failed\n";
        return 3;
    }

    std::vector<PROCESS_INFORMATION> procs;
    for (int i = 0; i < CHILDREN; ++i)
    {
        PROCESS_INFORMATION pi = spawn_child_windows(std::string(exePath));
        if (pi.hProcess == nullptr)
        {
            std::cerr << "spawn_child_windows failed\n";
            return 4;
        }
        procs.push_back(pi);
    }
    // Wait for each child to finish
    for (auto &pi : procs)
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        // retrieve exit code (optional)
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        (void)code;
    }
#else
    std::vector<pid_t> child_pids;
    for (int i = 0; i < CHILDREN; ++i)
    {
        pid_t pid = spawn_child_posix();
        if (pid < 0)
        {
            std::cerr << "fork/spawn failed\n";
            return 5;
        }
        child_pids.push_back(pid);
    }
    // wait for all children
    for (pid_t pid : child_pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
    }
#endif

    // Give small time to flush writes
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    L.shutdown();

    // Read file and verify content
    std::string contents;
    if (!read_file_contents(OUTPATH, contents))
    {
        std::cerr << "failed to open output log for verification\n";
        return 6;
    }

    // Basic content checks (fuzzy)
    if (contents.find("unit-test: ascii message 42") == std::string::npos)
    {
        std::cerr << "ascii message not found\n";
        return 7;
    }
    if (contents.find("unit-test: debug 3.14") == std::string::npos &&
        contents.find("unit-test: debug 3.14159") == std::string::npos)
    {
        std::cerr << "debug message not found\n";
        return 8;
    }
    if (contents.find("unit-test: utf8") == std::string::npos)
    {
        std::cerr << "utf8 indicator not found\n";
        return 9;
    }

    // Check for snowman UTF-8 (either literal or UTF-8 bytes)
    if (contents.find("☃") == std::string::npos &&
        contents.find(std::string("\xE2\x98\x83")) == std::string::npos)
    {
        std::cerr << "utf8 snowman not found\n";
        return 10;
    }

    // Count thread messages occurrences (fuzzy)
    size_t thread_occ = 0;
    const std::string thread_needle = "thread ";
    for (size_t pos = 0; pos < contents.size();)
    {
        pos = contents.find(thread_needle, pos);
        if (pos == std::string::npos)
            break;
        ++thread_occ;
        pos += thread_needle.size();
    }
    size_t expected_thread_msgs = THREADS * MESSAGES_PER_THREAD;
    if (thread_occ < expected_thread_msgs / 2)
    { // allow some loss or truncation
        std::cerr << "thread messages too few: found " << thread_occ << " expected ~"
                  << expected_thread_msgs << "\n";
        return 11;
    }

    // Roughly ensure children wrote something
    if (contents.find("child-msg") == std::string::npos)
    {
        std::cerr << "child messages not found\n";
        return 12;
    }

    std::cout << "test_logger: OK\n";
    return 0;
}
