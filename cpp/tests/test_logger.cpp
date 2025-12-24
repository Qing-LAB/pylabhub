// tests/test_logger.cpp
//
// GoogleTest tests for the single-process and multi-threaded behavior of the Logger.

#include <gtest/gtest.h>
#include <atomic>
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
#include "platform.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Helper utility to read the entire content of a file into a string.
static bool read_file_contents(const std::string &path, std::string &out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

// Helper utility to count newline characters in a string.
static size_t count_lines(const std::string &s)
{
    size_t count = 0;
    for (char c : s)
        if (c == '\n') ++count;
    return count;
}

// Helper utility to poll a file until a specific string appears or a timeout is reached.
// This is crucial for testing the asynchronous logger, as it provides a synchronization point.
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
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Helper to scale down test intensity for CI environments.
static std::string test_scale()
{
    const char *v = std::getenv("PYLAB_TEST_SCALE");
    return v ? std::string(v) : std::string();
}

static int scaled_value(int original, int small_value)
{
    if (test_scale() == "small") return small_value;
    return original;
}

} // namespace

// --- Test Fixture ---
// Creates a common setup for all logger tests, ensuring the lifecycle is
// initialized and that any created log files are cleaned up on teardown.
class LoggerTest : public ::testing::Test {
protected:
    std::vector<fs::path> paths_to_clean_;

    void SetUp() override {
        pylabhub::utils::Initialize();
    }

    void TearDown() override {
        // Ensure logger is using console and file handles released before cleanup.
        Logger::instance().set_console();
        Logger::instance().flush();
        
        // Finalize the subsystems for the test that just ran.
        pylabhub::utils::Finalize();

        // Reset the entire lifecycle/logger system for the *next* test.
        pylabhub::utils::ResetForTesting();

        for (const auto& p : paths_to_clean_) {
            try {
                if (fs::exists(p)) fs::remove(p);
            } catch (const fs::filesystem_error& e) {
                fmt::print(stderr, "Warning: failed to clean up '{}': {}\n", p.string(), e.what());
            }
        }
    }

    fs::path GetUniqueLogPath(const std::string& test_name) {
        auto p = fs::temp_directory_path() / ("pylabhub_test_" + test_name + ".log");
        paths_to_clean_.push_back(p);
        try { if (fs::exists(p)) fs::remove(p); } catch (...) {}
        return p;
    }
};

// What: Tests the fundamental ability of the logger to write formatted messages
//       to a file, including ASCII and UTF-8 characters.
// How: It configures the logger to use a file sink, logs several messages of
//      varying levels and content, flushes the logger, and then reads the file
//      to verify that the expected messages were written correctly.
TEST_F(LoggerTest, BasicLogging)
{
    auto log_path = GetUniqueLogPath("basic_logging");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_TRACE);

    ASSERT_TRUE(wait_for_string_in_file(log_path, "Log sink switched from: Console"));
    std::string contents_before;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    LOGGER_INFO("unit-test: ascii message {}", 42);
    LOGGER_DEBUG("unit-test: debug {:.2f}", 3.14159);
    LOGGER_INFO("unit-test: utf8 test {} {}", "☃", "日本語");

    L.flush();

    std::string contents_after;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    EXPECT_EQ(lines_after - lines_before, 3u);

    EXPECT_NE(contents_after.find("unit-test: ascii message 42"), std::string::npos);
    EXPECT_NE(contents_after.find("unit-test: debug 3.14"), std::string::npos);
    EXPECT_NE(contents_after.find("☃"), std::string::npos);
    EXPECT_NE(contents_after.find("日本語"), std::string::npos);
}

// What: Verifies that the logger correctly filters messages based on the current log level.
// How: The logger's level is set to L_WARNING. INFO and DEBUG messages are logged,
//      followed by a WARN message. The level is then lowered to L_TRACE and another
//      DEBUG message is logged. The test verifies that only the messages that meet
//      the level threshold at the time of logging are present in the final file.
TEST_F(LoggerTest, LogLevelFiltering)
{
    auto log_path = GetUniqueLogPath("log_level_filtering");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Log sink switched from: Console"));

    std::string contents_before;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    L.set_level(Logger::Level::L_WARNING);
    LOGGER_INFO("This should NOT be logged.");
    LOGGER_DEBUG("This should also NOT be logged.");
    LOGGER_WARN("This WARNING should be logged.");
    L.set_level(Logger::Level::L_TRACE);
    LOGGER_DEBUG("This DEBUG should now be logged.");

    L.flush();

    std::string contents_after;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    EXPECT_EQ(lines_after - lines_before, 2u);

    EXPECT_EQ(contents_after.find("This should NOT be logged."), std::string::npos);
    EXPECT_NE(contents_after.find("This WARNING should be logged."), std::string::npos);
    EXPECT_NE(contents_after.find("This DEBUG should now be logged."), std::string::npos);
}

// What: Checks the logger's robustness when given a malformed format string at runtime.
// How: It passes a {fmt} format string with a placeholder but no corresponding
//      argument to the `LOGGER_INFO_RT` macro. The test verifies that this does not
//      crash and that the logger correctly logs an internal format error message.
TEST_F(LoggerTest, BadFormatString)
{
    auto log_path = GetUniqueLogPath("bad_format_string");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_INFO);
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Log sink switched from: Console"));

    std::string bad_fmt = "Missing arg: {}";
    LOGGER_INFO_RT(bad_fmt); // This should not throw but should log an error.
    ASSERT_TRUE(wait_for_string_in_file(log_path, "[FORMAT ERROR]"));
}

// What: Tests the logger's ability to switch between the default console sink and a file sink.
// How: It logs a message, which should go to the default (console) sink. It then
//      sets a file sink and logs another message. It verifies that the first message
//      is NOT in the file, but the second one is.
TEST_F(LoggerTest, DefaultSinkAndSwitching)
{
    auto log_path = GetUniqueLogPath("default_sink_and_switching");

    Logger &L = Logger::instance();
    L.set_level(Logger::Level::L_INFO);

    // This message should not appear in the final log file.
    LOGGER_INFO("This message should go to the default console sink (stderr).");
    L.flush();

    L.set_logfile(log_path.string());
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Log sink switched from: Console"));
    LOGGER_INFO("This message should be logged to the file.");
    ASSERT_TRUE(wait_for_string_in_file(log_path, "This message should be logged to the file."));

    std::string contents;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents));
    EXPECT_EQ(contents.find("This message should go to the default console sink"), std::string::npos);
    EXPECT_NE(contents.find("Log sink switched from: Console"), std::string::npos);
}

// What: A chaos test to verify the logger's thread-safety under heavy load.
// How: It spawns a large number of "logging threads" that continuously write
//      messages. Simultaneously, a "sink switching thread" continuously and
//      rapidly switches the logger's output between a file and the console.
//      The test verifies that no crash occurs and that messages from all threads
//      and sink-switch notifications appear in the final log file.
TEST_F(LoggerTest, MultithreadStress)
{
    auto log_path = GetUniqueLogPath("multithread_stress");
    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_DEBUG);
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Log sink switched from: Console"));

    const int LOG_THREADS = scaled_value(32, 8);
    const int MESSAGES_PER_THREAD = scaled_value(1000, 100);
    const int SINK_SWITCHES = scaled_value(100, 10);

    std::vector<std::thread> threads;
    threads.reserve(LOG_THREADS + 1);

    // Create logging threads
    for (int t = 0; t < LOG_THREADS; ++t)
    {
        threads.emplace_back([t, MESSAGES_PER_THREAD]() {
            for (int i = 0; i < MESSAGES_PER_THREAD; ++i)
            {
                LOGGER_DEBUG("thread {} message {}", t, i);
                if (i % 100 == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Create a thread to chaotically switch sinks
    threads.emplace_back([&]() {
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

    for (auto &t : threads) t.join();
    // Ensure the logger is definitively set to the log file before flushing and verifying.
    L.set_logfile(log_path.string());
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Log sink switched from: Console"));

    L.flush();

    std::string contents;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents));

    size_t found_threads = 0;
    for (int t = 0; t < LOG_THREADS; ++t)
    {
        if (contents.find(fmt::format("thread {} message", t)) != std::string::npos)
            found_threads++;
    }
    EXPECT_EQ(found_threads, static_cast<size_t>(LOG_THREADS));
    // Check that we have messages indicating a switch TO console
    EXPECT_NE(contents.find("Switching log sink to: Console"), std::string::npos);
    // Check that we have messages indicating a switch FROM console (i.e., to file)
    EXPECT_NE(contents.find("Log sink switched from: Console"), std::string::npos);
}

// What: Ensures the `flush()` command is synchronous and waits for the logger's
//       queue to be empty before returning.
// How: A large number of messages are logged in a tight loop without any sleeps.
//      `flush()` is called immediately after. The test then checks the log file
//      to ensure that all messages have been written, proving that `flush()`
//      did not return prematurely.
TEST_F(LoggerTest, FlushWaitsForQueue)
{
    auto log_path = GetUniqueLogPath("flush_waits_for_queue");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_TRACE);
    ASSERT_TRUE(wait_for_string_in_file(log_path, "Log sink switched from: Console"));
    std::string contents_before;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_before));
    size_t lines_before = count_lines(contents_before);

    const int MESSAGES = 500;
    for (int i = 0; i < MESSAGES; ++i)
    {
        LOGGER_INFO("flush-test: msg={}", i);
    }

    L.flush();

    std::string contents_after;
    ASSERT_TRUE(read_file_contents(log_path.string(), contents_after));
    size_t lines_after = count_lines(contents_after);
    EXPECT_EQ(lines_after - lines_before, static_cast<size_t>(MESSAGES));
}

// What: Verifies that the logger's shutdown mechanism is idempotent and thread-safe.
// How: It logs a message, then spawns multiple threads that all call `shutdown()`
//      concurrently. It then attempts to log another message, which should be
//      dropped. The test verifies that no crash occurs and that the second message
//      was not written to the log file.
TEST_F(LoggerTest, ShutdownIdempotency)
{
    auto log_path = GetUniqueLogPath("shutdown_idempotency");

    Logger &L = Logger::instance();
    L.set_logfile(log_path.string());
    L.set_level(Logger::Level::L_INFO);

    LOGGER_INFO("Message before shutdown.");
    L.flush();

    std::string content_before_shutdown;
    ASSERT_TRUE(read_file_contents(log_path.string(), content_before_shutdown));
    EXPECT_NE(content_before_shutdown.find("Message before shutdown"), std::string::npos);

    const int THREADS = 16;
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([]() { Logger::instance().shutdown(); });
    }
    for (auto &t : threads) t.join();

    // This message should be silently dropped by the shutdown logger.
    LOGGER_INFO("This message should NOT be logged.");

    L.flush(); // Should be a no-op.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string content_after_shutdown;
    ASSERT_TRUE(read_file_contents(log_path.string(), content_after_shutdown));

    EXPECT_EQ(content_after_shutdown.find("This message should NOT be logged."), std::string::npos);
    EXPECT_EQ(content_before_shutdown, content_after_shutdown);
}

// What: Checks a critical deadlock-avoidance feature: logging from within a
//       write error callback should not deadlock the logger.
// How: A write error is forced by making the log file unwritable. A callback is
//      registered that, upon firing, attempts to log a *new* message. The test
//      verifies that this re-entrant log message is successfully written to the
//      *previous*, valid sink (in this case, another file) without deadlocking.
TEST_F(LoggerTest, ReentrantErrorCallback)
{
    auto initial_log_path = GetUniqueLogPath("reentrant_initial");

    Logger &L = Logger::instance();
    L.set_logfile(initial_log_path.string());
    ASSERT_TRUE(wait_for_string_in_file(initial_log_path, "Log sink switched from: Console"));

    std::atomic<bool> callback_invoked(false);
    L.set_write_error_callback([&](const std::string &msg) {
        // This log attempt is re-entrant. It must not deadlock.
        LOGGER_SYSTEM("Re-entrant log from error callback: {}", msg);
        callback_invoked = true;
    });

    // Force a write error by making the target log path unwritable.
#if defined(PLATFORM_WIN64)
    fs::path locked_log_path = fs::temp_directory_path() / "pylab_reentrant_locked.log";
    std::remove(locked_log_path.string().c_str());
    HANDLE h = CreateFileA(locked_log_path.string().c_str(), GENERIC_READ, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
#else
    fs::path readonly_dir = fs::temp_directory_path() / "pylab_readonly_dir_test_reentrant";
    fs::remove_all(readonly_dir);
    fs::create_directory(readonly_dir);
    chmod(readonly_dir.string().c_str(), S_IRUSR | S_IXUSR); // 0500
    fs::path locked_log_path = readonly_dir / "test.log";
#endif

    L.set_logfile(locked_log_path.string()); // This will fail internally.
    LOGGER_INFO("This message will be dropped and should trigger an error.");
    L.flush();

    // Verify the re-entrant message was written to the *original* log file.
    ASSERT_TRUE(wait_for_string_in_file(initial_log_path, "Re-entrant log from error callback"));
    EXPECT_TRUE(callback_invoked.load());

    // cleanup
#if defined(PLATFORM_WIN64)
    CloseHandle(h);
    fs::remove(locked_log_path);
#else
    chmod(readonly_dir.string().c_str(), S_IRWXU);
    fs::remove_all(readonly_dir);
#endif
}

// What: Verifies that the asynchronous write error callback is invoked when the
//       logger's worker thread fails to open a log file.
// How: It forces a file-open error by creating a read-only directory (on POSIX) or
//      a file locked for exclusive reading (on Windows). It then tells the logger
//      to switch to a file inside this location. The test verifies that the
//      registered error callback is invoked.
TEST_F(LoggerTest, WriteErrorCallbackAsync)
{
#if defined(PLATFORM_WIN64)
    fs::path test_log_path = fs::temp_directory_path() / "pylab_write_err_async.log";
    fs::remove(test_log_path);

    HANDLE h = CreateFileA(test_log_path.string().c_str(), GENERIC_READ, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);

    Logger &L = Logger::instance();
    std::atomic<bool> callback_invoked(false);
    L.set_write_error_callback([&](const std::string &msg) {
        callback_invoked = true;
        EXPECT_NE(msg.find("Failed to open log file"), std::string::npos);
    });

    L.set_logfile(test_log_path.string());
    LOGGER_INFO("This write should be dropped as sink creation failed.");
    L.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_TRUE(callback_invoked.load());
    CloseHandle(h);
    fs::remove(test_log_path);
#else
    // POSIX Test 1: read-only directory
    {
        fs::path readonly_dir = fs::temp_directory_path() / "pylab_readonly_dir_test";
        fs::remove_all(readonly_dir);
        fs::create_directory(readonly_dir);
        chmod(readonly_dir.string().c_str(), S_IRUSR | S_IXUSR); // 0500
        fs::path locked_log_path = readonly_dir / "test.log";

        Logger &L = Logger::instance();
        std::atomic<bool> callback_invoked(false);
        L.set_write_error_callback([&](const std::string &msg) {
            callback_invoked = true;
            EXPECT_NE(msg.find("Failed to open log file"), std::string::npos);
        });

        L.set_logfile(locked_log_path.string());
        LOGGER_INFO("This message will be dropped.");
        L.flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        EXPECT_TRUE(callback_invoked.load());

        chmod(readonly_dir.string().c_str(), S_IRWXU);
        fs::remove_all(readonly_dir);
    }

    // POSIX Test 2: read-only file
    {
        fs::path readonly_file = fs::temp_directory_path() / "pylab_readonly_file.log";
        {
            std::ofstream touch(readonly_file);
        }
        chmod(readonly_file.string().c_str(), S_IRUSR); // 0400

        Logger &L = Logger::instance();
        std::atomic<bool> callback_invoked(false);
        L.set_write_error_callback([&](const std::string &msg) {
            callback_invoked = true;
            EXPECT_NE(msg.find("Failed to open log file"), std::string::npos);
        });

        L.set_logfile(readonly_file.string());
        LOGGER_INFO("This message will also be dropped.");
        L.flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        EXPECT_TRUE(callback_invoked.load());

        chmod(readonly_file.string().c_str(), S_IRWXU);
        fs::remove(readonly_file);
    }
#endif
}

// What: A placeholder test for manually verifying platform-native logging sinks.
// How: On Windows, it logs to the Event Log. On POSIX, it logs to syslog. It is
//      disabled by default (`DISABLED_`) because it cannot be automatically
//      verified by a CI runner and requires a human to check the system logs.
TEST_F(LoggerTest, DISABLED_PlatformSinks)
{
    Logger &L = Logger::instance();
#if defined(_WIN32)
    L.set_eventlog(L"PyLabHubTestLogger");
    LOGGER_INFO("Testing Windows Event Log sink.");
    L.flush();
    fmt::print(stderr, "\n  MANUAL VERIFICATION REQUIRED for Windows Event Log:\n");
    fmt::print(stderr, "  1. Open Event Viewer (eventvwr.msc).\n");
    fmt::print(stderr, "  2. Look for an Information-level message from 'PyLabHubTestLogger'.\n\n");
#else
    L.set_syslog("pylab-logger-test");
    LOGGER_INFO("Testing syslog sink.");
    L.flush();
    fmt::print(stderr, "\n  MANUAL VERIFICATION REQUIRED for Syslog:\n");
    fmt::print(stderr, "  1. Run 'journalctl -r | grep \"pylab-logger-test\"' or\n");
    fmt::print(stderr, "     'cat /var/log/syslog | grep \"pylab-logger-test\"'.\n\n");
#endif
    SUCCEED();
}

// What: A chaos test to verify the logger remains stable during concurrent
//       lifecycle operations (logging, flushing, changing sinks) and shutdown.
// How: It spawns multiple threads that perform different actions in tight loops:
//      some log, some flush, some switch sinks. After a duration, `shutdown()`
//      is called from the main thread, and the test verifies that the logger
//      shuts down cleanly without crashing.
TEST_F(LoggerTest, ConcurrentLifecycleChaos)
{
    auto chaos_log_path = GetUniqueLogPath("lifecycle_chaos");
    std::atomic<bool> stop_flag(false);
    const int DURATION_MS = 10000;

    auto worker = [&](int id, auto fn) {
        while (!stop_flag.load(std::memory_order_relaxed)) fn(id);
    };

    std::vector<std::thread> threads;

    // Logging threads
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back(worker, i, [](int id) {
            LOGGER_INFO("chaos-log-{}: message", id);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        });
    }

    // Flushing threads
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(worker, i, [](int) {
            Logger::instance().flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
    }

    // Sink switching threads
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(worker, i, [&](int id) {
            if (id % 2 == 0) Logger::instance().set_console();
            else Logger::instance().set_logfile(chaos_log_path.string());
            Logger::instance().set_write_error_callback(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        });
    }

        std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
        // The critical action: shut down the logger while worker threads are busy.
        Logger::instance().shutdown();
        stop_flag.store(true);
    
        for (auto &t : threads) t.join();
        SUCCEED(); // Success is simply not crashing.
    }
