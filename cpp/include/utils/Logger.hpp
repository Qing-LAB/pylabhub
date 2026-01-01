/*******************************************************************************
 * @file Logger.hpp
 * @brief High-performance, asynchronous, thread-safe logging utility.
 *
 * @see src/utils/Logger.cpp
 * @see tests/logger/test_basic_logging.cpp
 *
 * **Design Philosophy: Decoupled Command-Queue**
 *
 * The Logger is engineered for high-throughput applications where logging latency
 * must not impact the performance of critical application threads. It achieves
 * this using a decoupled, asynchronous architecture based on a command-queue
 * pattern.
 *
 * 1.  **Asynchronous by Default**: Calls from application threads (e.g., via the
 *     `LOGGER_INFO(...)` macro) are lightweight. They simply format a log message
 *     and emplace a "command" object onto a thread-safe, lock-free queue. This
 *     is a very fast operation that avoids blocking the calling thread on slow
 *     I/O (disk, console, network, etc.).
 *
 * 2.  **Dedicated Worker Thread**: A single background thread is the **sole
 *     consumer** of the command queue. It is responsible for all potentially
 *     blocking operations: writing to the active sink (console, file), flushing
 *     buffers, and managing sink lifetimes (e.g., opening/closing files). This
 *     design isolates I/O latency from application logic and naturally
 *     serializes access to shared resources like file handles, eliminating the
 *     need for complex locking.
 *
 * 3.  **Sink Abstraction**: A `Sink` base class defines a simple interface for
 *     writing and flushing log messages. Concrete implementations like
 *     `ConsoleSink` and `FileSink` encapsulate the details of each log
 *     destination. This makes the logger easily extensible to other outputs
 *     (e.g., network sockets, syslog) in the future.
 *
 * 4.  **Thread Safety & Ordering**: All public methods are thread-safe.
 *     Logging calls and configuration changes (e.g., `set_logfile`) from
 *     multiple threads are all treated as commands that are pushed onto the
 *     queue. The worker thread processes them in the order they were enqueued,
 *     ensuring that log messages and configuration changes are causally ordered.
 *
 * 5.  **Explicit Lifecycle Management & Graceful Shutdown**:
 *     - The Logger is a managed module within the `LifecycleManager` framework,
 *       but it no longer registers itself automatically. The user is responsible
 *       for retrieving its module definition via `Logger::GetLifecycleModule()`
 *       and registering it with a `LifecycleGuard` or `LifecycleManager`.
 *     - The worker thread is only started when `LifecycleManager::initialize()`
 *       is called, not in the `Logger`'s constructor.
 *     - Using the logger before its module is initialized has two behaviors:
 *       a. **Silent Drop**: Calling a logging macro (e.g., `LOGGER_INFO`) will
 *          do nothing and the message will be silently dropped.
 *       b. **Fatal Error**: Calling a configuration method (e.g., `set_level`,
 *          `set_logfile`, `flush`) will immediately call `std::abort()` and
 *          terminate the program with a descriptive error message.
 *     - On shutdown, `pylabhub::lifecycle::FinalizeApp()` ensures that the
 *       logger's shutdown method is called, which gracefully flushes all
 *       pending messages before terminating the worker thread.
 *
 * **Usage**
 *
 * The logger is a singleton, but it must be explicitly initialized via the
 * `LifecycleManager` before use.
 *
 * ```cpp
 * #include "utils/Lifecycle.hpp"
 * #include "utils/Logger.hpp"
 * #include "utils/FileLock.hpp" // Other modules may be needed
 *
 * void my_application_logic() {
 *     int user_id = 123;
 *     LOGGER_INFO("User {} logged in successfully.", user_id);
 * }
 *
 * int main() {
 *     // The user must now explicitly manage the application lifecycle.
 *     // This is typically done by creating a LifecycleGuard in main().
 *     // All required modules must be passed to its constructor.
 *     pylabhub::lifecycle::LifecycleGuard app_lifecycle(
 *         pylabhub::utils::Logger::GetLifecycleModule(),
 *         pylabhub::utils::FileLock::GetLifecycleModule()
 *         // ... other modules ...
 *     );
 *
 *     // It is now safe to use the logger and other utilities.
 *
 *     // Change the log level at runtime.
 *     pylabhub::utils::Logger::instance().set_level(pylabhub::utils::Logger::Level::L_DEBUG);
 *     LOGGER_DEBUG("This is a debug message.");
 *
 *     my_application_logic();
 *
 *     // When `app_lifecycle` goes out of scope at the end of main(), its destructor
 *     // will automatically call `FinalizeApp()`, shutting down all modules
 *     // gracefully.
 *
 *     return 0;
 * }
 * ```
 ******************************************************************************/

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "utils/Lifecycle.hpp" // For ModuleDef
#include "pylabhub_utils_export.h"

// The default initial reserve size for the fmt::memory_buffer used for formatting
#ifndef LOGGER_FMT_BUFFER_RESERVE
#define LOGGER_FMT_BUFFER_RESERVE (1024u)
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251) // Pimpl pattern for ABI safety.
#endif

namespace pylabhub::utils
{

// Forward declare the lifecycle startup function to befriend it.
void do_logger_startup();

class PYLABHUB_UTILS_EXPORT Logger
{
  // Grant access to the startup function so it can start the worker thread.
  friend void do_logger_startup();

  // Forward declaration for the Pimpl pattern.
  struct Impl;

  public:
    /** @brief Defines the severity of a log message. */
    enum class Level : int
    {
        L_TRACE = 0,
        L_DEBUG = 1,
        L_INFO = 2,
        L_WARNING = 3,
        L_ERROR = 4,
        L_SYSTEM = 5, // For critical system-level messages.
    };

    /**
     * @brief Accessor for the singleton instance.
     * @return A reference to the single global Logger instance.
     */
    static Logger &instance();

    /**
     * @brief Returns a ModuleDef for the Logger to be used with the LifecycleManager.
     */
    static ModuleDef GetLifecycleModule();

    /**
     * @brief Checks if the Logger module has been initialized by the LifecycleManager.
     */
    static bool is_initialized() noexcept;



    // --- Lifecycle ---
    // The logger is non-copyable and non-movable to enforce the singleton pattern.
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    ~Logger();

    // --- Sinks / Configuration ---
    // All configuration changes are asynchronous; they are commands that will be
    // executed in order by the worker thread.

    /**
     * @brief Asynchronously switches the logging output to the console (stderr).
     * This is the default sink on startup.
     */
    void set_console();

    /**
     * @brief Asynchronously switches the logging output to a file.
     * @param utf8_path The UTF-8 encoded path to the log file.
     * @param use_flock If true, uses an advisory file lock (`flock`) on POSIX
     *                  systems to protect against concurrent writes from other
     *                  processes. This has no effect on Windows.
     */
    void set_logfile(const std::string &utf8_path, bool use_flock = false);

    /**
     * @brief Asynchronously switches logging to syslog (POSIX only). This is a no-op on Windows.
     * @param ident The identity string passed to `openlog`. Defaults to the program name.
     * @param option The option bitfield for `openlog` (e.g., `LOG_PID`).
     * @param facility The facility code for `openlog` (e.g., `LOG_USER`).
     */
    void set_syslog(const char *ident = nullptr, int option = 0, int facility = 0);

    /**
     * @brief Asynchronously switches logging to the Windows Event Log (Windows only).
     *        This is a no-op on other platforms.
     * @param source_name The event source name. This source must be registered
     *                    with the operating system for messages to appear correctly.
     */
    void set_eventlog(const wchar_t *source_name);

    /**
     * @brief Gracefully shuts down the logger.
     *
     * This function is blocking. It queues a shutdown command and waits until
     * the worker thread has processed all pending messages and terminated. This
     * guarantees that all logs are written before the function returns.
     * @note This is typically called automatically by `pylabhub::lifecycle::FinalizeApp()`.
     */
    void shutdown();

    /**
     * @brief Blocks until the logger has processed all currently queued messages.
     *
     * This is a synchronous call that is useful for ensuring that logs preceding
     * a critical event (like a crash) are written to disk before proceeding.
     */
    void flush();

    // --- Configuration & Diagnostics ---

    /**
     * @brief Sets the minimum level for messages to be processed.
     * Messages with a lower severity will be dropped.
     * @param lvl The new minimum log level.
     */
    void set_level(Level lvl);

    /**
     * @brief Gets the current logging level.
     * @return The current minimum log level.
     */
    Level level() const;

    /**
     * @brief Sets a callback to be invoked upon a sink write error.
     *
     * The callback is executed on a separate, dedicated thread, making it safe
     * to call logger functions from within the callback without causing a deadlock.
     * Any exceptions thrown by the user-provided callback are caught and ignored.
     *
     * @param cb A function taking a const std::string& containing the error message.
     */
    void set_write_error_callback(std::function<void(const std::string &)> cb);

    // --- Compile-Time Formatting API (Header-Only Templates) ---
    // These functions use `fmt::format_string` to validate the format string
    // against the arguments at compile time.

    template <Level lvl, typename... Args>
    void log_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept;

    template <typename... Args>
    void trace_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt<Level::L_TRACE>(fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt<Level::L_DEBUG>(fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt<Level::L_INFO>(fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt<Level::L_WARNING>(fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt<Level::L_ERROR>(fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void system_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt<Level::L_SYSTEM>(fmt_str, std::forward<Args>(args)...);
    }

    // --- Runtime Formatting API (Header-Only Templates) ---
    // These functions use `fmt::runtime` to parse the format string at runtime.
    // This is more flexible but less performant and lacks compile-time checks.

    template <typename... Args>
    void log_fmt_runtime(Level lvl, fmt::string_view fmt_str, Args &&...args) noexcept;

    template <typename... Args>
    void trace_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_TRACE, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_DEBUG, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_INFO, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_WARNING, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_ERROR, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void system_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_SYSTEM, fmt_str, std::forward<Args>(args)...);
    }

  private:
    // Private constructor for singleton pattern.
    Logger();

    // The pointer to the private implementation.
    std::unique_ptr<Impl> pImpl;

    // Internal low-level function that enqueues a pre-formatted message.
    void enqueue_log(Level lvl, fmt::memory_buffer &&body) noexcept;
    void enqueue_log(Level lvl, std::string &&body_str) noexcept;

    // Internal accessor for runtime log level filtering.
    bool should_log(Level lvl) const noexcept;
};

// --- Compile-Time Log Level Filtering ---
// By defining this macro before including Logger.hpp, users can strip log
// messages of a certain severity and below at compile time, resulting in zero
// runtime overhead for those logs.
#ifndef LOGGER_COMPILE_LEVEL
#define LOGGER_COMPILE_LEVEL 0 // 0=Trace, 1=Debug, 2=Info, 3=Warning, 4=Error
#endif

// =============================================================================
// Template & Macro Implementations (Must Be in Header)
// =============================================================================

template <Logger::Level lvl, typename... Args>
void Logger::log_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    // First, check the compile-time level. `if constexpr` ensures that if the
    // condition is false, the entire block is discarded during compilation.
    if constexpr (static_cast<int>(lvl) >= LOGGER_COMPILE_LEVEL)
    {
        // Second, check the runtime level. This is a quick atomic load.
        if (!should_log(lvl))
            return;

        try
        {
            // Format the message into a reusable memory buffer to avoid heap
            // allocations for the formatted string itself.
            fmt::memory_buffer mb;
            mb.reserve(LOGGER_FMT_BUFFER_RESERVE);
            fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);
            enqueue_log(lvl, std::move(mb));
        }
        catch (const std::exception &ex)
        {
            // If formatting fails, enqueue the error message instead.
            enqueue_log(Level::L_ERROR, std::string("[FORMAT ERROR] ") + ex.what());
        }
        catch (...)
        {
            enqueue_log(Level::L_ERROR, "[UNKNOWN FORMAT ERROR]");
        }
    }
}

template <typename... Args>
void Logger::log_fmt_runtime(Level lvl, fmt::string_view fmt_str, Args &&...args) noexcept
{
    // The runtime version cannot use `if constexpr` for compile-time filtering,
    // but we still check the runtime level first.
    if (!should_log(lvl))
        return;

    try
    {
        fmt::memory_buffer mb;
        mb.reserve(LOGGER_FMT_BUFFER_RESERVE);
        fmt::format_to(std::back_inserter(mb), fmt::runtime(fmt_str), std::forward<Args>(args)...);
        enqueue_log(lvl, std::move(mb));
    }
    catch (const std::exception &ex)
    {
        enqueue_log(Level::L_ERROR, std::string("[FORMAT ERROR] ") + ex.what());
    }
    catch (...)
    {
        enqueue_log(Level::L_ERROR, "[UNKNOWN FORMAT ERROR]");
    }
}

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// =============================================================================
// Public Logging Macros
// =============================================================================
// These macros are the primary user-facing API. They automatically capture
// the logger instance and pass the format string and arguments to the correct
// compile-time checked `log_fmt` implementation.

#define LOGGER_TRACE(fmt, ...)                                                                     \
    ::pylabhub::utils::Logger::instance().trace_fmt(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_DEBUG(fmt, ...)                                                                     \
    ::pylabhub::utils::Logger::instance().debug_fmt(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_INFO(fmt, ...)                                                                      \
    ::pylabhub::utils::Logger::instance().info_fmt(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_WARN(fmt, ...)                                                                      \
    ::pylabhub::utils::Logger::instance().warn_fmt(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_ERROR(fmt, ...)                                                                     \
    ::pylabhub::utils::Logger::instance().error_fmt(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_SYSTEM(fmt, ...)                                                                    \
    ::pylabhub::utils::Logger::instance().system_fmt(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)

// Macros for runtime format string checking.
#define LOGGER_TRACE_RT(fmt, ...)                                                                  \
    ::pylabhub::utils::Logger::instance().trace_fmt_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_DEBUG_RT(fmt, ...)                                                                  \
    ::pylabhub::utils::Logger::instance().debug_fmt_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_INFO_RT(fmt, ...)                                                                   \
    ::pylabhub::utils::Logger::instance().info_fmt_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_WARN_RT(fmt, ...)                                                                   \
    ::pylabhub::utils::Logger::instance().warn_fmt_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_ERROR_RT(fmt, ...)                                                                  \
    ::pylabhub::utils::Logger::instance().error_fmt_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOGGER_SYSTEM_RT(fmt, ...)                                                                 \
    ::pylabhub::utils::Logger::instance().system_fmt_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
