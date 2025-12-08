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
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "platform.hpp"
#if PYLABHUB_IS_POSIX
#include <cerrno>      // For EACCES
#include <sys/stat.h>  // For chmod, S_IRUSR, etc.
#include <sys/types.h> // For pid_t
#include <sys/wait.h>  // For waitpid() and associated macros
#include <unistd.h>    // For fork(), execl(), _exit()
#endif

#include "utils/Logger.hpp"

using namespace pylabhub::utils;
namespace fs = std::filesystem;

// --- Test Harness: Explicit Registration ---
// Tests are explicitly registered in a map inside main() to avoid static
// initialization issues. Each test is run in an isolated process.

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{}\n", #condition, __FILE__, __LINE__);    \
            throw std::runtime_error("Test case failed");                                          \
        }                                                                                          \
    } while (0)

// This function is called in the child process to run a single, isolated test.
void run_test_and_exit(const std::string &name, std::function<void()> test_func)
{
    fmt::print("\n=== {} ===\n", name);
    try
    {
        test_func();
        fmt::print("  --- PASSED ---\n");
        exit(0); // Exit with success code
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "  --- FAILED: {} ---\n", e.what());
    }
    catch (...)
    {
        fmt::print(stderr, "  --- FAILED with unknown exception ---\n");
    }
    exit(1); // Exit with failure code
}


// --- Test Globals & Helpers ---
static fs::path g_log_path;

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

// Child entrypoint used for the multi-process stress test.
int run_as_child_main(const std::string &log_path)
{
    Logger &L = Logger::instance();

    // Make sure child writes to same file (append)
    if (!L.init_file(log_path, /*use_flock*/ true))
    {
        fmt::print(stderr, "child: init_file failed (errno={})\n", L.last_errno());
        return 2;
    }

    // Use trace level so everything is logged
    L.set_level(Logger::Level::L_TRACE);

    // Each child writes a small set of messages
    for (int i = 0; i < 20; ++i)
    {
        L.info_fmt("child-msg pid={} idx={}",
                   static_cast<int>(std::hash<std::thread::id>()(std::this_thread::get_id())), i);
    }

    // UTF-8 sanity message
    L.info_fmt("child utf8 {}", "☃");
    L.flush();
    CHECK(!L.dirty());
    L.shutdown();
    return 0;
}

#if defined(PLATFORM_WIN64)
// Spawns the same executable with "--child" argument for the stress test.
static PROCESS_INFORMATION spawn_child_windows(const std::string &exePath,
                                               const std::string &logPath)
{
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::string cmd = fmt::format("\"{}\" --child \"{}\"", exePath, logPath);
    // CreateProcess modifies the command buffer, so we need a mutable char array
    std::vector<char> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                             &si, &pi);
    if (!ok)
    {
        fmt::print(stderr, "CreateProcessA failed: {}\n", GetLastError());
        PROCESS_INFORMATION empty{};
        return empty;
    }
    CloseHandle(pi.hThread);
    return pi;
}
#else
// Spawns the same executable with "--child" argument for the stress test.
static pid_t spawn_child_posix(const std::string &exePath, const std::string &logPath)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return -1;
    }
    if (pid == 0)
    {
        // Child process: replace its image with a new instance of this executable
        execl(exePath.c_str(), exePath.c_str(), "--child", logPath.c_str(), nullptr);
        _exit(127); // Should not be reached if execl is successful
    }
    return pid;
}
#endif




// Read file contents into a string (binary mode)
void test_basic_logging()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_TRACE);

    LOGGER_INFO("unit-test: ascii message {}", 42);
    L.flush(); // Diagnostic flush
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    L.flush(); // Diagnostic flush
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    L.flush();
    CHECK(!L.dirty());
    L.shutdown();

    std::string contents;
    fmt::print("Reading log file: {}\n", g_log_path.string());
    CHECK(read_file_contents(g_log_path.string(), contents));
    fmt::print("contents: {}\n", contents);
    CHECK(contents.find("unit-test: ascii message 42") != std::string::npos);
    CHECK(contents.find("unit-test: debug 3.14") != std::string::npos);
    CHECK(contents.find("☃") != std::string::npos);
    CHECK(contents.find("日本語") != std::string::npos);
    CHECK(count_lines(g_log_path.string()) == 3);
}

void test_log_level_filtering()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_WARNING); // Only WARNING and above should be logged

    LOGGER_INFO("This should NOT be logged.");
    LOGGER_DEBUG("This should also NOT be logged.");
    LOGGER_WARN("This WARNING should be logged.");
    L.set_level(Logger::Level::L_TRACE);
    LOGGER_DEBUG("This DEBUG should now be logged.");

    L.flush();
    CHECK(!L.dirty());
    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    CHECK(contents.find("This should NOT be logged.") == std::string::npos);
    CHECK(contents.find("This should also NOT be logged.") == std::string::npos);
    CHECK(contents.find("This WARNING should be logged.") != std::string::npos);
    CHECK(contents.find("This DEBUG should now be logged.") != std::string::npos);
    CHECK(count_lines(g_log_path.string()) == 2);
}

void test_message_truncation()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), false));
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
    std::string line;
    std::stringstream ss(contents);
    std::getline(ss, line);

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
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_INFO);

    // Use the _RT macro to test the runtime format string checking.
    std::string bad_fmt = "Missing arg: {}";
    LOGGER_INFO_RT(bad_fmt);

    L.flush();
    CHECK(!L.dirty());
    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    // The logger should report a format error internally.
    auto pos1 = contents.find("[FORMAT ERROR]");
    auto pos2 = contents.find("invalid format string"); // fmt may report this
    CHECK(pos1 != std::string::npos || pos2 != std::string::npos);
    CHECK(count_lines(g_log_path.string()) == 1);
}

void test_multithreaded_logging()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_DEBUG);

    const int THREADS = 8;
    const int MESSAGES_PER_THREAD = 200;
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back([t]() {
            for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
            {
                LOGGER_DEBUG("thread {} message {}", t, i);
                (void)t;
            }
        });
    }
    for (auto &t : threads)
    {
        t.join();
    }

    L.flush();
    CHECK(!L.dirty());
    L.shutdown();

    size_t lines = count_lines(g_log_path.string());
    CHECK(lines == THREADS * MESSAGES_PER_THREAD);
}

void test_flush_waits_for_queue()
{
    const int THREADS = 4;
    const int MESSAGES_PER_THREAD = 100;

    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_TRACE);

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([i] {
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

    // At this point, messages are likely still in the queue or being processed.
    // Call flush() to synchronously wait for the worker to drain the queue.
    L.flush();
    CHECK(!L.dirty());

    // Now that flush() has returned, the file MUST contain all messages.
    size_t lines = count_lines(g_log_path.string());
    CHECK(lines == THREADS * MESSAGES_PER_THREAD);

    // Now it's safe to shut down.
    L.flush();
    CHECK(!L.dirty());
    L.shutdown();
}



#if PYLABHUB_IS_POSIX
void test_write_error_callback()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();

    // Create a file that we can then make read-only
    {
        std::ofstream touch(g_log_path);
        touch << "initial content\n";
    }
    CHECK(fs::exists(g_log_path));

    // Set permissions to read-only
    chmod(g_log_path.string().c_str(), S_IRUSR | S_IRGRP | S_IROTH); // 0444

    // Re-initialize the logger. On POSIX, open() with O_WRONLY will fail on
    // a read-only file. We don't CHECK the result, we check the callback.
    L.init_file(g_log_path.string(), false);
    L.set_level(Logger::Level::L_INFO);
    L.set_fsync_per_write(true); // Force fsync to ensure the OS reports the write error

    std::atomic<bool> callback_invoked = false;
    std::atomic<int> error_code = 0;
    int initial_failures = L.write_failure_count();

    L.set_write_error_callback([&](const std::string &msg) {
        callback_invoked = true;
        error_code = L.last_write_error_code();
        (void)msg;
    });

    LOGGER_INFO("This write should fail.");

    L.flush();
    CHECK(!L.dirty());

    L.shutdown(); // This will flush, ensuring the write is attempted.

    CHECK(callback_invoked.load());
    CHECK(L.write_failure_count() > initial_failures);
    CHECK(error_code.load() == EACCES); // EACCES is "Permission denied"

    // Cleanup: make writable again so we can remove it
    chmod(g_log_path.string().c_str(), S_IRWXU | S_IWGRP | S_IWOTH);
}
#endif

// Global variable to hold the path to the current executable.
static std::string g_self_exe_path;

void test_multiprocess_logging()
{
    fs::path multiprocess_log_path = fs::temp_directory_path() / "pylabhub_multiprocess_test.log";
    std::remove(multiprocess_log_path.string().c_str());

    Logger &L = Logger::instance();
    CHECK(L.init_file(multiprocess_log_path.string(), true)); // Use flock for multi-process
    L.set_level(Logger::Level::L_INFO);

    LOGGER_INFO("parent-process-start");
    L.flush(); // Ensure parent's message is written before children start


    const int CHILDREN = 3;
    const int MESSAGES_PER_CHILD = 20;

#if defined(PLATFORM_WIN64)
    std::vector<PROCESS_INFORMATION> procs;
    for (int i = 0; i < CHILDREN; ++i)
    {
        PROCESS_INFORMATION pi = spawn_child_windows(g_self_exe_path, multiprocess_log_path.string());
        if (pi.hProcess == nullptr)
        {
            throw std::runtime_error("Failed to spawn child process");
        }
        procs.push_back(pi);
    }
    for (auto &pi : procs)
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CHECK(code == 0);
    }
#else
    std::vector<pid_t> child_pids;
    for (int i = 0; i < CHILDREN; ++i)
    {
        pid_t pid = spawn_child_posix(g_self_exe_path, multiprocess_log_path.string());
        if (pid < 0)
        {
            throw std::runtime_error("Failed to fork child process");
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
    CHECK(!L.dirty());
    L.shutdown();

    size_t lines = count_lines(multiprocess_log_path.string());
    size_t expected_lines = 1 /* parent start */ + (CHILDREN * (MESSAGES_PER_CHILD + 1 /* utf8 */));
    CHECK(lines == expected_lines);
    std::remove(multiprocess_log_path.string().c_str());
}

int main(int argc, char **argv)
{
    // Store path to self for multi-process test
    g_self_exe_path = argv[0];

    // Explicitly register all tests to avoid static initialization issues.
    // Use a vector of pairs to preserve order.
    std::vector<std::pair<std::string, std::function<void()>>> tests;
    tests.emplace_back("Basic Logging and File Sink", test_basic_logging);
    tests.emplace_back("Log Level Filtering", test_log_level_filtering);
    tests.emplace_back("Message Truncation", test_message_truncation);
    tests.emplace_back("Bad Format String Handling", test_bad_format_string);
    tests.emplace_back("Multi-threaded Logging", test_multithreaded_logging);
    tests.emplace_back("Flush Waits For Queue", test_flush_waits_for_queue);
#if PYLABHUB_IS_POSIX
    tests.emplace_back("Write Error Callback (POSIX-only)", test_write_error_callback);
    // The multi-process test is special and will be run last.
#endif

    // --- Child Process Entry Point (for isolated tests) ---
    if (argc > 1 && std::string(argv[1]) == "--run-test")
    {
        if (argc < 3)
        {
            fmt::print(stderr, "Child mode '--run-test' requires a test name argument.\n");
            return 1;
        }
        std::string test_to_run = argv[2];
        
        // Re-create the map to find the test function by name.
        std::map<std::string, std::function<void()>> test_map(tests.begin(), tests.end());
#if PYLABHUB_IS_POSIX
        test_map["Multi-process Logging"] = test_multiprocess_logging;
#endif

        if (test_map.count(test_to_run))
        {
            g_log_path = fs::temp_directory_path() / "pylabhub_test_logger.log";
            run_test_and_exit(test_to_run, test_map[test_to_run]);
        }
        else
        {
            fmt::print(stderr, "Test '{}' not found in registry.\n", test_to_run);
            return 1;
        }
        return 0; // Should not be reached
    }

    // --- Grandchild Process Entry Point (for multi-process stress test) ---
    if (argc > 1 && std::string(argv[1]) == "--child")
    {
        if (argc < 3)
        {
            fmt::print(stderr, "Child mode requires log path argument.\n");
            return 1;
        }
        return run_as_child_main(argv[2]);
    }

    // --- Parent Process Test Runner ---
    fmt::print("--- Logger Test Suite (Process-Isolated) ---\n");
#if defined(_LOGGER_DEBUG_ENABLED)
    fmt::print("Logger debug mode is ENABLED.\n");
#else
    fmt::print("Logger debug mode is DISABLED.\n");
#endif
    auto run_test_process = [&](const std::string& name) {
        fmt::print("\n--- Spawning test: {} ---\n", name);
#if defined(PLATFORM_WIN64)
        // Windows process spawning logic would go here
        // For now, we just increment failed count on Windows for tests that need fork.
        if (name == "Multi-process Logging") {
            fmt::print("Skipping test '{}' on Windows in this example.\n", name);
            return;
        }
#endif
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            tests_failed++;
            return;
        }
        if (pid == 0) { // Child process
            execl(argv[0], argv[0], "--run-test", name.c_str(), nullptr);
            _exit(127); // execl only returns on error
        }
        else { // Parent process
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                tests_passed++;
            } else {
                tests_failed++;
                fmt::print(stderr, "--- Test '{}' FAILED in child process ---\n", name);
            }
        }
    };
    
    // Run all standard tests sequentially.
    for (const auto& [name, func] : tests)
    {
        run_test_process(name);
    }

#if PYLABHUB_IS_POSIX
    // Run the multi-process test last and by itself.
    run_test_process("Multi-process Logging");
#endif

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    // Final cleanup
    std::error_code ec;
    fs::remove(fs::temp_directory_path() / "pylabhub_test_logger.log", ec);
    fs::remove(fs::temp_directory_path() / "pylabhub_multiprocess_test.log", ec);

    return tests_failed == 0 ? 0 : 1;
}
