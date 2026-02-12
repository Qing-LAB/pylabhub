// tests/test_harness/logger_workers.cpp
/**
 * @file logger_workers.cpp
 * @brief Implements worker functions for the Logger unit tests.
 *
 * These functions are designed to be executed in separate processes to test
 * various features of the Logger, including multi-process and multi-threaded
 * logging, lifecycle management, and error handling.
 */
#include <future>

#include "logger_workers.h"
#include "test_entrypoint.h"
#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "gtest/gtest.h"

// Platform-specific
#if defined(PLATFORM_WIN64)
#include <windows.h>
#else
#include <unistd.h> // for getpid
#endif

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
            ASSERT_TRUE(L.set_logfile(log_path, true)); // Append mode
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
            ASSERT_TRUE(Logger::instance().set_logfile(log_path_str));
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
            ASSERT_TRUE(Logger::instance().set_logfile(log_path_str));
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
            ASSERT_TRUE(Logger::instance().set_logfile(log_path_str));
            LOGGER_INFO_RT("Bad format: {} {}", "one"); // Too few args should cause format error
            Logger::instance().flush();

            // Verify that a format error message was logged
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            PLH_DEBUG("Log file contents for BadFormatString test:\n{}", contents); // ADDED DEBUG LINE
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

            ASSERT_TRUE(Logger::instance().set_logfile(log_path_str));
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
            ASSERT_TRUE(Logger::instance().set_logfile(log_path_str, true));
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
            ASSERT_EQ(count_lines(contents, "msg from thread"), THREADS * MSGS_PER_THREAD);
        },
        "logger::test_multithread_stress", Logger::GetLifecycleModule());
}

// Worker to test that `flush()` correctly waits for the log queue to be processed.
int test_flush_waits_for_queue(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            ASSERT_TRUE(Logger::instance().set_logfile(log_path_str));
            for (int i = 0; i < 100; ++i)
                LOGGER_INFO("message {}", i);
            Logger::instance().flush(); // This should block until all 100 messages are written.

            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path_str, contents));
            ASSERT_EQ(count_lines(contents, "message "), 100);
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
            ASSERT_TRUE(L.set_logfile(log_path.string()));
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
                threads.emplace_back(
                    []()
                    { LifecycleManager::instance().finalize(std::source_location::current()); });
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
            ASSERT_FALSE(Logger::instance().set_logfile("/"));
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

            ASSERT_FALSE(Logger::instance().set_logfile("/")); // Force an error
            Logger::instance().flush();

            // Wait for the callback to be invoked.
            auto future_status = err_msg_future.wait_for(2s);
            ASSERT_EQ(future_status, std::future_status::ready)
                << "Callback was not invoked within the timeout.";
                                ASSERT_NE(err_msg_future.get().find("Failed to create FileSink"), std::string::npos);
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
            ASSERT_TRUE(Logger::instance().set_eventlog(L"pylab-test-event-source"));
            LOGGER_INFO("Test message to Windows Event Log.");
#else
            ASSERT_TRUE(Logger::instance().set_syslog("pylab-test"));
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
    pylabhub::utils::RegisterModule(Logger::GetLifecycleModule());
    LifecycleManager::instance().initialize(std::source_location::current());

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
                                     (void)Logger::instance().set_console();
                                 else
                                     (void)Logger::instance().set_logfile(chaos_log_path.string());
                             });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));

    // Finalize while threads are running
    LifecycleManager::instance().finalize(std::source_location::current());
    stop_flag.store(true); // Signal threads to stop

    for (auto &t : threads)
        t.join();

    // Success is simply not crashing during the chaos.
    return 0;
}

// Worker to test inter-process locking with flock.
int test_inter_process_flock(const std::string &log_path, const std::string &worker_id,
                             int msg_count)
{
    return run_gtest_worker(
        [&]()
        {
            Logger &L = Logger::instance();
            L.set_log_sink_messages_enabled(false);
            ASSERT_TRUE(L.set_logfile(log_path, true)); // use_flock = true
            L.set_level(Logger::Level::L_INFO);

            for (int i = 0; i < msg_count; ++i)
            {
                // Create a long, unique, and verifiable payload.
                std::string payload = fmt::format(
                    "WORKER_ID={} MSG_NUM={} PAYLOAD=[ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789]",
                    worker_id, i);
                LOGGER_INFO("{}", payload);
            }
            L.flush();
        },
        "logger::test_inter_process_flock", Logger::GetLifecycleModule());
}

// Worker to test the RotatingFileSink functionality.
int test_rotating_file_sink(const std::string &base_log_path_str, size_t max_file_size_bytes,
                            size_t max_backup_files)
{
    return run_gtest_worker(
        [&]()
        {
            fs::path base_log_path(base_log_path_str);

            Logger &L = Logger::instance();
            // L.set_log_sink_messages_enabled(false); // Disable sink switch messages for this test
            fmt::print("Setting up rotating file sink: base_path='{}', max_size={} bytes, "
                       "max_backups={}\n",
                       base_log_path.string(), max_file_size_bytes, max_backup_files);
            std::error_code ec;
            bool success = L.set_rotating_logfile(base_log_path, max_file_size_bytes,
                                                  max_backup_files, false, ec);
            fmt::print("Rotating file sink setup success={}, ec={}\n", success, ec.message());

            ASSERT_TRUE(success);
            ASSERT_FALSE(ec);

            const int total_messages = 20;
            for (int i = 0; i < total_messages; ++i)
            {
                // Each message is ~100 bytes, so rotation should happen every ~2-3 messages.
                LOGGER_INFO("ROTATION-TEST-MSG-{:03} {}", i, std::string(50, 'X'));
            }
            L.flush();
            fmt::print("Finished logging {} messages for rotation test.\n", total_messages);

            // --- Verification Phase ---
            // Read all existing log files into a single string for analysis.
            std::string full_log_contents;
            std::string content_buffer;
            // Backups are numbered .1, .2, etc. We read in reverse order of numbering
            // to process from oldest to newest.
            for (size_t i = max_backup_files; i > 0; --i)
            {
                fs::path p = fs::path(base_log_path.string() + "." + std::to_string(i));
                if (fs::exists(p) && read_file_contents(p.string(), content_buffer))
                {
                    full_log_contents += content_buffer;
                }
            }
            if (fs::exists(base_log_path) &&
                read_file_contents(base_log_path.string(), content_buffer))
            {
                full_log_contents += content_buffer;
            }

            // 1. Verify that rotation actually happened.
            ASSERT_GT(count_lines(full_log_contents, "--- Log rotated successfully ---"), 0)
                << "Log rotation system message was not found.";

            // 2. Find the first message that wasn't purged by rotation.
            int first_found_idx = -1;
            for (int i = 0; i < total_messages; ++i)
            {
                if (full_log_contents.find(fmt::format("ROTATION-TEST-MSG-{:03}", i)) !=
                    std::string::npos)
                {
                    first_found_idx = i;
                    break;
                }
            }
            ASSERT_NE(first_found_idx, -1) << "No test messages found in any log files.";

            // 3. Verify that from the first found message to the end, there are no gaps.
            for (int i = first_found_idx; i < total_messages; ++i)
            {
                EXPECT_NE(full_log_contents.find(fmt::format("ROTATION-TEST-MSG-{:03}", i)),
                          std::string::npos)
                    << "Missing message " << i << " in final concatenated log. A gap was detected.";
            }

            // 4. Verify that the number of found messages is correct.
            size_t expected_message_count = total_messages - first_found_idx;
            ASSERT_EQ(count_lines(full_log_contents, "ROTATION-TEST-MSG-"), expected_message_count);

            SUCCEED() << "Rotating file sink test completed successfully.";
        },
        "logger::test_rotating_file_sink", Logger::GetLifecycleModule());
}

// Worker to test the logger's message dropping behavior when the queue is full.
int test_queue_full_and_message_dropping(const std::string &log_path_str)
{
    return run_gtest_worker(
        [&]()
        {
            fs::path log_path(log_path_str);
            Logger &logger = Logger::instance();
            const size_t max_queue = 5;
            logger.set_max_queue_size(max_queue);
            ASSERT_TRUE(logger.set_logfile(log_path.string()));
            logger.set_level(Logger::Level::L_INFO);
            logger.set_log_sink_messages_enabled(false);

            // 1. Fill the queue deterministically until the logger starts dropping messages.
            int messages_enqueued = 0;
            int messages_dropped = 0;
            // Loop more than enough times to ensure we trigger the drop.
            for (int i = 0; i < 100; ++i)
            {
                if (LOGGER_INFO("Message {}", i))
                {
                    messages_enqueued++;
                }
                else
                {
                    messages_dropped++;
                }
            }

            // We must have enqueued some, but not all, messages.
            ASSERT_GT(messages_enqueued, 0);
            ASSERT_EQ(messages_enqueued + messages_dropped, 100);

            // 1b. Assert get_total_dropped_since_sink_switch() matches our count
            ASSERT_EQ(logger.get_total_dropped_since_sink_switch(),
                      static_cast<size_t>(messages_dropped))
                << "get_total_dropped_since_sink_switch() should match the number of dropped messages";

            // 2. Flush the logger to ensure all enqueued messages and warnings are written.
            logger.flush();

            // 3. Verification
            std::string contents;
            ASSERT_TRUE(read_file_contents(log_path.string(), contents));
            PLH_DEBUG("Log file contents for QueueFullAndMessageDropping test:\n{}", contents);

            // 4a. Verify the preliminary "heads-up" warning exists.
            ASSERT_NE(contents.find("Overflow detected"), std::string::npos)
                << "The preliminary 'Overflow detected' warning was not found.";

            // 4b. Verify the final "summary" warning exists.
            std::string summary_substr = "Summary: At this point in time, the Logger dropped";
            ASSERT_NE(contents.find(summary_substr), std::string::npos)
                << "Final summary message about dropped logs not found in file.";

            // 4c. Extract and sum all "Summary: ... dropped N" numbers (there may be multiple lines).
            size_t reported_dropped_count = 0;
            for (std::string::size_type pos = 0;
                 (pos = contents.find(summary_substr, pos)) != std::string::npos;
                 pos += summary_substr.length())
            {
                std::string::size_type num_pos = pos + summary_substr.length();
                char *end_ptr = nullptr;
                reported_dropped_count += std::strtoul(contents.c_str() + num_pos, &end_ptr, 10);
            }
            ASSERT_EQ(reported_dropped_count, static_cast<size_t>(messages_dropped))
                << "The total of dropped messages reported in the summary lines is incorrect.";

            // 4d. Count the number of actual INFO messages logged.
            size_t logged_info_count = count_lines(contents, "Message ");
            ASSERT_EQ(logged_info_count, messages_enqueued)
                << "The number of logged INFO messages does not match the number successfully enqueued.";
        },
        "logger::test_queue_full_and_message_dropping", Logger::GetLifecycleModule());
}

int use_without_lifecycle_aborts()
{
    // No LifecycleGuard - Logger module not initialized.
    // Calling set_logfile should PLH_PANIC and abort. If it returns instead,
    // we must signal failure so the parent's ASSERT_NE(exit, 0) fails.
    bool ok = Logger::instance().set_logfile("/tmp/pylabhub_logger_no_lifecycle.log");
    (void)ok;
    return 0;  // Should not reach here; if we do, implementation changed
}

} // namespace logger
} // namespace pylabhub::tests::worker

// Self-registering dispatcher — no separate dispatcher file needed.
namespace
{
struct LoggerWorkerRegistrar
{
    LoggerWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "logger")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::logger;
                if (scenario == "test_basic_logging" && argc > 2)
                    return test_basic_logging(argv[2]);
                if (scenario == "test_log_level_filtering" && argc > 2)
                    return test_log_level_filtering(argv[2]);
                if (scenario == "test_bad_format_string" && argc > 2)
                    return test_bad_format_string(argv[2]);
                if (scenario == "test_default_sink_and_switching" && argc > 2)
                    return test_default_sink_and_switching(argv[2]);
                if (scenario == "test_multithread_stress" && argc > 2)
                    return test_multithread_stress(argv[2]);
                if (scenario == "test_flush_waits_for_queue" && argc > 2)
                    return test_flush_waits_for_queue(argv[2]);
                if (scenario == "test_shutdown_idempotency" && argc > 2)
                    return test_shutdown_idempotency(argv[2]);
                if (scenario == "test_reentrant_error_callback" && argc > 2)
                    return test_reentrant_error_callback(argv[2]);
                if (scenario == "test_write_error_callback_async")
                    return test_write_error_callback_async();
                if (scenario == "test_platform_sinks")
                    return test_platform_sinks();
                if (scenario == "test_concurrent_lifecycle_chaos" && argc > 2)
                    return test_concurrent_lifecycle_chaos(argv[2]);
                if (scenario == "stress_log" && argc > 3)
                {
                    stress_log(argv[2], std::stoi(argv[3]));
                    return 0;
                }
                if (scenario == "test_inter_process_flock" && argc > 4)
                    return test_inter_process_flock(argv[2], argv[3], std::stoi(argv[4]));
                if (scenario == "test_rotating_file_sink" && argc > 4)
                    return test_rotating_file_sink(argv[2],
                                                   static_cast<size_t>(std::stoul(argv[3])),
                                                   static_cast<size_t>(std::stoul(argv[4])));
                if (scenario == "test_queue_full_and_message_dropping" && argc > 2)
                    return test_queue_full_and_message_dropping(argv[2]);
                if (scenario == "use_without_lifecycle_aborts")
                    return use_without_lifecycle_aborts();
                fmt::print(stderr, "ERROR: Unknown logger scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static LoggerWorkerRegistrar g_logger_registrar;
} // namespace
