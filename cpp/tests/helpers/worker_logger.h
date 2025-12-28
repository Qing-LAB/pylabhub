#pragma once
#include <string>
#include <vector>

// Declares worker functions for Logger multi-process tests.
namespace worker
{
    namespace logger
    {
        void stress_log(const std::string& log_path, int msg_count);

        // Worker function for the BasicLogging test.
        // Returns 0 on success, non-zero on failure.
        int test_basic_logging(const std::string& log_path_str);

        // Worker function for the LogLevelFiltering test.
        // Returns 0 on success, non-zero on failure.
        int test_log_level_filtering(const std::string& log_path_str);

        // Worker function for the BadFormatString test.
        int test_bad_format_string(const std::string& log_path_str);

        // Worker function for the DefaultSinkAndSwitching test.
        int test_default_sink_and_switching(const std::string& log_path_str);

        // Worker function for the MultithreadStress test.
        int test_multithread_stress(const std::string& log_path_str);

        // Worker function for the FlushWaitsForQueue test.
        int test_flush_waits_for_queue(const std::string& log_path_str);

        // Worker function for the ShutdownIdempotency test.
        int test_shutdown_idempotency(const std::string& log_path_str);

        // Worker function for the ReentrantErrorCallback test.
        int test_reentrant_error_callback(const std::string& initial_log_path_str);

        // Worker function for the WriteErrorCallbackAsync test.
        int test_write_error_callback_async();

        // Worker function for the DISABLED_PlatformSinks test.
        int test_platform_sinks();

        // Worker function for the ConcurrentLifecycleChaos test.
        int test_concurrent_lifecycle_chaos(const std::string& log_path_str);

    } // namespace logger
} // namespace worker