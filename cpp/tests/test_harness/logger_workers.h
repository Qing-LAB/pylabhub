#pragma once
#include <string>
#include <vector>

// tests/test_harness/logger_workers.h

#pragma once

#include <string>

#include <vector>

/**

 * @file logger_workers.h

 * @brief Declares worker functions for Logger tests.

 *

 * These functions are designed to be executed in separate processes to test

 * various features of the Logger, including multi-process and multi-threaded

 * logging, lifecycle management, and error handling.

 */

namespace pylabhub::tests::worker

{

namespace logger

{

/**

 * @brief Worker that writes a high volume of log messages from a single process.

 * @param log_path Path to the output log file.

 * @param msg_count The number of messages to log.

 * @return 0 on success, non-zero on failure.

 */

int stress_log(const std::string &log_path, int msg_count);

/**

 * @brief Tests basic file logging in a worker process.

 * @return 0 on success, non-zero on failure.

 */

int test_basic_logging(const std::string &log_path_str);

/**

 * @brief Tests that log messages are filtered based on the current log level.

 * @return 0 on success, non-zero on failure.

 */

int test_log_level_filtering(const std::string &log_path_str);

/**

 * @brief Tests the logger's behavior with a malformed format string.

 * @return 0 on success, non-zero on failure.

 */

int test_bad_format_string(const std::string &log_path_str);

/**

 * @brief Tests switching from the default sink to a file sink.

 * @return 0 on success, non-zero on failure.

 */

int test_default_sink_and_switching(const std::string &log_path_str);

/**

 * @brief Writes log messages from multiple threads within a single worker process.

 * @return 0 on success, non-zero on failure.

 */

int test_multithread_stress(const std::string &log_path_str);

/**

 * @brief Verifies that `flush()` waits for the logging queue to be empty.

 * @return 0 on success, non-zero on failure.

 */

int test_flush_waits_for_queue(const std::string &log_path_str);

/**

 * @brief Tests that the logger shutdown process is idempotent.

 * @return 0 on success, non-zero on failure.

 */

int test_shutdown_idempotency(const std::string &log_path_str);

/**

 * @brief Tests the behavior of the write error callback, including re-entrant logging.

 * @return 0 on success, non-zero on failure.

 */

int test_reentrant_error_callback(const std::string &initial_log_path_str);

/**

 * @brief Tests that the asynchronous write error callback is invoked on failure.

 * @return 0 on success, non-zero on failure.

 */

int test_write_error_callback_async();

/**

 * @brief Smoke test for platform-specific sinks (Event Log on Windows, syslog on POSIX).

 * @return 0 on success, non-zero on failure.

 */

int test_platform_sinks();

/**

 * @brief Tests the stability of the logger and lifecycle manager under chaotic concurrent
 operations.

 * @return 0 on success, non-zero on failure.

 */

int test_concurrent_lifecycle_chaos(const std::string &log_path_str);

} // namespace logger

} // namespace pylabhub::tests::worker
