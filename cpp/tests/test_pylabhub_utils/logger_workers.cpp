// tests/test_harness/logger_workers.cpp
/**
 * @file logger_workers.cpp
 * @brief Implements worker functions for the Logger unit tests.
 *
 * These functions are designed to be executed in separate processes to test
 * various features of the Logger, including multi-process and multi-threaded
 * logging, lifecycle management, and error handling.
 */
#include "platform.hpp"

// Standard Library
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

// Platform-specific
#if defined(PLATFORM_WIN64)
#include <windows.h>
#else
#include <unistd.h> // for getpid
#endif

// Project-specific
#include "logger_workers.h"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "utils/Logger.hpp"

using namespace pylabhub::tests::helper;
using namespace pylabhub::utils;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker
{
namespace logger
{
// Worker to log a large number of messages to test throughput and file writing.
int stress_log(const std::string &log_path, int msg_count)
{
    return run_gtest_worker(
        [&]()
        {
            Logger &L = Logger::instance();
            L.set_log_sink_messages_enabled(false); // Disable sink switch messages for this test
            L.set_logfile(log_path, true);          // Append mode
            L.set_level(Logger::Level::L_TRACE);
            for (int i = 0; i < msg_count; ++i)
            {
                // Introduce slight delays to simulate real-world conditions
                if (std::rand() % 10 == 0)
                    std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
#if defined(PLATFORM_WIN64)
                LOGGER_INFO("child-msg pid={} idx={}", GetCurrentProcessId(), i);
#else
                LOGGER_INFO("child-msg pid={} idx={}", getpid(), i);
#endif
            }
            L.set_log_sink_messages_enabled(true); // Re-enable for subsequent tests if any
            L.flush();
        },
        "logger::stress_log", Logger::GetLifecycleModule());
}

// Worker to test basic logging to a file.
int test_basic_logging(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            Logger::instance().set_logfile(log_path_str);
            LOGGER_INFO("Hello, world!");
            Logger::instance().flush();

            // Verify the message was written to the file
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_NE(contents.find("Hello, world!"), std::string::npos);
        },
        "logger::test_basic_logging", Logger::GetLifecycleModule());
}

// Worker to test log level filtering.
int test_log_level_filtering(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            Logger::instance().set_logfile(log_path_str);
            Logger::instance().set_level(Logger::Level::L_WARNING);

            LOGGER_INFO("This should be filtered.");
            LOGGER_WARN("This should appear.");
            Logger::instance().flush();

            // Verify that only the WARNING message was logged
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_EQ(contents.find("This should be filtered."), std::string::npos);
            ASSERT_NE(contents.find("This should appear."), std::string::npos);
        },
        "logger::test_log_level_filtering", Logger::GetLifecycleModule());
}

// Worker to test the fallback mechanism for bad format strings.
int test_bad_format_string(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            Logger::instance().set_logfile(log_path_str);
            LOGGER_INFO("Bad format: {}", "one", "two"); // Extra arg should cause format error
            Logger::instance().flush();

            // Verify that a format error message was logged
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_NE(contents.find("[FORMAT ERROR]"), std::string::npos);
        },
        "logger::test_bad_format_string", Logger::GetLifecycleModule());
}

// Worker to test switching from the default sink (stderr) to a file sink.
int test_default_sink_and_switching(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            // This message goes to the default sink (stderr), which is not captured by the test.
            LOGGER_SYSTEM("This goes to default sink.");

            // Switch to a file sink
            Logger::instance().set_logfile(log_path_str);
            LOGGER_SYSTEM("This should be in the file.");
            Logger::instance().flush();

            // Verify the message was written to the file after the switch.
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_NE(contents.find("This should be in the file."), std::string::npos);
        },
        "logger::test_default_sink_and_switching", Logger::GetLifecycleModule());
}

// Worker to test thread-safe logging from multiple threads concurrently.
int test_multithread_stress(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            const int THREADS = scaled_value(16, 4);
            const int MSGS_PER_THREAD = scaled_value(200, 50);
            Logger::instance().set_logfile(log_path_str, true);
            std::vector<std::thread> threads;

            for (int i = 0; i < THREADS; ++i)
            {
                threads.emplace_back(
                    [&, i]()
                    {
                        for (int j = 0; j < MSGS_PER_THREAD; ++j)
                        {
                            LOGGER_INFO("msg from thread {}-{}", i, j);
                        }
                    });
            }
            for (auto &t : threads)
                t.join();
            Logger::instance().flush();

            // Verify that all messages were logged
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_EQ(count_lines(contents), THREADS * MSGS_PER_THREAD);
        },
        "logger::test_multithread_stress", Logger::GetLifecycleModule());
}

// Worker to test that `flush()` correctly waits for the log queue to be processed.
int test_flush_waits_for_queue(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            Logger::instance().set_logfile(log_path_str);
            for (int i = 0; i < 100; ++i)
                LOGGER_INFO("message {}", i);
            Logger::instance().flush(); // This should block until all 100 messages are written.

            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_EQ(count_lines(contents), 100);
        },
        "logger::test_flush_waits_for_queue", Logger::GetLifecycleModule());
}

// Worker to test that repeated calls to the lifecycle shutdown are handled gracefully.
int test_shutdown_idempotency(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            fs::path log_path(log_path_str);
            Logger &L = Logger::instance();
            L.set_logfile(log_path.string());
            L.set_level(Logger::Level::L_INFO);
            LOGGER_INFO("Message before shutdown.");
            L.flush();

            std::string content_before_shutdown;
            ASSERT_TRUE(read_file_contents(log_path.string(), content_before_shutdown));
            EXPECT_NE(content_before_shutdown.find("Message before shutdown"), std::string::npos);

            // Call finalize from multiple threads concurrently.
            const int THREADS = 16;
            std::vector<std::thread> threads;
            for (int i = 0; i < THREADS; ++i)
            {
                threads.emplace_back([]() { LifecycleManager::instance().finalize(); });
            }
            for (auto &t : threads)
                t.join();

            // This log call should be gracefully ignored by the fallback mechanism after shutdown.
            LOGGER_INFO("This message should NOT be logged.");
            std::this_thread::sleep_for(100ms);

            // Verify the message was not logged.
            std::string content_after_shutdown;
            ASSERT_TRUE(read_file_contents(log_path.string(), content_after_shutdown));
            EXPECT_EQ(content_after_shutdown.find("This message should NOT be logged."),
                      std::string::npos);
        },
        "logger::test_shutdown_idempotency", Logger::GetLifecycleModule());
}

// Worker to test re-entrant logging from within the error callback.
int test_reentrant_error_callback([[maybe_unused]] const std::string &initial_log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
    // This test requires a sink that is guaranteed to fail.
    // On POSIX, we can point it to a directory, which fails to open as a file.
#if !defined(PLATFORM_WIN64)
            std::atomic<int> callback_count = 0;
            Logger::instance().set_write_error_callback(
                [&](const std::string &err_msg)
                {
                    callback_count++;
                    // Re-entrant log call inside the error callback. This should not deadlock.
                    LOGGER_SYSTEM("Log from error callback: {}", err_msg);
                });

            // Set log file to a directory to cause a write error.
            Logger::instance().set_logfile("/");
            LOGGER_ERROR("This write will fail.");
            Logger::instance().flush(); // Ensure the error is processed by the background thread.

            ASSERT_GE(callback_count.load(), 1);
#else
            // Cannot guarantee a write failure on Windows in the same way.
            GTEST_SUCCESS_("Windows does not have a simple equivalent of writing to a directory to "
                           "force a log error.");
#endif
        },
        "logger::test_reentrant_error_callback", Logger::GetLifecycleModule());
}

// Worker to test the asynchronous invocation of the write error callback.
int test_write_error_callback_async()
{
    return run_gtest_worker(
        [&]()
        {
#if !defined(PLATFORM_WIN64)
            std::promise<std::string> err_msg_promise;
            auto err_msg_future = err_msg_promise.get_future();

            // The callback will fulfill the promise.
            Logger::instance().set_write_error_callback([&](const std::string &msg)
                                                        { err_msg_promise.set_value(msg); });

            Logger::instance().set_logfile("/"); // Force an error
            LOGGER_ERROR("This will fail.");
            Logger::instance().flush();

            // Wait for the callback to be invoked.
            auto future_status = err_msg_future.wait_for(2s);
            ASSERT_EQ(future_status, std::future_status::ready)
                << "Callback was not invoked within the timeout.";
            ASSERT_NE(err_msg_future.get().find("Logger error"), std::string::npos);
#else
            GTEST_SUCCESS_("Windows does not have a simple equivalent of writing to a directory to "
                           "force a log error.");
#endif
        },
        "logger::test_write_error_callback_async", Logger::GetLifecycleModule());
}

// Worker to smoke test platform-specific logging sinks.
int test_platform_sinks()
{
    return run_gtest_worker(
        [&]()
        {
    // This test is mostly to ensure that platform-specific sinks can be initialized
    // and used without crashing. Verification of output is a manual process.
#if defined(PLATFORM_WIN64)
            Logger::instance().set_eventlog(L"pylab-test-event-source");
            LOGGER_INFO("Test message to Windows Event Log.");
#else
            Logger::instance().set_syslog("pylab-test");
            LOGGER_INFO("Test message to syslog.");
#endif
            Logger::instance().flush();
            GTEST_SUCCESS_("Platform sink test completed without crashing.");
        },
        "logger::test_platform_sinks", Logger::GetLifecycleModule());
}

// Tests stability by running logging, flushing, and sink switching from multiple threads.
int test_concurrent_lifecycle_chaos(const std::string &log_path_str)
{
    // This test manually manages its lifecycle to test shutdown under load.
    // It does not use run_gtest_worker, as the goal is to call finalize()
    // while other threads are actively using the logger.
    // Register the Logger module with the LifecycleManager.
    pylabhub::lifecycle::RegisterModule(Logger::GetLifecycleModule());
    LifecycleManager::instance().initialize();

    fs::path chaos_log_path(log_path_str);
    std::atomic<bool> stop_flag(false);
    const int DURATION_MS = scaled_value(1000, 250);

    auto worker_thread_fn = [&](auto fn)
    {
        while (!stop_flag.load(std::memory_order_relaxed))
            fn();
    };

    std::vector<std::thread> threads;
    // Logging threads
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back(worker_thread_fn,
                             []()
                             {
                                 LOGGER_INFO("chaos-log: message");
                                 std::this_thread::sleep_for(std::chrono::microseconds(500));
                             });
    }
    // Flushing thread
    for (int i = 0; i < 1; ++i)
    {
        threads.emplace_back(worker_thread_fn,
                             []()
                             {
                                 Logger::instance().flush();
                                 std::this_thread::sleep_for(std::chrono::milliseconds(10));
                             });
    }
    // Sink-switching thread
    for (int i = 0; i < 1; ++i)
    {
        threads.emplace_back(worker_thread_fn,
                             [&]()
                             {
                                 if (std::rand() % 2 == 0)
                                     Logger::instance().set_console();
                                 else
                                     Logger::instance().set_logfile(chaos_log_path.string());
                             });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));

    // Finalize while threads are running
    LifecycleManager::instance().finalize();
    stop_flag.store(true); // Signal threads to stop

    for (auto &t : threads)
        t.join();

    // Success is simply not crashing during the chaos.
    return 0;
}

} // namespace logger
} // namespace pylabhub::tests::worker
