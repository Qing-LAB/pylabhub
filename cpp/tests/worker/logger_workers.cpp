
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
#include "logger_worker.h"       // Keep this specific header
#include "shared_test_helpers.h" // Keep this specific helper header
#include "test_process_utils.h" // Explicitly include test_process_utils.h for test_utils namespace
#include "utils/Logger.hpp"
#include "utils/Lifecycle.hpp"

using namespace test_utils;

namespace pylabhub::tests::worker
{
namespace logger
{

// NOTE: The implementations for many of these workers were missing in the original
// workers.cpp file (commented as /* ... */). They are left as empty function
// bodies here. If the original logic exists elsewhere, it should be restored.

void stress_log(const std::string &log_path, int msg_count)
{
    LifecycleManager::instance().initialize();
    auto finalizer = pylabhub::basics::make_scope_guard([] { LifecycleManager::instance().finalize(); });
    Logger &L = Logger::instance();
    L.set_logfile(log_path, true);
    L.set_level(Logger::Level::L_TRACE);
    for (int i = 0; i < msg_count; ++i)
    {
        if (std::rand() % 10 == 0) std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
#if defined(PLATFORM_WIN64)
        LOGGER_INFO("child-msg pid={} idx={}", GetCurrentProcessId(), i);
#else
        LOGGER_INFO("child-msg pid={} idx={}", getpid(), i);
#endif
    }
    L.flush();
}

int test_basic_logging(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]() {
            Logger::instance().set_logfile(log_path_str);
            LOGGER_INFO("Hello, world!");
            Logger::instance().flush();
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_NE(contents.find("Hello, world!"), std::string::npos);
        },
        "logger::test_basic_logging");
}

int test_log_level_filtering(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]() {
            Logger::instance().set_logfile(log_path_str);
            Logger::instance().set_level(Logger::Level::L_WARNING);
            LOGGER_INFO("This should be filtered.");
            LOGGER_WARN("This should appear.");
            Logger::instance().flush();
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_EQ(contents.find("This should be filtered."), std::string::npos);
            ASSERT_NE(contents.find("This should appear."), std::string::npos);
        },
        "logger::test_log_level_filtering");
}

int test_bad_format_string(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]() {
            Logger::instance().set_logfile(log_path_str);
            LOGGER_INFO("Bad format: {}", "one", "two"); // Extra arg
            Logger::instance().flush();
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            // The fallback format error message should be logged.
            ASSERT_NE(contents.find("[FORMAT ERROR]"), std::string::npos);
        },
        "logger::test_bad_format_string");
}

int test_default_sink_and_switching(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]() {
            // Note: Default sink is stderr, which isn't captured. This test
            // mainly verifies the switch works.
            LOGGER_SYSTEM("This goes to default sink.");
            Logger::instance().set_logfile(log_path_str);
            LOGGER_SYSTEM("This should be in the file.");
            Logger::instance().flush();
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_NE(contents.find("This should be in the file."), std::string::npos);
        },
        "logger::test_default_sink_and_switching");
}

int test_multithread_stress(const std::string &log_path_str)
{
     return run_gtest_worker(
        [&]() {
            const int THREADS = scaled_value(16, 4);
            const int MSGS_PER_THREAD = scaled_value(200, 50);
            Logger::instance().set_logfile(log_path_str, true);
            std::vector<std::thread> threads;
            for (int i = 0; i < THREADS; ++i)
            {
                threads.emplace_back([&, i]() {
                    for (int j = 0; j < MSGS_PER_THREAD; ++j)
                    {
                        LOGGER_INFO("msg from thread {}-{}", i, j);
                    }
                });
            }
            for (auto &t : threads) t.join();
            Logger::instance().flush();

            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_EQ(count_lines(contents), THREADS * MSGS_PER_THREAD);
        },
        "logger::test_multithread_stress");
}

int test_flush_waits_for_queue(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]() {
            Logger::instance().set_logfile(log_path_str);
            for(int i=0; i<100; ++i) LOGGER_INFO("message {}", i);
            Logger::instance().flush();
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_EQ(count_lines(contents), 100);
        },
        "logger::test_flush_waits_for_queue");
}

int test_shutdown_idempotency(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]() {
            fs::path log_path(log_path_str);
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
                threads.emplace_back([]() { LifecycleManager::instance().finalize(); });
            }
            for (auto &t : threads)
                t.join();

            // This log should be gracefully ignored by the fallback mechanism
            LOGGER_INFO("This message should NOT be logged.");
            std::this_thread::sleep_for(100ms);

            std::string content_after_shutdown;
            ASSERT_TRUE(read_file_contents(log_path.string(), content_after_shutdown));
            EXPECT_EQ(content_after_shutdown.find("This message should NOT be logged."),
                      std::string::npos);
        },
        "logger::test_shutdown_idempotency");
}

int test_reentrant_error_callback([[maybe_unused]] const std::string &initial_log_path_str)
{
    return run_gtest_worker(
        [&]() {
            // This test requires a sink that is guaranteed to fail.
            // On POSIX, we can point it to a directory.
#if !defined(PLATFORM_WIN64)
            std::atomic<int> callback_count = 0;
            Logger::instance().set_write_error_callback([&](const std::string& err_msg){
                callback_count++;
                // Re-entrant log call inside the error callback
                LOGGER_SYSTEM("Log from error callback: {}", err_msg);
            });

            // Set log file to a directory, which will cause write errors
            Logger::instance().set_logfile("/");
            LOGGER_ERROR("This write will fail.");
            Logger::instance().flush(); // Ensure the error is processed

            ASSERT_GE(callback_count.load(), 1);
#else
            // Cannot guarantee a write failure on Windows in the same way.
            // Mark test as passed.
            GTEST_SUCCESS_("Windows does not have a simple equivalent of writing to a directory to force a log error.");
#endif
        },
        "logger::test_reentrant_error_callback");
}

int test_write_error_callback_async()
{
    return run_gtest_worker(
        [&]() {
#if !defined(PLATFORM_WIN64)
            std::promise<std::string> err_msg_promise;
            auto err_msg_future = err_msg_promise.get_future();
            Logger::instance().set_write_error_callback([&](const std::string& msg){
                err_msg_promise.set_value(msg);
            });

            Logger::instance().set_logfile("/");
            LOGGER_ERROR("This will fail.");
            Logger::instance().flush();

            auto future_status = err_msg_future.wait_for(2s);
            ASSERT_EQ(future_status, std::future_status::ready) << "Callback was not invoked within the timeout.";
            ASSERT_NE(err_msg_future.get().find("Logger error"), std::string::npos);
#else
            GTEST_SUCCESS_("Windows does not have a simple equivalent of writing to a directory to force a log error.");
#endif
        },
        "logger::test_write_error_callback_async");
}

int test_platform_sinks()
{
    return run_gtest_worker(
        [&]() {
            // This test is mostly a smoke test to ensure platform sinks don't crash.
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
        "logger::test_platform_sinks");
}

int test_concurrent_lifecycle_chaos(const std::string &log_path_str)
{
    // This test manually manages its lifecycle to test shutdown under load.
    // It does not use the run_gtest_worker template because the point is to
    // call FinalizeApplication while other threads are active.
    LifecycleManager::instance().initialize();

    fs::path chaos_log_path(log_path_str);
    std::atomic<bool> stop_flag(false);
    const int DURATION_MS = scaled_value(1000, 250);

    auto worker_thread_fn = [&](auto fn) {
        while (!stop_flag.load(std::memory_order_relaxed))
            fn();
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(worker_thread_fn, []() {
            LOGGER_INFO("chaos-log: message");
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        });
    }
    for (int i = 0; i < 1; ++i) {
        threads.emplace_back(worker_thread_fn, []() {
            Logger::instance().flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
    }
    for (int i = 0; i < 1; ++i) {
        threads.emplace_back(worker_thread_fn, [&]() {
            if (std::rand() % 2 == 0)
                Logger::instance().set_console();
            else
                Logger::instance().set_logfile(chaos_log_path.string());
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    LifecycleManager::instance().finalize();
    stop_flag.store(true);
    for (auto &t : threads) t.join();
    return 0; // Success is simply not crashing.
}

} // namespace logger
} // namespace worker
