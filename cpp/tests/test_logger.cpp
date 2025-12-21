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

#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include "platform.hpp" // Include for PLATFORM_WIN64 macro

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>      // For EACCES
#include <fcntl.h>     // For open()
#include <signal.h>    // For kill()
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
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            tests_failed++;                                                                        \
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{}\n", #condition, __FILE__, __LINE__);   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define FAIL_TEST(msg)                                                                             \
    do                                                                                             \
    {                                                                                              \
        tests_failed++;                                                                            \
        fmt::print(stderr, "  TEST FAILED: {} at {}:{}\n", msg, __FILE__, __LINE__);               \
        exit(1);                                                                                   \
    } while (0)

// --- Test Globals & Helpers ---
static fs::path g_log_path; // Only used for multi-process tests now
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

static bool wait_for_string_in_file(const fs::path &path, const std::string &expected,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(15))
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

static fs::path get_unique_log_path(const std::string &test_name)
{
    // Generate a unique filename for a test's log file.
    return fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
}

// --- Test Cases ---

//
// [Purpose]
// Verifies the most fundamental logging operations:
// 1. Asynchronously switching to a file sink.
// 2. Logging messages at different levels (INFO, DEBUG).
// 3. Correctly formatting messages with arguments.
// 4. Handling of non-ASCII (Unicode) characters in log messages.
// 5. Ensuring that `flush()` correctly waits for all queued messages to be written.
//
// [Method]
// - Sets the log sink to a file and the level to TRACE.
// - Logs several messages with different levels, arguments, and character sets.
// - Calls `flush()` to block until the log queue is empty.
// - Reads the log file and verifies that the exact number of expected lines have been
//   written and that their content is correct.
void test_basic_logging()
{
    fs::path log_path = get_unique_log_path("basic_logging");
    fs::remove(log_path);

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_TRACE);

    CHECK(wait_for_string_in_file(log_path, "Switched log to file"));
    std::string contents_before;
    CHECK(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    LOGGER_INFO("unit-test: ascii message {}", 42);
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    L.flush();

    std::string contents_after;
    CHECK(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    CHECK(lines_after - lines_before == 3);

    CHECK(contents_after.find("unit-test: ascii message 42") != std::string::npos);
    CHECK(contents_after.find("unit-test: debug 3.14") != std::string::npos);
    CHECK(contents_after.find("☃") != std::string::npos);
    CHECK(contents_after.find("日本語") != std::string::npos);

    fs::remove(log_path);
}

//
// [Purpose]
// Tests the logger's ability to filter messages based on the currently set log level.
// This is a core feature to control logging verbosity at runtime.
//
// [Method]
// - Sets the log level to WARNING.
// - Attempts to log INFO and DEBUG messages, which should be discarded.
// - Logs a WARNING message, which should be written.
// - Changes the log level back to TRACE.
// - Logs a DEBUG message, which should now be written.
// - Flushes and inspects the log file to confirm that only the messages that met
//   the level requirement at the time of logging were written.
void test_log_level_filtering()
{
    fs::path log_path = get_unique_log_path("log_level_filtering");
    fs::remove(log_path);

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    CHECK(wait_for_string_in_file(log_path, "Switched log to file"));

    std::string contents_before;
    CHECK(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    L.set_level(Logger::Level::L_WARNING);
    LOGGER_INFO("This should NOT be logged.");
    LOGGER_DEBUG("This should also NOT be logged.");
    LOGGER_WARN("This WARNING should be logged.");
    L.set_level(Logger::Level::L_TRACE);
    LOGGER_DEBUG("This DEBUG should now be logged.");

    L.flush();

    std::string contents_after;
    CHECK(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    CHECK(lines_after - lines_before == 2);

    CHECK(contents_after.find("This should NOT be logged.") == std::string::npos);
    CHECK(contents_after.find("This WARNING should be logged.") != std::string::npos);
    CHECK(contents_after.find("This DEBUG should now be logged.") != std::string::npos);

    fs::remove(log_path);
}

//
// [Purpose]
// Ensures the logger is robust against malformed format strings. A logging utility
// should not crash the application if given a bad format string (e.g., more format
// specifiers than arguments).
//
// [Method]
// - Uses the runtime (`_RT`) version of the logging macro, which is necessary for
//   passing a variable as the format string.
// - Provides a format string with a specifier `{}` but no corresponding argument.
// - The logger's internal error handling should catch the `fmt::format_error`
//   exception and instead log a special "[FORMAT ERROR]" message.
// - The test verifies that this special error message appears in the log file.
void test_bad_format_string()
{
    fs::path log_path = get_unique_log_path("bad_format_string");
    fs::remove(log_path);

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_INFO);
    CHECK(wait_for_string_in_file(log_path, "Switched log to file"));
    std::string bad_fmt = "Missing arg: {}";
    LOGGER_INFO_RT(bad_fmt); // This log call will internally create a "[FORMAT ERROR]" message
    CHECK(wait_for_string_in_file(log_path, "[FORMAT ERROR]"));

    fs::remove(log_path);
}

//
// [Purpose]
// Validates the logger's default behavior and its ability to switch between sinks.
// By default, the logger should write to the console. This test ensures that behavior
// and verifies a clean switch to a file sink.
//
// [Method]
// - Logs a message without explicitly setting a sink. This should go to the default
//   console sink (stderr). This part of the test relies on visual inspection of test
//   output, as capturing stderr is complex.
// - Switches the sink to a file.
// - Logs a second message.
// - Verifies that the log file contains the second message but *not* the first one,
//   confirming the switch was successful.
void test_default_sink_and_switching()
{
    fs::path log_path = get_unique_log_path("default_sink_and_switching");
    fs::remove(log_path);

    Logger &L = Logger::instance();
    L.set_level(Logger::Level::L_INFO);
    LOGGER_INFO("This message should go to the default console sink (stderr).");
    L.flush();

    L.set_logfile(log_path.string());
    LOGGER_INFO("This message should be logged to the file.");
    CHECK(wait_for_string_in_file(log_path, "This message should be logged to the file."));

    std::string contents;
    CHECK(read_file_contents(log_path.string(), contents));
    CHECK(contents.find("This message should go to the default console sink") == std::string::npos);
    CHECK(contents.find("Switched log to file") != std::string::npos);

    fs::remove(log_path);
}

//
// [Purpose]
// This is a stress test to verify the logger's correctness and stability in a
// highly concurrent environment, which is a primary design goal. It tests for
// race conditions, deadlocks, and message corruption.
//
// [Method]
// - Spawns a large number of "logging threads" that continuously write messages.
// - Simultaneously, a "sink switching thread" frantically and repeatedly switches
//   the log destination between the console and a file.
// - This creates a chaotic scenario where logging and configuration changes are
//   interleaved from many threads.
// - After joining all threads, the test verifies that:
//   1. At least one message from each logging thread made it to the final log file,
//      proving no thread was completely starved.
//   2. "Switched to..." messages for both console and file sinks are present,
//      proving the sink switching commands were executed.
void test_multithread_stress()
{
    fs::path log_path = get_unique_log_path("multithread_stress");
    fs::remove(log_path);

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_DEBUG);
    CHECK(wait_for_string_in_file(log_path, "Switched log to file"));

    const int LOG_THREADS = 32;
    const int MESSAGES_PER_THREAD = 1000;
    const int SINK_SWITCHES = 100;

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
                    Logger::instance().set_logfile(log_path.string());
                else
                    Logger::instance().set_console();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            Logger::instance().set_logfile(log_path.string());
        });

    for (auto &t : threads)
    {
        t.join();
    }
    L.flush();

    std::string contents;
    CHECK(read_file_contents(log_path.string(), contents));

    size_t found_threads = 0;
    for (int t = 0; t < LOG_THREADS; ++t)
    {
        if (contents.find(fmt::format("thread {} message", t)) != std::string::npos)
        {
            found_threads++;
        }
    }
    CHECK(found_threads == LOG_THREADS);
    CHECK(contents.find("Switched log to Console") != std::string::npos);
    CHECK(contents.find("Switched log to file") != std::string::npos);

    fs::remove(log_path);
}

//
// [Purpose]
// Verifies that `flush()` is a blocking operation that correctly waits for the
// asynchronous worker thread to process all messages currently in its queue. This
// is critical for ensuring log data is persisted before a program section exits.
//
// [Method]
// - Enqueues a large number of log messages in a tight loop without any delay.
//   This ensures the command queue is heavily populated.
// - Immediately calls `flush()`.
// - After `flush()` returns, the test reads the log file and checks if the number
//   of lines written matches the number of messages logged. If `flush()` were
//   non-blocking or incorrect, the file check would likely run before all messages
//   were written, causing the line count to be off.
void test_flush_waits_for_queue()
{
    fs::path log_path = get_unique_log_path("flush_waits_for_queue");
    fs::remove(log_path);

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_TRACE);
    CHECK(wait_for_string_in_file(log_path, "Switched log to file"));
    std::string contents_before;
    CHECK(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    const int MESSAGES = 500;
    for (int i = 0; i < MESSAGES; ++i)
    {
        LOGGER_INFO("flush-test: msg={}", i);
    }

    L.flush();

    std::string contents_after;
    CHECK(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    CHECK(lines_after - lines_before == MESSAGES);

    fs::remove(log_path);
}

//
// [Purpose]
// Tests the idempotency of the `shutdown()` method and verifies that logging is
// disabled after shutdown. It should be safe to call `shutdown()` multiple times
// from multiple threads. The logger should shut down cleanly once and ignore
// subsequent calls and log messages without crashing.
//
// [Method]
// - Spawns multiple threads that all call `Logger::instance().shutdown()` concurrently
//   to test for race conditions in the shutdown logic.
// - Verifies that logging to a file works before shutdown.
// - After the shutdown calls, it attempts to log another message.
// - It then verifies that the new message was *not* written to the file, confirming
//   that the logger becomes inactive after shutdown.
// - The test passes if no crash occurs and no messages are logged post-shutdown.
void test_shutdown_idempotency()
{
    fs::path log_path = get_unique_log_path("shutdown_idempotency");
    fs::remove(log_path);

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_INFO);

    LOGGER_INFO("Message before shutdown.");
    L.flush();

    std::string content_before_shutdown;
    CHECK(read_file_contents(log_path.string(), content_before_shutdown));
    CHECK(content_before_shutdown.find("Message before shutdown") != std::string::npos);

    const int THREADS = 16;
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([]() { Logger::instance().shutdown(); });
    }
    for (auto &t : threads)
    {
        t.join();
    }

    // This message should be silently dropped.
    LOGGER_INFO("This message should NOT be logged.");

    // This flush should be a no-op on a shutdown logger. Give some time in
    // case the message queue were to be processed erroneously.
    L.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string content_after_shutdown;
    CHECK(read_file_contents(log_path.string(), content_after_shutdown));

    // Verify the new message was not written.
    CHECK(content_after_shutdown.find("This message should NOT be logged.") == std::string::npos);
    // Verify the content is exactly as it was before the post-shutdown log call.
    CHECK(content_before_shutdown == content_after_shutdown);

    fs::remove(log_path);
}

//
// [Purpose]
// Verifies a critical deadlock-avoidance mechanism: the ability to safely log
// from within a write error callback. If a sink fails, the logger invokes a
// user-provided callback. If that callback itself tries to log a message, a naive
// implementation could easily deadlock.
//
// [Method]
// 1. Induce a sink error: The test makes a log file path unwritable. On Windows,
//    this is done by opening the file with an exclusive lock (`dwShareMode = 0`).
//    On POSIX, it's done by creating a directory with no write permissions.
// 2. Set an error callback that, upon invocation, calls `LOGGER_SYSTEM`. This is
//    the "re-entrant" log call.
// 3. Attempt to switch the logger's sink to the unwritable path. This asynchronous
//    command will fail in the worker thread.
// 4. The worker thread, upon failure, will invoke the error callback.
// 5. The re-entrant log message from the callback should be successfully sent to
//    the *previous*, valid sink.
// 6. The test verifies that this re-entrant message appears in the original log file.
void test_reentrant_error_callback()
{
    fs::path initial_log_path = get_unique_log_path("reentrant_initial");
    fs::remove(initial_log_path);

    Logger &L = Logger::instance();
    L.set_logfile(initial_log_path.string());
    CHECK(wait_for_string_in_file(initial_log_path, "Switched log to file"));

    std::atomic<bool> callback_invoked = false;
    L.set_write_error_callback(
        [&](const std::string &msg)
        {
            // This log call is the key part of the test. It ensures that calling the
            // logger from within an error callback doesn't deadlock.
            LOGGER_SYSTEM("Re-entrant log from error callback: {}", msg);
            callback_invoked = true;
        });

    // To test the error callback, we need to induce a sink creation failure.
    // The method differs by platform.
#if defined(PLATFORM_WIN64)
    // On Windows, setting a directory to read-only does not prevent file creation
    // within it. The correct but complex way involves modifying directory ACLs,
    // which is too fragile for a unit test. A robust alternative is to lock the
    // target log file path exclusively, causing the logger's CreateFileW call to fail.
    fs::path locked_log_path = fs::temp_directory_path() / "pylab_reentrant_locked.log";
    std::remove(locked_log_path.string().c_str());
    HANDLE h = CreateFileA(locked_log_path.string().c_str(), GENERIC_READ, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
#else
    // On POSIX, we can remove write permissions from a directory. Any attempt to
    // create a file inside it will then fail.
    fs::path readonly_dir = fs::temp_directory_path() / "pylab_readonly_dir_test_reentrant";
    fs::remove_all(readonly_dir);
    fs::create_directory(readonly_dir);
    chmod(readonly_dir.string().c_str(), S_IRUSR | S_IXUSR); // mode 0500
    fs::path locked_log_path = readonly_dir / "test.log";
#endif

    L.set_logfile(locked_log_path.string());
    LOGGER_INFO("This message will be dropped and should trigger an error.");
    L.flush();

    // The re-entrant log should have been written to the *previous* sink.
    CHECK(wait_for_string_in_file(initial_log_path, "Re-entrant log from error callback"));
    CHECK(callback_invoked.load());

    // Cleanup
#if defined(PLATFORM_WIN64)
    CloseHandle(h);
    fs::remove(locked_log_path);
#else
    chmod(readonly_dir.string().c_str(), S_IRWXU); // allow deletion
    fs::remove_all(readonly_dir);
#endif
    fs::remove(initial_log_path);
}

//
// [Purpose]
// Tests the asynchronous write error callback mechanism. When a sink fails to be
// created (e.g., due to file permissions), the logger should detect this and
// trigger the registered error callback.
//
// [Method]
// 1. A log file path is made unwritable using platform-specific methods (exclusive
//    file lock on Windows, read-only permissions on POSIX).
// 2. An error callback is registered, which sets a flag (`callback_invoked`) and
//    checks that the received error message is as expected.
// 3. The logger is instructed to switch to the unwritable log file.
// 4. A message is logged. This message should be dropped because the sink could
//    not be created.
// 5. The test waits briefly and then checks if the callback was invoked, confirming
//    that the asynchronous sink creation failure was correctly handled.
void test_write_error_callback_async()
{
#if defined(PLATFORM_WIN64)
    // Use a unique path for this test to avoid contention with other tests.
    fs::path test_log_path = fs::temp_directory_path() / "pylab_write_err_async.log";
    fs::remove(test_log_path);

    HANDLE h = CreateFileA(test_log_path.string().c_str(), GENERIC_READ, 0, nullptr, CREATE_ALWAYS,
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

    L.set_logfile(test_log_path.string());
    LOGGER_INFO("This write should be dropped as sink creation failed.");
    L.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(callback_invoked.load());
    CloseHandle(h);
    fs::remove(test_log_path);
#else
    // On POSIX, we test two distinct failure modes.

    // Test 1: Failure to create a file in a read-only directory.
    {
        fmt::print("  - Testing sink failure: read-only directory (POSIX)\n");
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
    }

    // Test 2: Failure to open an existing read-only file for writing.
    {
        fmt::print("  - Testing sink failure: read-only file (POSIX)\n");
        fs::path readonly_file = fs::temp_directory_path() / "pylab_readonly_file.log";
        // Create the file, then make it read-only
        {
            std::ofstream touch(readonly_file);
        }
        chmod(readonly_file.string().c_str(), S_IRUSR); // 0400

        Logger &L = Logger::instance();
        std::atomic<bool> callback_invoked = false;
        L.set_write_error_callback(
            [&](const std::string &msg)
            {
                callback_invoked = true;
                CHECK(msg.find("Failed to open log file") != std::string::npos);
            });

        L.set_logfile(readonly_file.string());
        LOGGER_INFO("This message will also be dropped.");
        L.flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CHECK(callback_invoked.load());

        chmod(readonly_file.string().c_str(), S_IRWXU);
        fs::remove(readonly_file);
    }
#endif
}

//
// [Purpose]
// Verifies that the logger can switch to platform-specific logging backends,
// namely the Windows Event Log and POSIX syslog.
//
// [Method]
// - On Windows, it calls `set_eventlog()` and logs a message.
// - On POSIX, it calls `set_syslog()` and logs a message.
// - Since programmatically reading from these system-wide logs is complex and
//   often requires elevated permissions, this test requires manual verification.
//   It prints instructions to the console for the user to follow.
void test_platform_sinks()
{
    Logger &L = Logger::instance();
#if defined(_WIN32)
    L.set_eventlog(L"PyLabHubTestLogger");
    LOGGER_INFO("Testing Windows Event Log sink.");
    L.flush();
    fmt::print(stderr, "\n  MANUAL VERIFICATION REQUIRED for Windows Event Log:\n");
    fmt::print(stderr, "  1. Open Event Viewer (eventvwr.msc).\n");
    fmt::print(stderr, "  2. Navigate to 'Windows Logs' -> 'Application'.\n");
    fmt::print(stderr, "  3. Look for an Information-level message from source "
                       "'PyLabHubTestLogger' containing 'Testing Windows Event Log sink.'.\n\n");
#else
    L.set_syslog("pylab-logger-test");
    LOGGER_INFO("Testing syslog sink.");
    L.flush();
    fmt::print(stderr, "\n  MANUAL VERIFICATION REQUIRED for Syslog:\n");
    fmt::print(stderr, "  1. Open your system's terminal.\n");
    fmt::print(stderr, "  2. Run 'journalctl -r | grep \"pylab-logger-test\"' or "
                       "'cat /var/log/syslog | grep \"pylab-logger-test\"'.\n");
    fmt::print(stderr, "  3. Look for a message containing 'Testing syslog sink.'.\n\n");
#endif
    CHECK(true);
}

void multiproc_child_main(int msg_count)
{
    Logger &L = Logger::instance();
    L.set_logfile(g_log_path.string(), true);
    L.set_level(Logger::Level::L_TRACE);

    // Seed per-process
#if defined(PLATFORM_WIN64)
    std::srand(static_cast<unsigned int>(GetCurrentProcessId() +
                                          std::chrono::system_clock::now().time_since_epoch().count()));
#else
    std::srand(static_cast<unsigned int>(getpid() +
                                          std::chrono::system_clock::now().time_since_epoch().count()));
#endif

    for (int i = 0; i < msg_count; ++i)
    {
        if (std::rand() % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
        }
#if defined(_WIN32)
        LOGGER_INFO("child-msg pid={} idx={}", GetCurrentProcessId(), i);
#else
        LOGGER_INFO("child-msg pid={} idx={}", getpid(), i);
#endif
    }
    L.flush();
}

bool run_multiproc_iteration(const std::string& self_exe, int num_children, int msgs_per_child)
{
    // On Windows, the logger from the parent process will hold a handle to the log file,
    // preventing its deletion between iterations. Switching to a console sink forces
    // the old file sink to be destroyed, releasing the handle.
    Logger::instance().set_console();
    Logger::instance().flush();

    g_log_path = fs::temp_directory_path() / "pylabhub_multiprocess_test.log";
    std::remove(g_log_path.string().c_str());

    Logger &L = Logger::instance();
    L.set_console(); 
    L.set_logfile(g_log_path.string(), true);
    L.set_level(Logger::Level::L_INFO);
    
    LOGGER_INFO("parent-process-start");
    L.flush();

#if defined(_WIN32)
    std::vector<HANDLE> procs;
    for (int i = 0; i < num_children; ++i)
    {
        std::string cmdline_str =
            fmt::format("\"{}\" --multiproc-child \"{}\" {}", self_exe, g_log_path.string(), msgs_per_child);
        std::vector<char> cmdline(cmdline_str.begin(), cmdline_str.end());
        cmdline.push_back('\0');

        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &pi))
        {
            fmt::print(stderr, "CreateProcessA failed for child {}\n", i);
            return false;
        }
        CloseHandle(pi.hThread);
        procs.push_back(pi.hProcess);
    }
    for (auto &h : procs)
    {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
#else
    std::vector<pid_t> child_pids;
    for (int i = 0; i < num_children; ++i)
    {
        pid_t pid = fork();
        if (pid == -1) return false;
        if (pid == 0)
        {
            execl(self_exe.c_str(), self_exe.c_str(), "--multiproc-child",
                  g_log_path.string().c_str(), std::to_string(msgs_per_child).c_str(), (char *)nullptr);
            _exit(127);
        }
        child_pids.push_back(pid);
    }
    for (pid_t pid : child_pids)
    {
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    }
#endif
    L.flush();

    std::string contents;
    if (!read_file_contents(g_log_path.string(), contents)) return false;
    
    size_t found = 0;
    std::stringstream ss(contents);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.find("child-msg") != std::string::npos)
        {
            found++;
        }
    }
    
    size_t expected = static_cast<size_t>(num_children) * msgs_per_child;
    fmt::print("  [Stress: {} procs * {} msgs] Found: {} / Expected: {}\n", 
               num_children, msgs_per_child, found, expected);
               
    return found == expected;
}

//
// [Purpose]
// A high-stress test for multiprocess logging. This ensures that when multiple
// independent processes all write to the same log file concurrently, messages are
// not lost or corrupted. It's a critical test for server environments or applications
// that spawn child processes.
//
// [Method]
// - The test runs in iterations, ramping up the number of child processes (from 10
//   to 50).
// - In each iteration, it spawns `N` child processes.
// - Each child process gets its own instance of the logger and logs a large number
//   of messages to a shared log file before exiting.
// - The parent process waits for all children to complete.
// - It then reads the shared log file and counts the number of lines, verifying that
//   the total number of messages received equals the total number sent (`N` * msgs_per_child).
// - The use of `use_flock=true` (on POSIX) or the kernel-level file locking (on Windows)
//   is implicitly tested here to prevent log entries from being garbled.
void test_multiprocess_logging()
{
    // High-stress ramp-up
    const int start_children = 10;
    const int max_children = 50;
    const int step_children = 10;
    const int msgs = 1000;

    fmt::print("Starting high-stress multiprocess ramp-up (msgs/child={})...\n", msgs);

    for (int n = start_children; n <= max_children; n += step_children)
    {
        if (!run_multiproc_iteration(g_self_exe_path, n, msgs))
        {
            FAIL_TEST(fmt::format("Multiprocess logging FAILED at {} children.", n));
        }
    }
    fmt::print("Multiprocess high-stress test PASSED up to {} children.\n", max_children);
}

//
// [Purpose]
// Tests the logger's stability under extreme "chaos" conditions. This test simulates
// a worst-case scenario where multiple threads are not only logging but also
// concurrently and repeatedly changing the logger's configuration and lifecycle state.
// Its goal is to surface rare race conditions or deadlocks.
//
// [Method]
// - A variety of worker threads are spawned:
//   - "Logging threads" that constantly send messages.
//   - "Flushing threads" that constantly call `flush()`.
//   - "Config threads" that constantly switch sinks and reset callbacks.
// - These threads all run concurrently for a fixed duration.
// - At the end, `Logger::instance().shutdown()` is called by the main thread while
//   the other threads are still running.
// - The test passes if it completes without crashing, deadlocking, or throwing an
//   exception, demonstrating the robustness of the logger's internal state management.
void test_concurrent_lifecycle_chaos()
{
    fs::path chaos_log_path = get_unique_log_path("lifecycle_chaos");
    fs::remove(chaos_log_path);

    std::atomic<bool> stop_flag = false;
    const int DURATION_MS = 2000;

    auto worker = [&](int id, auto fn)
    {
        while (!stop_flag.load(std::memory_order_relaxed))
        {
            fn(id);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
    {
        threads.emplace_back(worker, i,
                             [](int id)
                             {
                                 LOGGER_INFO("chaos-log-{}: message", id);
                                 std::this_thread::sleep_for(std::chrono::microseconds(500));
                             });
    }
    for (int i = 0; i < 2; ++i)
    {
        threads.emplace_back(worker, i,
                             [](int)
                             {
                                 Logger::instance().flush();
                                 std::this_thread::sleep_for(std::chrono::milliseconds(10));
                             });
    }
    for (int i = 0; i < 2; ++i)
    {
        threads.emplace_back(worker, i,
                             [&](int id)
                             {
                                 if (id % 2 == 0)
                                 {
                                     Logger::instance().set_console();
                                 }
                                 else
                                 {
                                     Logger::instance().set_logfile(chaos_log_path.string());
                                 }
                                 Logger::instance().set_write_error_callback(nullptr);
                                 std::this_thread::sleep_for(std::chrono::milliseconds(15));
                             });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    Logger::instance().shutdown();
    stop_flag.store(true);

    for (auto &t : threads)
    {
        t.join();
    }
    CHECK(true);
    fs::remove(chaos_log_path);
}

void run_test_in_process(const std::string &test_name)
{
    fmt::print("\n--- Running test: {} ---\n", test_name);
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
        fmt::print(stderr, "  --- FAILED: CreateProcessA failed ({}) ---\n", GetLastError());
        return;
    }
    WaitForSingleObject(pi.hProcess, 60000); 
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
        fmt::print(stderr, "  --- FAILED in child process ---\n");
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
              (char *)nullptr);
        _exit(127);
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

namespace
{
struct TestLifecycleManager
{
    TestLifecycleManager() { pylabhub::utils::Initialize(); }
    ~TestLifecycleManager() { pylabhub::utils::Finalize(); }
};
} // namespace

int main(int argc, char **argv)
{
    TestLifecycleManager lifecycle_manager;
    g_self_exe_path = argv[0];
    // Set a default global path, now only used by the multiprocess test.
    g_log_path = fs::temp_directory_path() / "pylabhub_test_logger.log";

    if (argc > 1 && std::string(argv[1]) == "--multiproc-child")
    {
        if (argc < 3)
            return 3;
        g_log_path = argv[2];
        int count = 200;
        if (argc >= 4) count = std::stoi(argv[3]);
        multiproc_child_main(count);
        return 0;
    }

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
        else if (test_name == "test_shutdown_idempotency")
            test_shutdown_idempotency();
        else if (test_name == "test_reentrant_error_callback")
            test_reentrant_error_callback();
        else if (test_name == "test_concurrent_lifecycle_chaos")
            test_concurrent_lifecycle_chaos();
        else if (test_name == "test_write_error_callback_async")
            test_write_error_callback_async();
        else if (test_name == "test_platform_sinks")
            test_platform_sinks();
        else if (test_name == "test_multiprocess_logging")
            test_multiprocess_logging();
        else
        {
            fmt::print(stderr, "Unknown test name: {}\n", test_name);
            return 1;
        }
        return 0;
    }

    fmt::print("--- Logger Test Suite (Process-Isolated) ---\n");
    const std::vector<std::string> test_names = {"test_basic_logging",
                                                 "test_log_level_filtering",
                                                 "test_bad_format_string",
                                                 "test_default_sink_and_switching",
                                                 "test_multithread_stress",
                                                 "test_flush_waits_for_queue",
                                                 "test_shutdown_idempotency",
                                                 "test_reentrant_error_callback",
                                                 "test_concurrent_lifecycle_chaos",
                                                 "test_write_error_callback_async",
                                                 "test_platform_sinks",
                                                 "test_multiprocess_logging"};

    for (const auto &name : test_names)
    {
        run_test_in_process(name);
    }

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    return tests_failed == 0 ? 0 : 1;
}

