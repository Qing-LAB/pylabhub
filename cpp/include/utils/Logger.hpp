/*******************************************************************************
 * @file Logger.hpp
 * @brief High-performance, asynchronous, thread-safe logging utility.
 *
 * **Design Philosophy: Command-Queue Pattern**
 * The Logger is designed for high-throughput applications where logging latency
 * must not impact the performance of application threads. It achieves this through
 * a decoupled, asynchronous architecture:
 *
 * 1.  **Non-Blocking API**: Calls from application threads (e.g., `LOGGER_INFO(...)`)
 *     are lightweight. They create a command object (e.g., a log message or a
 *     control command like changing a sink) and push it into a thread-safe queue.
 *     This is a fast, non-blocking operation.
 * 2.  **Asynchronous Worker Thread**: A single, dedicated background thread
 *     is the **sole consumer** of the command queue. It performs all I/O
 *     operations (writing to console or file) and manages sink lifetimes (e.g.,
 *     opening/closing files). This isolates I/O latency from application logic
 *     and eliminates the need for complex locking around shared resources like
 *     file handles.
 * 3.  **Sink Abstraction**: A `Sink` base class defines a simple interface for
 *     writing and flushing. Concrete implementations (`ConsoleSink`, `FileSink`)
 *     encapsulate the details of each destination. This makes the logger
 *     extensible to other destinations (e.g., network, syslog) in the future.
 * 4.  **Minimal Locking**: API threads only acquire a brief lock to push a
 *     command onto the queue. Since the worker is the only thread that accesses
 *     the active sink, complex lock-ordering deadlocks are avoided.
 * 5.  **Robustness**: The logger supports graceful shutdown, ensuring all buffered
 *     messages are written before the program exits. I/O errors are handled
 *     within the worker and can be reported via a callback, preventing logging
 *     failures from crashing the main application.
 *
 * **Thread Safety**
 * - All public methods are thread-safe.
 * - Logging calls and configuration changes from multiple threads are serialized
 *   into an internal command queue, preserving their order.
 *
 * **Usage**
 * ```cpp
 * // Basic logging
 * #include "utils/Logger.hpp"
 * LOGGER_INFO("User {} logged in", user_id);
 *
 * // Configuration
 * Logger& logger = Logger::instance();
 * logger.set_logfile("/var/log/my_app.log"); // Asynchronously switches to a file
 * logger.set_level(Logger::Level::L_DEBUG);
 *
 * // Graceful shutdown
 * logger.shutdown(); // Blocks until all logs are written
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

#include "pylabhub_utils_export.h"

// Default initial reserve for fmt::memory_buffer used by Logger::log_fmt.
#ifndef LOGGER_FMT_BUFFER_RESERVE
#define LOGGER_FMT_BUFFER_RESERVE (1024u)
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::utils
{

// Forward declaration of the private implementation
struct Impl;

class PYLABHUB_UTILS_EXPORT Logger
{
  public:
    enum class Level : int
    {
        L_TRACE = 0,
        L_DEBUG = 1,
        L_INFO = 2,
        L_WARNING = 3,
        L_ERROR = 4,
        L_SYSTEM = 5,
    };

    // Singleton accessor
    static Logger &instance();

    // Lifecycle
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    ~Logger();

    // --- Sinks / Configuration ---
    // All configuration changes are asynchronous; they are commands that will be
    // executed in order by the worker thread.

    /**
     * @brief Switch logging to the console (stderr). Non-blocking.
     */
    void set_console();

    /**
     * @brief Switch logging to a file. Non-blocking.
     * @param utf8_path Path to the log file.
     * @param use_flock If true, use an advisory file lock during writes (POSIX).
     */
    void set_logfile(const std::string &utf8_path, bool use_flock = false);

    /**
     * @brief Switch logging to syslog (POSIX only). Non-blocking.
     * @param ident The identity string passed to openlog. Defaults to program name.
     * @param option The option bitfield for openlog.
     * @param facility The facility code for openlog.
     */
    void set_syslog(const char *ident = nullptr, int option = 0, int facility = 0);

    /**
     * @brief Switch logging to Windows Event Log (Windows only). Non-blocking.
     * @param source_name The event source name.
     */
    void set_eventlog(const wchar_t *source_name);

    /**
     * @brief Gracefully shuts down the logger.
     *
     * This function queues a shutdown command and blocks until the worker thread
     * has processed all messages and terminated. This guarantees that all logs are
     * written before the function returns.
     */
    void shutdown();

    /**
     * @brief Waits for the logger to process all currently queued messages.
     *
     * This is a synchronous call that blocks until the worker thread has finished
     * writing all messages that were in the queue at the time `flush()` was called.
     */
    void flush();

    // --- Configuration & Diagnostics ---
    void set_level(Level lvl);
    Level level() const;

    /**
     * @brief Sets a callback to be invoked upon a write error.
     *
     * The callback will be executed from the worker thread's context.
     * @param cb A function taking a const std::string& with the error message.
     */
    void set_write_error_callback(std::function<void(const std::string &)> cb);

    // --- Formatting API (header-only templates) ---
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

    // --- Runtime Path ---
    template <typename... Args>
    void log_fmt_runtime(Level lvl, fmt::string_view fmt_str, Args &&...args) noexcept;

    template <typename... Args> void trace_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_TRACE, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void debug_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_DEBUG, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void info_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_INFO, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void warn_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_WARNING, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void error_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_ERROR, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void system_fmt_rt(fmt::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt_runtime(Level::L_SYSTEM, fmt_str, std::forward<Args>(args)...);
    }

  private:
    // private constructor for singleton
    Logger();

    // Pimpl pointer.
    std::unique_ptr<Impl> pImpl;

    // Internal logging function that enqueues a formatted message.
    void enqueue_log(Level lvl, std::string &&body) noexcept;

    // Accessor for runtime log level check
    bool should_log(Level lvl) const noexcept;
};

// --- Compile-Time Log Level ---
#ifndef LOGGER_COMPILE_LEVEL
#define LOGGER_COMPILE_LEVEL 0 // 0=Trace, 1=Debug, 2=Info, 3=Warning, 4=Error
#endif

// ----------------- Template implementation (must be in header) -----------------

template <Logger::Level lvl, typename... Args>
void Logger::log_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    if constexpr (static_cast<int>(lvl) >= LOGGER_COMPILE_LEVEL)
    {
        if (!should_log(lvl))
            return;

        try
        {
            fmt::memory_buffer mb;
            mb.reserve(LOGGER_FMT_BUFFER_RESERVE);
            fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);
            enqueue_log(lvl, std::string(mb.data(), mb.size()));
        }
        catch (const std::exception &ex)
        {
            enqueue_log(lvl, std::string("[FORMAT ERROR] ") + ex.what());
        }
        catch (...)
        {
            enqueue_log(lvl, "[UNKNOWN FORMAT ERROR]");
        }
    }
}

template <typename... Args>
void Logger::log_fmt_runtime(Level lvl, fmt::string_view fmt_str, Args &&...args) noexcept
{
    if (!should_log(lvl))
        return;

    try
    {
        fmt::memory_buffer mb;
        mb.reserve(LOGGER_FMT_BUFFER_RESERVE);
        fmt::format_to(std::back_inserter(mb), fmt::runtime(fmt_str), std::forward<Args>(args)...);
        enqueue_log(lvl, std::string(mb.data(), mb.size()));
    }
    catch (const std::exception &ex)
    {
        enqueue_log(lvl, std::string("[FORMAT ERROR] ") + ex.what());
    }
    catch (...)
    {
        enqueue_log(lvl, "[UNKNOWN FORMAT ERROR]");
    }
}

} // namespace pylabhub::utils

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// --- Macro Implementation ---
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
