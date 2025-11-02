// test_logger.cpp
//
// Cross-platform unit test for Logger (POSIX + Windows).
//
// - Tests file logging with multi-process + multi-thread writers
// - On Unix: uses fork() for child processes and syslog for best-effort check
// - On Windows: uses CreateProcess() to spawn child processes and OutputDebugString()
// - Validates log file contains expected number of lines
//
// Build (Linux/macOS):
//   g++ -std=c++17 -O2 test_logger.cpp logger.cpp -o test_logger -lpthread
//   # For full verbosity:
//   g++ -std=c++17 -O2 test_logger.cpp logger.cpp -o test_logger -lpthread -DLOGGER_COMPILE_LEVEL=0
//
// Build (Windows, MSVC):
//   cl /std:c++17 /EHsc test_logger.cpp logger.cpp /Fe:test_logger.exe
//   # For full verbosity add: /DLOGGER_COMPILE_LEVEL=0
//
// Run:
//   ./test_logger            # parent: spawns children and threads, validates log file
//
// Notes:
//   - The test will create a temporary file in the system temp directory.
//   - Ensure the logger implementation (logger.cpp / logger.hpp) is compiled with the
//     LOGGER_COMPILE_LEVEL you want; otherwise some macros may compile out.
//
#include "util/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <processthreadsapi.h>
#include <strsafe.h>
#include <tchar.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <syslog.h>
#endif

using namespace std::chrono_literals;

// Test parameters (adjust if needed)
static const int NUM_CHILD_PROCS = 3;
static const int MESSAGES_PER_CHILD = 40;
static const int NUM_THREADS = 4;
static const int MESSAGES_PER_THREAD = 50;

// Helper: create a temp logfile path and ensure the file exists.
// Portable approach: use system temp dir; create a unique filename using pid and timestamp.
static std::string create_temp_logfile()
{
    std::string tmpdir;
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetTempPathA(MAX_PATH, buf) == 0)
    {
        tmpdir = ".";
    }
    else
    {
        tmpdir = std::string(buf);
    }
    // generate a filename
    DWORD pid = GetCurrentProcessId();
    SYSTEMTIME st;
    GetSystemTime(&st);
    char name[256];
    snprintf(name, sizeof(name), "logger_test_%lu_%04d%02d%02d%02d%02d%03d.log", (unsigned long)pid,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wMilliseconds);
    std::string path = tmpdir + name;
    // create empty file
    std::ofstream ofs(path, std::ios::out);
    if (!ofs.is_open())
        return {};
    ofs.close();
    return path;
#else
    const char *envtmp = std::getenv("TMPDIR");
    tmpdir = envtmp ? envtmp : "/tmp";
    pid_t pid = getpid();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char name[256];
    snprintf(name, sizeof(name), "%s/logger_test_%d_%ld.log", tmpdir.c_str(), (int)pid,
             (long)ts.tv_nsec);
    // create file
    std::ofstream ofs(name, std::ios::out);
    if (!ofs.is_open())
        return {};
    ofs.close();
    return std::string(name);
#endif
}

// Helper: count lines in file
static size_t count_lines_in_file(const std::string &path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return 0;
    size_t lines = 0;
    std::string s;
    while (std::getline(in, s))
        ++lines;
    return lines;
}

// Child workload: write a set of messages and exit
static int child_workload_file(const std::string &logfile, int child_index)
{
    Logger *lg = get_global_logger();
    if (!lg)
        return 3;
    // Ensure logger is configured by parent (parent initializes logger to logfile).
    // Here we just write messages.
    for (int m = 0; m < MESSAGES_PER_CHILD; ++m)
    {
        int pid;
#if defined(_WIN32)
        pid = (int)GetCurrentProcessId();
#else
        pid = (int)getpid();
#endif
        LOGGER_INFO_LOC("child[%d] pid=%d msg=%d", child_index, pid, m);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#if defined(_WIN32)
    return 0;
#else
    _exit(0); // not reached on Windows
#endif
}

// On Windows, spawn a child (same exe) with command line "--child N <logfile>"
#if defined(_WIN32)
static bool spawn_child_process(const std::string &exe_path, int idx, const std::string &logfile)
{
    // Build command: "<exe_path>" --child=<idx> --logfile="<path>"
    std::ostringstream oss;
    oss << "\"" << exe_path << "\" --child=" << idx << " --logfile=\"" << logfile << "\"";
    std::string cmd = oss.str();

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // CreateProcessA expects mutable char*
    std::vector<char> cmdvec(cmd.begin(), cmd.end());
    cmdvec.push_back(0);

    BOOL ok = CreateProcessA(nullptr, cmdvec.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                             &si, &pi);
    if (!ok)
    {
        DWORD err = GetLastError();
        std::cerr << "CreateProcess failed (err=" << err << ")\n";
        return false;
    }
    // Close thread handle
    CloseHandle(pi.hThread);
    // Wait for process to finish
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    std::cout << "child process idx=" << idx << " exitCode=" << exitCode << "\n";
    return exitCode == 0;
}
#endif

// Entry point for child mode on Windows when launched with --child argument
static int run_child_mode_from_args(const std::string &logfile, int child_idx)
{
    // In child mode we assume parent previously set up logger file in shared location.
    // But on Windows, since each process has its own address space, parent must have
    // initialized the log file path and children must init the logger to same file path.
    // So children will call get_global_logger()->init_file(logfile, true).
    Logger *lg = get_global_logger();
    if (!lg)
    {
        std::cerr << "child: get_global_logger returned nullptr\n";
        return 3;
    }
    if (!lg->init_file(logfile, /*use_flock=*/true))
    {
        std::cerr << "child: init_file failed, errno=" << lg->last_errno() << "\n";
        // try continuing anyway
    }
    lg->set_level(Logger::Level::TRACE);
    for (int m = 0; m < MESSAGES_PER_CHILD; ++m)
    {
        int pid;
#if defined(_WIN32)
        pid = (int)GetCurrentProcessId();
#else
        pid = (int)getpid();
#endif
        LOGGER_INFO_LOC("child[%d] pid=%d msg=%d", child_idx, pid, m);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    lg->shutdown();
    return 0;
}

// Parse simple command-line args of form --child=N --logfile=PATH
static bool parse_child_args(int argc, char **argv, int &out_child, std::string &out_logfile)
{
    out_child = -1;
    out_logfile.clear();
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a.rfind("--child=", 0) == 0)
        {
            out_child = std::atoi(a.c_str() + 8);
        }
        else if (a.rfind("--logfile=", 0) == 0)
        {
            out_logfile = a.substr(10);
        }
    }
    return out_child >= 0 && !out_logfile.empty();
}

int main(int argc, char **argv)
{
    // Detect child mode (Windows path)
    int pid;
#if defined(_WIN32)
    int child_idx = -1;
    std::string child_logfile;
    if (parse_child_args(argc, argv, child_idx, child_logfile))
    {
        // Child process on Windows: initialize logger to same file and run child workload
        return run_child_mode_from_args(child_logfile, child_idx);
    }
#endif

    // Normal parent test flow
    std::string logfile = create_temp_logfile();
    if (logfile.empty())
    {
        std::cerr << "Failed to create temp logfile\n";
        return 2;
    }
    std::cout << "Using temp logfile: " << logfile << "\n";

    Logger *lg = get_global_logger();
    if (!lg)
    {
        std::cerr << "get_global_logger() returned nullptr\n";
        return 2;
    }

    // On POSIX, parent initializes logger once and children inherit fd via fork.
    // On Windows, child processes are separate address spaces: children must init logger
    // themselves.
#if defined(_WIN32)
    // For Windows: we do not init file in parent for children — each child will init to same path.
    // But for the parent thread-phase we still initialize parent logger to the same file.
    if (!lg->init_file(logfile, /*use_flock=*/true))
    {
        std::cerr << "init_file failed (errno=" << lg->last_errno() << ")\n";
        // continue to attempt spawn children (they will init themselves)
    }
#else
    if (!lg->init_file(logfile, /*use_flock=*/true))
    {
        std::cerr << "init_file failed (errno=" << lg->last_errno() << ")\n";
        return 2;
    }
#endif

    // Set verbose for test
    lg->set_level(Logger::Level::TRACE);

#if defined(_WIN32)
    pid = (int)GetCurrentProcessId();
#else
    pid = (int)getpid();
#endif
    LOGGER_INFO_LOC("test starting: pid=%d", pid);

    // --- Spawn child processes ---
#if defined(_WIN32)
    // Windows: spawn via CreateProcess
    // Need the current executable path.
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string exeStr = exePath;
    bool spawn_ok = true;
    for (int i = 0; i < NUM_CHILD_PROCS; ++i)
    {
        bool ok = spawn_child_process(exeStr, i, logfile);
        if (!ok)
        {
            std::cerr << "Failed to spawn child " << i << "\n";
            spawn_ok = false;
        }
    }
    if (!spawn_ok)
    {
        std::cerr << "One or more child processes failed\n";
    }
#else
    // POSIX fork children (do before threads)
    std::vector<pid_t> child_pids;
    for (int i = 0; i < NUM_CHILD_PROCS; ++i)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            return 2;
        }
        if (pid == 0)
        {
            // child: the child inherits the open file descriptor belonging to lg
            // just run the child workload and exit
            return child_workload_file(logfile, i);
        }
        else
        {
            child_pids.push_back(pid);
        }
    }
    // Parent waits for children
    for (pid_t pid : child_pids)
    {
        int status = 0;
        if (waitpid(pid, &status, 0) == -1)
        {
            perror("waitpid");
        }
        else
        {
            if (WIFEXITED(status))
            {
                int ec = WEXITSTATUS(status);
                std::cout << "child " << pid << " exited with " << ec << "\n";
            }
            else
            {
                std::cout << "child " << pid << " terminated abnormally\n";
            }
        }
    }
#endif

#if defined(_WIN32)
    pid = (int)GetCurrentProcessId();
#else
    pid = (int)getpid();
#endif
    LOGGER_INFO("All children finished (or spawn completed), now spawning threads in parent (pid=%d)",
             pid);

    // --- Multi-threaded logging in parent ---
    std::vector<std::thread> threads;
    std::atomic<int> started{0};

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back(
            [t, &started, &logfile]()
            {
                started.fetch_add(1, std::memory_order_relaxed);
                for (int m = 0; m < MESSAGES_PER_THREAD; ++m)
                {
                    std::ostringstream idoss;
                    idoss << std::this_thread::get_id();
                    int pid;
#if defined(_WIN32)
                    pid = (int)GetCurrentProcessId();
#else
                    pid = (int)getpid();
#endif
                    LOGGER_DEBUG_LOC("thread[%d] tid=%s pid=%d msg=%d", t, idoss.str().c_str(), pid,
                                  m);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
    }

    while (started.load(std::memory_order_relaxed) < NUM_THREADS)
    {
        std::this_thread::sleep_for(10ms);
    }
    for (auto &th : threads)
        th.join();

    LOGGER_WARN("Parent threads finished");

    std::this_thread::sleep_for(200ms);

    // Validate file contains expected number of lines:
    const size_t expected_lines = (size_t)NUM_CHILD_PROCS * MESSAGES_PER_CHILD +
                                  (size_t)NUM_THREADS * MESSAGES_PER_THREAD +
                                  5; // a few parent messages
    size_t found = count_lines_in_file(logfile);
    std::cout << "Log file '" << logfile << "' contains " << found
              << " lines (expected >= " << expected_lines << ")\n";

    bool ok = found >= expected_lines;
    if (!ok)
    {
        std::cerr << "Log file does not contain expected number of lines. Test FAILED.\n";
        std::ifstream in(logfile);
        if (in.is_open())
        {
            std::vector<std::string> lines;
            std::string s;
            while (std::getline(in, s))
                lines.push_back(s);
            int start = (int)lines.size() - 50;
            if (start < 0)
                start = 0;
            std::cerr << "---- last log lines (tail) ----\n";
            for (size_t i = start; i < lines.size(); ++i)
            {
                std::cerr << lines[i] << "\n";
            }
            std::cerr << "-------------------------------\n";
        }
    }
    else
    {
        std::cout << "File logging test PASSED\n";
    }

    // --- Syslog / platform log test (best-effort) ---
#if defined(_WIN32)
    std::cout << "Windows: emitting OutputDebugString messages for manual verification\n";
    // Use OutputDebugString for test messages - visible in debuggers or DebugView
    std::ostringstream os1;
    os1 << "test_logger: INFO pid=" << GetCurrentProcessId();
    OutputDebugStringA(os1.str().c_str());
    std::ostringstream os2;
    os2 << "test_logger: ERROR pid=" << GetCurrentProcessId();
    OutputDebugStringA(os2.str().c_str());
    std::cout << "OutputDebugString messages emitted. Use a debugger or DebugView to view them.\n";
#else
    std::cout << "Emitting a few messages to syslog for manual verification (check system journal "
                 "or /var/log)\n";
    lg->init_syslog("test_logger", LOG_PID | LOG_CONS, LOG_USER);
    LOGGER_INFO("syslog test INFO pid=%d", (int)getpid());
    LOGGER_WARN("syslog test WARN pid=%d", (int)getpid());
    LOGGER_ERROR("syslog test ERROR pid=%d", (int)getpid());
    std::this_thread::sleep_for(200ms);
    std::cout << "Syslog messages emitted (check system logs).\n";
#endif

    // Shutdown logger
    lg->shutdown();

    return ok ? 0 : 4;
}
