// tests/test_logger.cpp
//
// Unit test for pylabhub::util::Logger

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "platform.hpp"
#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>      // For EACCES
#include <fcntl.h>       // For open()
#include <signal.h>      // For kill()
#include <sys/file.h>    // For flock()
#include <sys/stat.h>  // For chmod, S_IRUSR, etc.
#include <sys/types.h> // For pid_t
#include <sys/wait.h>  // For waitpid() and associated macros
#include <unistd.h>    // For fork(), execl(), _exit()
#endif

#if PYLABHUB_IS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "utils/Logger.hpp"

using namespace pylabhub::utils;
namespace fs = std::filesystem;

// --- Test Harness ---
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition)                                                                        \
    do                                                                                          \
    {                                                                                           \
        if (!(condition))                                                                       \
        {                                                                                       \
            tests_failed++;                                                                     \
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{}", #condition, __FILE__, __LINE__);  \
            exit(1);                                                                            \
        }                                                                                       \
    } while (0)

#define FAIL_TEST(msg)                                                                 \
    do                                                                                          \
    {                                                                                           \
        tests_failed++;                                                                         \
        fmt::print(stderr, "  TEST FAILED: {} at {}:{}", msg, __FILE__, __LINE__);              \
        exit(1);                                                                                \
    } while (0)

// --- Test Globals & Helpers ---
static fs::path g_log_path;
static std::string g_self_exe_path;

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

static size_t count_lines(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return 0;
    size_t count = 0;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty())
        {
            count++;
        }
    }
    return count;
}

// --- Test Cases ---

void test_basic_logging()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.set_logfile(g_log_path.string(), false));
    L.set_level(Logger::Level::L_TRACE);
    L.flush(); // ensure startup message is written
    size_t lines_before = count_lines(g_log_path.string());

    LOGGER_INFO("unit-test: ascii message {}", 42);
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    L.flush();
    CHECK(!L.dirty());
    size_t lines_after = count_lines(g_log_path.string());
    CHECK(lines_after - lines_before == 3);

    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    CHECK(contents.find("unit-test: ascii message 42") != std::string::npos);
    CHECK(contents.find("unit-test: debug 3.14") != std::string::npos);
    CHECK(contents.find("☃") != std::string::npos);
    CHECK(contents.find("日本語") != std::string::npos);
}

void test_log_level_filtering()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.set_logfile(g_log_path.string(), false));
    L.flush(); // ensure startup message is written
    size_t lines_before = count_lines(g_log_path.string());

    L.set_level(Logger::Level::L_WARNING); // Only WARNING and above should be logged
    LOGGER_INFO("This should NOT be logged.");
    LOGGER_DEBUG("This should also NOT be logged.");
    LOGGER_WARN("This WARNING should be logged.");
    L.set_level(Logger::Level::L_TRACE);
    LOGGER_DEBUG("This DEBUG should now be logged.");

    L.flush();
    CHECK(!L.dirty());
    size_t lines_after = count_lines(g_log_path.string());
    CHECK(lines_after - lines_before == 2);

    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    CHECK(contents.find("This should NOT be logged.") == std::string::npos);
    CHECK(contents.find("This should also NOT be logged.") == std::string::npos);
    CHECK(contents.find("This WARNING should be logged.") != std::string::npos);
    CHECK(contents.find("This DEBUG should now be logged.") != std::string::npos);
}

void test_message_truncation()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.set_logfile(g_log_path.string(), false));
    L.set_level(Logger::Level::L_DEBUG);
    const size_t max_len = 32;
    L.set_max_log_line_length(max_len);
    std::string long_msg(100, 'A'); // 100 'A' characters

    LOGGER_INFO_RT(long_msg);

    L.flush();
    CHECK(!L.dirty());
    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    CHECK(contents.find("...[TRUNCATED]") != std::string::npos);
    CHECK(contents.find(long_msg) == std::string::npos);

    // Extract the logged body and check its length
    std::string line, discard;
    std::stringstream ss(contents);
    std::getline(ss, discard); // Discard the "switched destination" line
    std::getline(ss, line);    // This should be our truncated line

    // The format is "TIME [LEVEL] [tid=TID] BODY"
    // Find the start of the body by finding the last "] "
    auto body_start_pos = line.rfind("] ");
    CHECK(body_start_pos != std::string::npos);

    std::string body = line.substr(body_start_pos + 2);
    // The body should be exactly max_len characters long after truncation.
    CHECK(body.length() == max_len);
}

void test_bad_format_string()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.set_logfile(g_log_path.string(), false));
    L.set_level(Logger::Level::L_INFO);
    L.flush(); // ensure startup message is written
    size_t lines_before = count_lines(g_log_path.string());

    // Use the _RT macro to test the runtime format string checking.
    std::string bad_fmt = "Missing arg: {}";
    LOGGER_INFO_RT(bad_fmt);

    L.flush();
    CHECK(!L.dirty());
    size_t lines_after = count_lines(g_log_path.string());
    CHECK(lines_after - lines_before == 1);

    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    // The logger should report a format error internally.
    auto pos1 = contents.find("[FORMAT ERROR]");
    auto pos2 = contents.find("invalid format string"); // fmt may report this
    CHECK(pos1 != std::string::npos || pos2 != std::string::npos);
}

void test_multithreaded_logging()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.set_logfile(g_log_path.string(), false));
    L.set_level(Logger::Level::L_DEBUG);
    L.flush(); // ensure startup message is written
    size_t lines_before = count_lines(g_log_path.string());

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
    for (auto &t : threads)
    {
        t.join();
    }

    L.flush();
    CHECK(!L.dirty());
    size_t lines_after = count_lines(g_log_path.string());
    CHECK(lines_after - lines_before == THREADS * MESSAGES_PER_THREAD);

    L.shutdown();
}

void test_flush_waits_for_queue()
{
    const int THREADS = 4;
    const int MESSAGES_PER_THREAD = 100;
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.set_logfile(g_log_path.string(), false));
    L.set_level(Logger::Level::L_TRACE);
    L.flush(); // ensure startup message is written
    size_t lines_before = count_lines(g_log_path.string());

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [i]
            {
                for (int j = 0; j < MESSAGES_PER_THREAD; ++j)
                {
                    LOGGER_INFO("flush-test: thread={} msg={}", i, j);
                }
            });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    L.flush();
    CHECK(!L.dirty());

    size_t lines_after = count_lines(g_log_path.string());
    CHECK(lines_after - lines_before == THREADS * MESSAGES_PER_THREAD);
    L.shutdown();
}

void test_write_error_callback()
{
#if defined(PLATFORM_WIN64)
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();

    // On Windows, lock the file with an exclusive read handle
    HANDLE h = CreateFileA(g_log_path.string().c_str(), GENERIC_READ, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);

    L.set_logfile(g_log_path.string(), false);
    L.set_level(Logger::Level::L_INFO);

    std::atomic<bool> callback_invoked = false;
    std::atomic<int> error_code = 0;
    int initial_failures = L.write_failure_count();

    L.set_write_error_callback(
        [&](const std::string &msg)
        {
            callback_invoked = true;
            error_code = L.last_write_error_code();
            (void)msg;
        });

    LOGGER_INFO("This write should fail.");
    L.flush();
    L.shutdown();

    CHECK(callback_invoked.load());
    CHECK(L.write_failure_count() > initial_failures);
    CHECK(error_code.load() == ERROR_SHARING_VIOLATION);

    CloseHandle(h);
#else
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();

    {
        std::ofstream touch(g_log_path);
        touch << "initial content\n";
    }
    CHECK(fs::exists(g_log_path));

    // Make the file read-only so that the subsequent open() for writing fails.
    chmod(g_log_path.string().c_str(), S_IRUSR | S_IRGRP | S_IROTH); // 0444

    std::atomic<bool> callback_invoked = false;
    std::atomic<int> error_code = 0;
    int initial_failures = L.write_failure_count();

    L.set_write_error_callback(
        [&](const std::string &msg)
        {
            callback_invoked = true;
            // Note: last_write_error_code() is thread-safe (atomic)
            error_code = L.last_write_error_code();
            (void)msg;
        });

    // We expect this to fail because the file is read-only.
    // This will trigger the error callback.
    bool set_logfile_opened_successfully = L.set_logfile(g_log_path.string(), false);

    L.shutdown();

    CHECK(!set_logfile_opened_successfully);
    CHECK(callback_invoked.load());
    CHECK(L.write_failure_count() > initial_failures);
    CHECK(error_code.load() == EACCES);

    // cleanup
    chmod(g_log_path.string().c_str(), S_IRWXU | S_IWGRP | S_IWOTH);
#endif
}

void test_uninitialized_state_drops_logs()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    // In the uninitialized (default) state, destination is L_NONE.
    // These logs should be processed by the worker but dropped instead of written.
    LOGGER_INFO("This message should be dropped.");
    LOGGER_ERROR("This message should also be dropped.");
    L.flush(); // The flush should complete without writing anything.

    // Now, set a destination. Subsequent logs should be written.
    CHECK(L.set_logfile(g_log_path.string(), false));
    L.set_level(Logger::Level::L_TRACE);
    L.flush(); // ensure startup message is written
    size_t lines_before = count_lines(g_log_path.string());

    LOGGER_INFO("This message should be logged.");
    L.flush();
    size_t lines_after = count_lines(g_log_path.string());
    CHECK(lines_after - lines_before == 1);

    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    CHECK(contents.find("This message should be dropped.") == std::string::npos);
    CHECK(contents.find("This message should also be dropped.") == std::string::npos);
    CHECK(contents.find("This message should be logged.") != std::string::npos);
    // The log file should contain the "Switched log destination" message and the one after.
}

void test_set_console()
{
    // This test doesn't write to a file, it just checks that the
    // sink-switching logic for the console works as expected.
    // It's difficult to capture stderr here, so we mainly test that
    // the calls don't crash and that the internal state looks correct.
    Logger &L = Logger::instance();

    // Switch to console (from NONE)
    L.set_console();
    L.set_level(Logger::Level::L_INFO);
    LOGGER_INFO("This should go to console.");

    // Switch to file
    CHECK(L.set_logfile(g_log_path.string(), false));
    LOGGER_INFO("This should go to file.");

    // Switch back to console
    L.set_console();
    LOGGER_INFO("This should go back to console.");

    L.flush();
    L.shutdown();
    CHECK(true); // If we got here without crashing, consider it a pass.
}

#if PYLABHUB_IS_POSIX
// This test is designed to trigger a specific race condition:
// 1. A lock-holding process is forked to block our worker thread on `flock`.
// 2. We log a message, causing our worker to block on `flock`.
// 3. While the worker is blocked, we reconfigure the logger from the main thread.
// 4. We kill the lock-holder, unblocking the worker.
// The test verifies that the worker detects the configuration change and safely
// aborts the write to the old, now-stale file descriptor.
void test_reconfigure_while_flock_waiting()
{
    // 1. Setup files
    fs::path log_path1 = fs::temp_directory_path() / "pylabhub_race_test1.log";
    fs::path log_path2 = fs::temp_directory_path() / "pylabhub_race_test2.log";
    std::remove(log_path1.string().c_str());
    std::remove(log_path2.string().c_str());

    // 2. Fork a child process to hold the file lock.
    pid_t lock_holder_pid = fork();
    CHECK(lock_holder_pid != -1);

    if (lock_holder_pid == 0)
    {
        // --- Child (Lock Holder) Process ---
        // Open the file, acquire an exclusive lock, and wait to be killed.
        int fd = open(log_path1.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd == -1)
        {
            _exit(1);
        }
        if (::flock(fd, LOCK_EX) != 0)
        {
            _exit(2);
        }
        // Lock acquired. Now just wait. pause() waits for any signal.
        pause();
        _exit(0); // Should not be reached.
    }

    // --- Parent (Test) Process ---
    Logger &L = Logger::instance();

    // Give the lock holder a moment to start and acquire the lock.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 3. Configure logger to use the locked file.
    CHECK(L.set_logfile(log_path1.string(), true));
    L.set_level(Logger::Level::L_INFO);
    L.flush(); // Wait for "Switched log destination" message to be processed.

    // 4. Log a message. The worker will queue this and then block on flock.
    const char *test_message = "This message should be dropped.";
    LOGGER_INFO_RT(test_message);

    // 5. Give the worker thread a moment to run and block on flock().
    // This is not foolproof, but is the simplest way to test the race.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 6. While the worker is blocked, reconfigure the logger. This is the race.
    LOGGER_INFO("Switching log files.");
    CHECK(L.set_logfile(log_path2.string(), false)); // Switch to new file, no flock
    LOGGER_INFO("Switched to new file.");

    // 7. Kill the lock-holding child process to release the lock.
    kill(lock_holder_pid, SIGKILL);
    int status = 0;
    waitpid(lock_holder_pid, &status, 0); // Clean up the zombie process

    // 8. Flush and shut down. This should not hang.
    L.flush();
    L.shutdown();

    // 9. Verification
    std::string contents1, contents2;
    // The first file might not exist if nothing was ever written to it, which is ok.
    read_file_contents(log_path1.string(), contents1);
    CHECK(read_file_contents(log_path2.string(), contents2));

    // The first message should have been dropped because the fd changed.
    CHECK(contents1.find(test_message) == std::string::npos);

    // The new file should contain the reconfiguration messages.
    CHECK(contents2.find("Switching log files.") != std::string::npos);
    CHECK(contents2.find("Switched to new file.") != std::string::npos);
    CHECK(contents2.find("Switched log destination from") != std::string::npos);

    // Cleanup
    std::remove(log_path1.string().c_str());
    std::remove(log_path2.string().c_str());
}
#endif

// --- Multi-process Test Logic ---

// Entrypoint for child processes spawned by test_multiprocess_logging
void multiproc_child_main()
{
    Logger &L = Logger::instance();
    // The g_log_path is set by the --multiproc-child handler in main()
    if (!L.set_logfile(g_log_path.string(), true))
    {
        exit(2);
    }
    L.set_level(Logger::Level::L_TRACE);
    for (int i = 0; i < 20; ++i)
    {
#if PYLABHUB_IS_WINDOWS
        L.info_fmt("child-msg pid={} idx={}", GetCurrentProcessId(), i);
#else
        L.info_fmt("child-msg pid={} idx={}", getpid(), i);
#endif
    }
    L.info_fmt("child utf8 {}", "☃");
    L.flush();
    L.shutdown();
}
void test_multiprocess_logging()
{
    g_log_path = fs::temp_directory_path() / "pylabhub_multiprocess_test.log";
    std::remove(g_log_path.string().c_str());

    Logger &L = Logger::instance();
    CHECK(L.set_logfile(g_log_path.string(), true));
    L.set_level(Logger::Level::L_INFO);

    LOGGER_INFO("parent-process-start");
    L.flush();

    const int CHILDREN = 3;
    const int MESSAGES_PER_CHILD = 21; // 20 + 1 utf8

#if PYLABHUB_IS_WINDOWS
    std::vector<HANDLE> procs;
    for (int i = 0; i < CHILDREN; ++i)
    {
        std::string cmdline_str = fmt::format("\"{}\" --multiproc-child \"{}\"", g_self_exe_path,
                                              g_log_path.string());
        std::vector<char> cmdline(cmdline_str.begin(), cmdline_str.end());
        cmdline.push_back('\0');

        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &si, &pi))
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
                  g_log_path.string().c_str(), nullptr);
            _exit(127); // Should not be reached
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
    L.shutdown();

    size_t lines = count_lines(g_log_path.string());
    // Parent: 1 for set_logfile, 1 for 'start' message, 1 for shutdown.
    // Children: 1 for set_logfile, MESSAGES_PER_CHILD, 1 for shutdown.
    size_t expected_lines = 3 + (CHILDREN * (MESSAGES_PER_CHILD + 2));
    CHECK(lines == expected_lines);
    std::remove(g_log_path.string().c_str());
}

// --- Test Runner ---

// Spawns a child process to run a single, isolated test.
void run_test_in_process(const std::string &test_name)
{
    fmt::print("\n--- Running test: {} ---\n", test_name);

#if PYLABHUB_IS_WINDOWS
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
        fmt::print(stderr, "  --- FAILED: CreateProcessA failed ({}) ---\n", GetLastError());
        return;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code == 0)
    {
        tests_passed++;
        fmt::print("  --- PASSED ---\n");
    }
    else
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED in child process (exit code: {})", exit_code);
    }
#else
    pid_t pid = fork();
    if (pid == -1)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED: fork() failed ---\n");
        return;
    }

    if (pid == 0)
    {
        execl(g_self_exe_path.c_str(), g_self_exe_path.c_str(), "--run-test", test_name.c_str(),
              nullptr);
        _exit(127); // execl only returns on error
    }
    else
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        {
            tests_passed++;
            fmt::print("  --- PASSED ---\n");
        }
        else
        {
            tests_failed++;
            fmt::print(stderr, "  --- FAILED in child process ---\n");
        }
    }
#endif
}

int main(int argc, char **argv)
{
    g_self_exe_path = argv[0];
    g_log_path = fs::temp_directory_path() / "pylabhub_test_logger.log";

    // --- Child Process Entry Point ---
    if (argc > 1 && std::string(argv[1]) == "--run-test")
    {
        if (argc < 3)
        {
            return 1; // Test name not provided
        }
        const std::string test_name = argv[2];
        if (test_name == "test_basic_logging")
            test_basic_logging();
        else if (test_name == "test_log_level_filtering")
            test_log_level_filtering();
        else if (test_name == "test_message_truncation")
            test_message_truncation();
        else if (test_name == "test_bad_format_string")
            test_bad_format_string();
        else if (test_name == "test_multithreaded_logging")
            test_multithreaded_logging();
        else if (test_name == "test_flush_waits_for_queue")
            test_flush_waits_for_queue();
        else if (test_name == "test_uninitialized_state_drops_logs")
            test_uninitialized_state_drops_logs();
        else if (test_name == "test_set_console")
            test_set_console();
        else if (test_name == "test_multiprocess_logging")
            test_multiprocess_logging();
#if PYLABHUB_IS_POSIX
        else if (test_name == "test_write_error_callback")
            test_write_error_callback();
        else if (test_name == "test_reconfigure_while_flock_waiting")
            test_reconfigure_while_flock_waiting();
#endif
        else
        {
            fmt::print(stderr, "Unknown test name: {}", test_name);
            return 1;
        }
        exit(0); // Explicitly exit with success
    }

    if (argc > 1 && std::string(argv[1]) == "--multiproc-child")
    {
        if (argc < 3)
        {
            return 3; // Log path not provided
        }
        g_log_path = argv[2];
        multiproc_child_main();
        exit(0);
    }

    // --- Parent Process Test Runner ---
    fmt::print("--- Logger Test Suite (Process-Isolated) ---\n");

    const std::vector<std::string> test_names = {
        "test_basic_logging",
        "test_log_level_filtering",
        "test_message_truncation",
        "test_bad_format_string",
        "test_multithreaded_logging",
        "test_flush_waits_for_queue",
        "test_uninitialized_state_drops_logs",
        "test_set_console"};

    for (const auto &name : test_names)
    {
        run_test_in_process(name);
    }
#if PYLABHUB_IS_POSIX
    run_test_in_process("test_write_error_callback");
    run_test_in_process("test_reconfigure_while_flock_waiting");
#endif
    // The multi-process test is special and runs directly from the parent
    run_test_in_process("test_multiprocess_logging");

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}", tests_passed, tests_failed);

    std::error_code ec;
    fs::remove(g_log_path, ec);
    fs::remove(fs::temp_directory_path() / "pylabhub_multiprocess_test.log", ec);

    return tests_failed == 0 ? 0 : 1;
}