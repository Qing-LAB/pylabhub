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

#include "platform.hpp"
#if PYLABHUB_IS_POSIX
#include <cerrno>      // For EACCES
#include <sys/stat.h>  // For chmod
#endif

#include "utils/Logger.hpp"

using namespace pylabhub::utils;
namespace fs = std::filesystem;

// --- Minimal Test Harness (from test_atomicguard.cpp) ---
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

void TEST_CASE(const std::string &name, std::function<void()> test_func)
{
    fmt::print("\n=== {} ===\n", name);
    try
    {
        test_func();
        tests_passed++;
        fmt::print("  --- PASSED ---\n");
    }
    catch (const std::exception &e)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED: {} ---\n", e.what());
    }
    catch (...)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED with unknown exception ---\n");
    }
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

// Child entrypoint used on Windows (parent will spawn the same exe with "--child")
// On POSIX we fork and run this logic in child directly.
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

    L.shutdown();
    return 0;
}

#if defined(PLATFORM_WIN64)
// Spawn the same executable with "--child" argument. Returns child process handle or nullptr on
// error.
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
        // Return zeroed PROCESS_INFORMATION
        PROCESS_INFORMATION empty{};
        return empty;
    }
    // parent: close thread handle but keep process handle to wait on it
    CloseHandle(pi.hThread);
    return pi;
}
#else
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
        // Child process: replace its image with a new instance of this executable,
        // running in child mode. This is the safe way to handle multi-process
        // testing with threaded singletons.
        execl(exePath.c_str(), exePath.c_str(), "--child", logPath.c_str(), nullptr);
        _exit(127); // Should not be reached if execl is successful
    }
    // parent: return child's pid
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
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    L.shutdown(); // This will flush and close.

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
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
    LOGGER_ERROR("This ERROR should be logged.");

    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    CHECK(contents.find("This should NOT be logged.") == std::string::npos);
    CHECK(contents.find("This should also NOT be logged.") == std::string::npos);
    CHECK(contents.find("This WARNING should be logged.") != std::string::npos);
    CHECK(contents.find("This ERROR should be logged.") != std::string::npos);
    CHECK(count_lines(g_log_path.string()) == 2);
}

void test_message_truncation()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_DEBUG);
    const size_t max_len = 50;
    L.set_max_log_line_length(max_len);

    std::string long_msg(100, 'A');
    LOGGER_INFO("long message: {}", long_msg);

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

    // These should be caught by the try/catch in log_fmt and logged as an error
    LOGGER_INFO("Missing arg: {}", /* missing */);
    LOGGER_INFO("Type mismatch: {:d}", "not a number");

    L.shutdown();

    std::string contents;
    CHECK(read_file_contents(g_log_path.string(), contents));
    // The logger should report a format error internally.
    auto pos1 = contents.find("[FORMAT ERROR]");
    auto pos2 = contents.find("invalid format string"); // fmt may report this
    CHECK(pos1 != std::string::npos || pos2 != std::string::npos);
    CHECK(count_lines(g_log_path.string()) == 2);
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
            }
        });
    }
    for (auto &th : threads)
        th.join();

    L.shutdown();

    size_t lines = count_lines(g_log_path.string());
    CHECK(lines == THREADS * MESSAGES_PER_THREAD);
}

void test_shutdown_flushes_queue()
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_DEBUG);

    const int THREADS = 4;
    const int MESSAGES_PER_THREAD = 500;
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back([t]() {
            for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
            {
                LOGGER_DEBUG("flush-test thread {} message {}", t, i);
            }
        });
    }
    for (auto &th : threads)
        th.join();

    // Immediately shutdown. No sleep. The logger's destructor must block
    // until the worker thread has processed all queued messages.
    L.shutdown();

    size_t lines = count_lines(g_log_path.string());
    CHECK(lines == THREADS * MESSAGES_PER_THREAD);
}

void test_multiprocess_logging(const std::string &self_exe)
{
    std::remove(g_log_path.string().c_str());
    Logger &L = Logger::instance();
    CHECK(L.init_file(g_log_path.string(), true)); // Use flock for multi-process
    L.set_level(Logger::Level::L_INFO);

    LOGGER_INFO("parent-process-start");

    const int CHILDREN = 3;
    const int MESSAGES_PER_CHILD = 20;

#if defined(PLATFORM_WIN64)
    std::vector<PROCESS_INFORMATION> procs;
    for (int i = 0; i < CHILDREN; ++i)
    {
        PROCESS_INFORMATION pi = spawn_child_windows(self_exe, g_log_path.string());
        if (pi.hProcess == nullptr)
        {
            fmt::print(stderr, "spawn_child_windows failed\n");
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
        pid_t pid = spawn_child_posix(self_exe, g_log_path.string());
        if (pid < 0)
        {
            fmt::print(stderr, "fork/spawn failed\n");
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

    L.shutdown();

    size_t lines = count_lines(g_log_path.string());
    size_t expected_lines = 1 /* parent start */ + (CHILDREN * (MESSAGES_PER_CHILD + 1 /* utf8 */));
    CHECK(lines == expected_lines);
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

    // Re-initialize the logger. On POSIX, open(O_WRONLY) on a read-only file
    // can succeed, with the subsequent write() failing. This is what we test.
    CHECK(L.init_file(g_log_path.string(), false));
    L.set_level(Logger::Level::L_INFO);

    std::atomic<bool> callback_invoked = false;
    std::atomic<int> error_code = 0;
    int initial_failures = L.write_failure_count();

    L.set_write_error_callback([&](const std::string &msg) {
        callback_invoked = true;
        error_code = L.last_write_error_code();
    });

    LOGGER_INFO("This write should fail.");

    L.shutdown(); // This will flush, ensuring the write is attempted.

    CHECK(callback_invoked.load());
    CHECK(L.write_failure_count() > initial_failures);
    CHECK(error_code.load() == EACCES); // EACCES is "Permission denied"

    // Cleanup: make writable again so we can remove it
    chmod(g_log_path.string().c_str(), S_IRWXU | S_IWGRP | S_IWOTH);
}
#endif

int main(int argc, char **argv)
{
    // Child process entry point
    if (argc > 1 && std::string(argv[1]) == "--child")
    {
        if (argc < 3)
        {
            fmt::print(stderr, "Child mode requires log path argument.\n");
            return 1;
        }
        return run_as_child_main(argv[2]);
    }

    // Parent process test runner
    g_log_path = fs::temp_directory_path() / "pylabhub_test_logger.log";
    fmt::print("--- Logger Test Suite ---\n");
    fmt::print("Using temporary log file: {}\n", g_log_path.string());

    TEST_CASE("Basic Logging and File Sink", test_basic_logging);
    TEST_CASE("Log Level Filtering", test_log_level_filtering);
    TEST_CASE("Message Truncation", test_message_truncation);
    TEST_CASE("Bad Format String Handling", test_bad_format_string);
    TEST_CASE("Multi-threaded Logging", test_multithreaded_logging);
    TEST_CASE("Shutdown Flushes Queue Correctly", test_shutdown_flushes_queue);
#if PYLABHUB_IS_POSIX
    TEST_CASE("Write Error Callback (POSIX-only)", test_write_error_callback);
#endif
    TEST_CASE("Multi-process Logging", [&]() { test_multiprocess_logging(argv[0]); });

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    // Final cleanup
    std::error_code ec;
    fs::remove(g_log_path, ec);

    return tests_failed == 0 ? 0 : 1;
}
