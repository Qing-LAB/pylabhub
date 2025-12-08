/*******************************************************************************
 * @file Logger.hpp
 * @brief High-performance, asynchronous, thread-safe logging utility.
 *
 * **Design Philosophy**
 * The Logger is designed for high-throughput applications where logging latency
 * must not impact the performance of application threads. It achieves this through
 * a decoupled, asynchronous architecture:
 *
 * 1.  **Non-Blocking API**: Calls from application threads (e.g., `LOGGER_INFO(...)`)
 *     are lightweight. They perform `fmt`-style string formatting and push the
 *     resulting message into a thread-safe queue. This is a fast, non-blocking
 *     operation.
 * 2.  **Asynchronous Worker Thread**: A single, dedicated background thread
 *     continuously pulls messages from the queue and performs all I/O operations
 *     (writing to console, file, syslog, etc.). This isolates I/O latency from
 *     the application logic.
 * 3.  **Singleton with Shared Pimpl**: The Logger is a singleton (`Logger::instance()`)
 *     that uses a `std::shared_ptr` to a private implementation (`Impl`). This
 *     ensures that even if the `Logger` class is instantiated across different
 *     modules (e.g., in a main executable and a plugin), they all share the same
 *     underlying logging engine, queue, and worker thread. `std::call_once`
 *     guarantees thread-safe initialization of this singleton `Impl`.
 * 4.  **Header/Source Separation**:
 *     - `Logger.hpp`: Contains the public API and header-only templates for
 *       efficient message formatting (`log_fmt`).
 *     - `Logger.cpp`: Contains the private implementation (`Impl`), the worker
 *       thread logic, and all platform-specific I/O code.
 * 5.  **Robustness**: Logging operations are guaranteed `noexcept`. Any exceptions
 *     during formatting or I/O are caught internally and reported as logging
 *     errors, ensuring that a logging failure can never crash the application.
 *
 * **Thread Safety**
 * - All public methods are thread-safe.
 * - Logging calls from multiple threads are serialized into the internal queue.
 * - Configuration methods (`set_level`, `init_file`, etc.) use mutexes to protect
 *   the logger's state from concurrent modification.
 *
 * **Usage**
 * 1.  **Basic Logging (Compile-Time Format String)**: Use the standard macros
 *     for logging string literals. This is the most common, safe, and performant method.
 *     ```cpp
 *     #include "utils/Logger.hpp"
 *     ...
 *     LOGGER_INFO("User {} logged in from IP {}", user_id, ip_address);
 *     LOGGER_ERROR("Failed to process request {}: {}", request_id, error_msg);
 *     ```
 *
 * 2.  **Logging with Runtime Format Strings**: For cases where the format string
 *     is a variable, use the `_RT` suffixed macros.
 *     ```cpp
 *     std::string format_from_config = get_format_for_event("login");
 *     LOGGER_INFO_RT(format_from_config, user_id, ip_address);
 *     ```
 *
 * 3.  **Initialization**: Before logging, configure the desired sink.
 *     ```cpp
 *     Logger& logger = Logger::instance();
 *     logger.set_logfile("/var/log/my_app.log");
 *     logger.set_level(Logger::Level::L_DEBUG);
 *     ```
 *
 * 4.  **Shutdown**: Call `shutdown()` for a graceful, deterministic exit, ensuring
 *     all buffered messages are written before the function returns. If not
 *     called explicitly, shutdown will happen automatically at program exit.
 *     ```cpp
 *     Logger::instance().shutdown();
 *     ```
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

#if !defined(PLATFORM_WIN64)
#include <syslog.h> // provides LOG_PID, LOG_CONS, LOG_USER, etc.
#endif

// Include project-specific headers last to ensure they have the final say on macros.
#include "platform.hpp" // use your canonical platform macros

// Default initial reserve for fmt::memory_buffer used by Logger::log_fmt.
// Can be overridden by -DLOGGER_FMT_BUFFER_RESERVE=N on the compiler command line.
#ifndef LOGGER_FMT_BUFFER_RESERVE
#define LOGGER_FMT_BUFFER_RESERVE (1024u)
#endif

namespace pylabhub::utils
{

// The struct Impl is forward declaration to implement a Pimpl pattern.
// The actual definition is in Logger.cpp.
// This keeps implementation details out of the public header.
// The Pimpl is managed by a std::shared_ptr in the Logger class.
// This will ensure only one instance of the logging engine exists by using
// std::call_once in get_impl_instance().
// This also avoids C++ ABI issues across shared library boundaries.
struct Impl;

// The Logger class provides a high-performance, asynchronous, thread-safe logging API.
class PYLABHUB_API Logger
{
  public:
    enum class Level : int
    {
        L_TRACE = 0,
        L_DEBUG = 1,
        L_INFO = 2,
        L_WARNING = 3,
        L_ERROR = 4,
    };

    enum class Destination
    {
        L_CONSOLE,
        L_FILE,
        L_SYSLOG,
        L_EVENTLOG,
    };

    // Singleton accessor
    static Logger &instance();

    // Lifecycle - defined in Logger.cpp because Impl is incomplete here
    Logger();
    ~Logger(); // Now non-trivial, must be defined in .cpp

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger(Logger &&) = delete;
    Logger &operator=(Logger &&) = delete;

    // ---- Sinks / initializers (defined in .cpp) ----
    // set_logfile: open the given UTF-8 path for append. Returns true on success.
    // use_flock: on POSIX enables advisory flock() while writing.
    bool set_logfile(const std::string &utf8_path, bool use_flock = false, int mode = 0644);

    // set_syslog: POSIX only (no-op on Windows)
    void set_syslog(const char *ident = nullptr, int option = 0, int facility = 0);

    // set_eventlog: Windows only (takes wchar_t* source); returns true on success.
    bool set_eventlog(const wchar_t *source_name);

    /**
     * @brief Performs a final, synchronous shutdown of the logger.
     * 
     * This function logs a shutdown message, flushes all pending messages to the
     * current sink, then permanently stops the background worker thread. Any
     * subsequent calls to the logger will be ignored.
     * 
     * @note This is an advanced feature for deterministic cleanup. Most applications
     * can rely on the automatic shutdown that occurs when the program exits. It is
     * primarily useful in scenarios like:
     * - Unit tests that need to verify log contents before exiting.
     * - Daemon processes that need to release file handles without terminating.
     * - Programs that need to ensure logging is complete before calling `exec`.
     */
    void shutdown();

    /**
     * @brief Waits for the logger to process all currently queued messages.
     * 
     * This is a synchronous call that blocks until the worker thread has finished
     * writing all messages that were in the queue at the time `flush()` was called.
     */
    void flush() noexcept;

    // ---- Formatting API (header-only templates) ----
    // The logger provides two distinct APIs for logging:
    // 1. Compile-Time: `..._fmt` functions for string literals, offering maximum
    //    performance and safety via compile-time format string validation.
    // 2. Run-Time: `..._fmt_rt` functions for string variables, offering flexibility.

    // --- Compile-Time Path ---
    template <Level level, typename... Args>
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

    // --- Runtime Path ---
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

  private:
    void set_destination(Destination dest);
    // Non-template sink: accepts an already-formatted UTF-8 body (no newline).
    // Implemented in Logger.cpp so it can access Impl.
    void write_formatted(Logger::Level lvl, std::string &&body) noexcept;

    // Pimpl pointer. Impl is forward declared (below) and defined in Logger.cpp only.
    std::shared_ptr<Impl> pImpl;
};

// --- Compile-Time Log Level ---
// Sets the minimum log level to be compiled into the binary.
// This is controlled by the build system, e.g., by passing -DLOGGER_COMPILE_LEVEL=1
// 0=Trace, 1=Debug, 2=Info, 3=Warning, 4=Error
#ifndef LOGGER_COMPILE_LEVEL
#define LOGGER_COMPILE_LEVEL 0
#endif

// ----------------- Template implementation (must be in header) -----------------

// --- COMPILE-TIME PATH IMPLEMENTATION ---
template <Logger::Level level, typename... Args>
void Logger::log_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    // This entire block is removed by the compiler if the log level is below LOGGER_COMPILE_LEVEL.
    if constexpr (static_cast<int>(level) >= LOGGER_COMPILE_LEVEL)
    {
        // After the compile-time check, do a runtime check.
        if (!this->should_log(level))
            return;

        try
        {
            fmt::memory_buffer mb;
            mb.reserve(static_cast<size_t>(LOGGER_FMT_BUFFER_RESERVE));
            fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);

            const size_t max_line = max_log_line_length();
            static constexpr std::string_view trunc_marker = "...[TRUNCATED]";
            size_t cap = (max_line > trunc_marker.size()) ? (max_line - trunc_marker.size()) : 1;

            std::string body;
            if (mb.size() <= cap) {
                body.assign(mb.data(), mb.size());
            } else {
                body.assign(mb.data(), cap);
                body.append(trunc_marker);
            }
            this->write_formatted(level, std::move(body));
        }
        catch (const std::exception &ex)
        {
            std::string err = std::string("[FORMAT ERROR] ") + ex.what();
            this->write_formatted(level, std::move(err));
        }
        catch (...)
        {
            this->write_formatted(level, std::string("[UNKNOWN FORMAT ERROR]"));
        }
    }
}

// --- RUNTIME PATH IMPLEMENTATION ---
template <typename... Args>
void Logger::log_fmt_runtime(Level lvl, fmt::string_view fmt_str, Args &&...args) noexcept
{
    // No compile-time check is possible here, but we still check the runtime level.
    if (!this->should_log(lvl))
        return;

    try
    {
        fmt::memory_buffer mb;
        mb.reserve(static_cast<size_t>(LOGGER_FMT_BUFFER_RESERVE));
        // Use fmt::runtime() as the format string is not known at compile time.
        fmt::format_to(std::back_inserter(mb), fmt::runtime(fmt_str), std::forward<Args>(args)...);

        const size_t max_line = max_log_line_length();
        static constexpr std::string_view trunc_marker = "...[TRUNCATED]";
        size_t cap = (max_line > trunc_marker.size()) ? (max_line - trunc_marker.size()) : 1;

        std::string body;
        if (mb.size() <= cap) {
            body.assign(mb.data(), mb.size());
        } else {
            body.assign(mb.data(), cap);
            body.append(trunc_marker);
        }
        this->write_formatted(lvl, std::move(body));
    }
    catch (const std::exception &ex)
    {
        std::string err = std::string("[FORMAT ERROR] ") + ex.what();
        this->write_formatted(lvl, std::move(err));
    }
    catch (...)
    {
        this->write_formatted(lvl, std::string("[UNKNOWN FORMAT ERROR]"));
    }
}

} // namespace pylabhub::utils

// --- Macro Implementation ---
// The logger provides two sets of macros for different use cases.

// 1. Standard Macros (e.g., LOGGER_INFO): For compile-time constant format strings.
//    This is the recommended macro for 99% of use cases, providing maximum
//    performance and compile-time format string validation.
#define LOGGER_TRACE(fmt, ...) ::pylabhub::utils::Logger::instance().trace_fmt(FMT_STRING(fmt) __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_DEBUG(fmt, ...) ::pylabhub::utils::Logger::instance().debug_fmt(FMT_STRING(fmt) __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_INFO(fmt, ...)  ::pylabhub::utils::Logger::instance().info_fmt(FMT_STRING(fmt) __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_WARN(fmt, ...)  ::pylabhub::utils::Logger::instance().warn_fmt(FMT_STRING(fmt) __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_ERROR(fmt, ...) ::pylabhub::utils::Logger::instance().error_fmt(FMT_STRING(fmt) __VA_OPT__(,) __VA_ARGS__)

// 2. Runtime Macros (e.g., LOGGER_INFO_RT): For format strings held in variables.
//    Use this when the format string is not a compile-time constant. These calls
//    are not checked at compile time and will fall back to the slower runtime formatter.
#define LOGGER_TRACE_RT(fmt, ...) ::pylabhub::utils::Logger::instance().trace_fmt_rt(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_DEBUG_RT(fmt, ...) ::pylabhub::utils::Logger::instance().debug_fmt_rt(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_INFO_RT(fmt, ...)  ::pylabhub::utils::Logger::instance().info_fmt_rt(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_WARN_RT(fmt, ...)  ::pylabhub::utils::Logger::instance().warn_fmt_rt(fmt __VA_OPT__(,) __VA_ARGS__)
#define LOGGER_ERROR_RT(fmt, ...) ::pylabhub::utils::Logger::instance().error_fmt_rt(fmt __VA_OPT__(,) __VA_ARGS__)
