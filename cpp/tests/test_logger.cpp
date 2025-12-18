// tests/test_logger.cpp
//
// Unit test for pylabhub::util::Logger

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "utils/Logger.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>      // For EACCES
#include <fcntl.h>       // For open()
#include <signal.h>      // For kill()
#include <sys/stat.h>  // For chmod, S_IRUSR, etc.
#include <sys/types.h> // For pid_t
#include <sys/wait.h>  // For waitpid() and associated macros
#include <unistd.h>    // For fork(), execl(), _exit()
#endif

using namespace pylabhub::utils;
namespace fs = std::filesystem;

// --- Test Harness ---
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {
        if (!(condition))                                                                          \
        {
            tests_failed++;                                                                        \
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{}\\n", #condition, __FILE__, __LINE__);   \
            exit(1);                                                                               \
        }
    } while (0)

#define FAIL_TEST(msg)                                                                             \
    do                                                                                             \
    {
        tests_failed++;                                                                            \
        fmt::print(stderr, "  TEST FAILED: {} at {}:{}\\n", msg, __FILE__, __LINE__);               \
        exit(1);                                                                                   \
    } while (0)

// --- Test Globals & Helpers ---
static fs::path g_log_path;
static std::string g_self_exe_path;

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

static size_t count_lines(const std::string &s)
{
    size_t count = 0;
    for (char c : s)
    {
        if (c == '\n')
        {
            count++;
        }
    }
    return count;
}

static bool wait_for_string_in_file(const fs::path &path, const std::string &expected, \
                                    std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout)
    {
        std::string contents;
        if (read_file_contents(path.string(), contents))
        {
            if (contents.find(expected) != std::string::npos)
            {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// --- Test Cases ---

void test_basic_logging()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    L.set_logfile(g_log_path.string());
    L.set_level(Logger::Level::L_TRACE);

    CHECK(wait_for_string_in_file(g_log_path, "Switched log to file"));
    std::string contents_before;
    CHECK(read_file_contents(g_log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    LOGGER_INFO("unit-test: ascii message {}", 42);
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    L.flush();

    std::string contents_after;
    CHECK(read_file_contents(g_log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    CHECK(lines_after - lines_before == 3);

    CHECK(contents_after.find("unit-test: ascii message 42") != std::string::npos);
    CHECK(contents_after.find("unit-test: debug 3.14") != std::string::npos);
    CHECK(contents_after.find("☃") != std::string::npos);
    CHECK(contents_after.find("日本語") != std::string::npos);
}

void test_log_level_filtering()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    L.set_logfile(g_log_path.string());
    CHECK(wait_for_string_in_file(g_log_path, "Switched log to file"));

    std::string contents_before;
    CHECK(read_file_contents(g_log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    L.set_level(Logger::Level::L_WARNING);
    LOGGER_INFO("This should NOT be logged.");
    LOGGER_DEBUG("This should also NOT be logged.");
    LOGGER_WARN("This WARNING should be logged.");
    L.set_level(Logger::Level::L_TRACE);
    LOGGER_DEBUG("This DEBUG should now be logged.");

    L.flush();

    std::string contents_after;
    CHECK(read_file_contents(g_log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    CHECK(lines_after - lines_before == 2);

    CHECK(contents_after.find("This should NOT be logged.") == std::string::npos);
    CHECK(contents_after.find("This WARNING should be logged.") != std::string::npos);
    CHECK(contents_after.find("This DEBUG should now be logged.") != std::string::npos);
}

void test_bad_format_string()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    L.set_logfile(g_log_path.string());
    L.set_level(Logger::Level::L_INFO);
    CHECK(wait_for_string_in_file(g_log_path, "Switched log to file"));
    std::string bad_fmt = "Missing arg: {}";
    LOGGER_INFO_RT(bad_fmt); // This log call will internally create a "[FORMAT ERROR]" message
    CHECK(wait_for_string_in_file(g_log_path, "[FORMAT ERROR]"));
    // No other checks needed, if it contains the error, it works.
}

void test_default_sink_and_switching()
{
    Logger &L = Logger::instance();
    L.set_level(Logger::Level::L_INFO);
    LOGGER_INFO("This message should go to the default console sink (stderr).");
    L.flush();

    std::remove(g_log_path.string().c_str());
    L.set_logfile(g_log_path.string());
    LOGGER_INFO("This message should be logged to the file.");
    CHECK(wait_for_string_in_file(g_log_path, "This message should be logged to the file."));

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    CHECK(contents.find("This message should go to the default console sink") == std::string::npos);
    CHECK(contents.find("Switched log to file") != std::string::npos);
}

void test_multithread_stress()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    L.set_logfile(g_log_path.string());
    L.set_level(Logger::Level::L_DEBUG);
    CHECK(wait_for_string_in_file(g_log_path, "Switched log to file"));

    const int LOG_THREADS = 16;
    const int MESSAGES_PER_THREAD = 500;
    const int SINK_SWITCHES = 50;

    std::vector<std::thread> threads;
    threads.reserve(LOG_THREADS + 1);

    for (int t = 0; t < LOG_THREADS; ++t)
    {
        threads.emplace_back(
            [t]()
            {
                for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
                {
                    LOGGER_DEBUG("thread {} message {}", t, i);
                    if (i % 100 == 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
    }

    threads.emplace_back(
        [&]()
        {
            for (int i = 0; i < SINK_SWITCHES; ++i)
            {
                if (i % 2 == 0)
                    Logger::instance().set_console();
                else
                    Logger::instance().set_logfile(g_log_path.string());
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            Logger::instance().set_logfile(g_log_path.string());
        });

    for (auto &t : threads)
    {
        t.join();
    }
    L.flush();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));

    size_t found_threads = 0;
    for (int t = 0; t < LOG_THREADS; ++t)
    {
        if (contents.find(fmt::format("thread {} message", t)) != std::string::npos)
        {
            found_threads++;
        }
    }
    CHECK(found_threads > 0);
    CHECK(contents.find("Switched log to Console") != std::string::npos);
    CHECK(contents.find("Switched log to file") != std::string::npos);
}

void test_flush_waits_for_queue()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    L.set_logfile(g_log_path.string());
    L.set_level(Logger::Level::L_TRACE);
    CHECK(wait_for_string_in_file(g_log_path, "Switched log to file"));
    std::string contents_before;
    CHECK(read_file_contents(g_log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    const int MESSAGES = 500;
    for (int i = 0; i < MESSAGES; ++i)
    {
        LOGGER_INFO("flush-test: msg={}", i);
    }

    L.flush();

    std::string contents_after;
    CHECK(read_file_contents(g_log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    CHECK(lines_after - lines_before == MESSAGES);
}

void test_write_error_callback_async()
{
#if defined(_WIN32)
    std::remove(g_log_path.string().c_str());
    HANDLE h =
        CreateFileA(g_log_path.string().c_str(), GENERIC_READ, 0, nullptr, CREATE_ALWAYS, 
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);

    Logger &L = Logger::instance();
    std::atomic<bool> callback_invoked = false;
    L.set_write_error_callback(
        [&](const std::string &msg)
        {
            callback_invoked = true;
            CHECK(msg.find("Failed to open log file") != std::string::npos);
        });

    L.set_logfile(g_log_path.string());
    LOGGER_INFO("This write should be dropped as sink creation failed.");
    L.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(callback_invoked.load());
    CloseHandle(h);
#else
    fs::path readonly_dir = fs::temp_directory_path() / "pylab_readonly_dir_test";
    fs::remove_all(readonly_dir);
    fs::create_directory(readonly_dir);
    chmod(readonly_dir.string().c_str(), S_IRUSR | S_IXUSR); // 0500
    fs::path locked_log_path = readonly_dir / "test.log";

    Logger &L = Logger::instance();
    std::atomic<bool> callback_invoked = false;

    L.set_write_error_callback(
        [&](const std::string &msg)
        {
            callback_invoked = true;
            CHECK(msg.find("Failed to open log file") != std::string::npos);
        });

    L.set_logfile(locked_log_path.string());
    LOGGER_INFO("This message will be dropped.");
    L.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(callback_invoked.load());

    chmod(readonly_dir.string().c_str(), S_IRWXU);
    fs::remove_all(readonly_dir);
#endif
}

void test_platform_sinks()
{
    Logger &L = Logger::instance();
#if defined(_WIN32)
    L.set_eventlog(L"PyLabHubTestLogger");
    LOGGER_INFO("Testing Windows Event Log sink.");
#else
    L.set_syslog("pylab-logger-test");
    LOGGER_INFO("Testing syslog sink.");
#endif
    L.flush();
    CHECK(true);
}

void multiproc_child_main()
{
    Logger &L = Logger::instance();
    if (!L.set_logfile(g_log_path.string(), true))
    {
        exit(2);
    }
    L.set_level(Logger::Level::L_TRACE);
    for (int i = 0; i < 100; ++i)
    {
#if defined(_WIN32)
        LOGGER_INFO("child-msg pid={} idx={}", GetCurrentProcessId(), i);
#else
        LOGGER_INFO("child-msg pid={} idx={}", getpid(), i);
#endif
    }
    L.flush();
}

void test_multiprocess_logging()
{
    g_log_path = fs::temp_directory_path() / "pylabhub_multiprocess_test.log";
    std::remove(g_log_path.string().c_str());

    Logger &L = Logger::instance();
    L.set_logfile(g_log_path.string(), true);
    L.set_level(Logger::Level::L_INFO);
    LOGGER_INFO("parent-process-start");
    L.flush();

    const int CHILDREN = 5;
    const int MESSAGES_PER_CHILD = 100;

#if defined(_WIN32)
    std::vector<HANDLE> procs;
    for (int i = 0; i < CHILDREN; ++i)
    {
        std::string cmdline_str =
            fmt::format("\"{}\" --multiproc-child \"{}\"", g_self_exe_path, g_log_path.string());
        std::vector<char> cmdline(cmdline_str.begin(), cmdline_str.end());
        cmdline.push_back('\0');

        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                             &si, &pi))
        {
            FAIL_TEST("CreateProcessA failed");
        }
        CloseHandle(pi.hThread);
        procs.push_back(pi.hProcess);
    }
    for (auto &h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(h, &code);
        CloseHandle(h);
        CHECK(code == 0);
    }
#else
    std::vector<pid_t> child_pids;
    for (int i = 0; i < CHILDREN; ++i)
    {
        pid_t pid = fork();
        CHECK(pid != -1);
        if (pid == 0)
        {
            execl(g_self_exe_path.c_str(), g_self_exe_path.c_str(), "--multiproc-child",
                  g_log_path.string().c_str(), (char *)nullptr);
            _exit(127);
        }
        child_pids.push_back(pid);
    }
    for (pid_t pid : child_pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
#endif
    L.flush();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    size_t child_msg_count = 0;
    std::stringstream ss(contents);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.find("child-msg") != std::string::npos)
        {
            child_msg_count++;
        }
    }
    CHECK(child_msg_count == CHILDREN * MESSAGES_PER_CHILD);
    std::remove(g_log_path.string().c_str());
}

// --- Test Runner ---

void run_test_in_process(const std::string &test_name)
{
    fmt::print("\n--- Running test: {} ---\\n", test_name);
#if defined(_WIN32)
    std::string cmdline_str = fmt::format("\"{}\" --run-test {}", g_self_exe_path, test_name);
    std::vector<char> cmdline(cmdline_str.begin(), cmdline_str.end());
    cmdline.push_back('\0');
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                         &pi))
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED: CreateProcessA failed ({}) ---\\n", GetLastError());
        return;
    }
    WaitForSingleObject(pi.hProcess, 15000); // 15 second timeout
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    pid_t pid = fork();
    if (pid == -1)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED: fork() failed ---\\n");
        return;
    }

    if (pid == 0)
    {
        execl(g_self_exe_path.c_str(), g_self_exe_path.c_str(), "--run-test", test_name.c_str(),
              (char *)nullptr);
        _exit(127); // execl only returns on error
    }
    else
    {
        int status = 0;
        waitpid(pid, &status, 0); // no timeout, rely on test logic to not hang
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            tests_passed++;
            fmt::print("  --- PASSED ---\\n");
        }
        else
        {
            tests_failed++;
            fmt::print(stderr, "  --- FAILED in child process ---\\n");
        }
    }
#endif
}

int main(int argc, char **argv)
{
    g_self_exe_path = argv[0];
    g_log_path = fs::temp_directory_path() / "pylabhub_test_logger.log";

    if (argc > 1 && std::string(argv[1]) == "--run-test")
    {
        if (argc < 3)
            return 1;
        const std::string test_name = argv[2];

        if (test_name == "test_basic_logging")
            test_basic_logging();
        else if (test_name == "test_log_level_filtering")
            test_log_level_filtering();
        else if (test_name == "test_bad_format_string")
            test_bad_format_string();
        else if (test_name == "test_default_sink_and_switching")
            test_default_sink_and_switching();
        else if (test_name == "test_multithread_stress")
            test_multithread_stress();
        else if (test_name == "test_flush_waits_for_queue")
            test_flush_waits_for_queue();
        else if (test_name == "test_write_error_callback_async")
            test_write_error_callback_async();
        else if (test_name == "test_platform_sinks")
            test_platform_sinks();
        else if (test_name == "test_multiprocess_logging")
            test_multiprocess_logging();
        else
        {
            fmt::print(stderr, "Unknown test name: {}$\\n", test_name);
            return 1;
        }
        // Success is exit code 0, which is default.
        // CHECK failures exit(1)
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "--multiproc-child")
    {
        if (argc < 3)
            return 3;
        g_log_path = argv[2];
        multiproc_child_main();
        return 0;
    }

    fmt::print("--- Logger Test Suite (Process-Isolated) ---\\n");
    const std::vector<std::string> test_names = {
        "test_basic_logging",          "test_log_level_filtering",
        "test_bad_format_string",      "test_default_sink_and_switching",
        "test_multithread_stress",     "test_flush_waits_for_queue",
        "test_write_error_callback_async", "test_platform_sinks",
        "test_multiprocess_logging"};

    for (const auto &name : test_names)
    {
        run_test_in_process(name);
    }

    fmt::print("\\n--- Test Summary ---\\n");
    fmt::print("Passed: {}, Failed: {}$\\n", tests_passed, tests_failed);

    std::error_code ec;
    fs::remove(g_log_path, ec);
    fs::remove(fs::temp_directory_path() / "pylabhub_multiprocess_test.log", ec);
    fs::remove_all(fs::temp_directory_path() / "pylab_readonly_dir_test", ec);

    return tests_failed == 0 ? 0 : 1;
}
