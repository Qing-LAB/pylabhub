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
 * 1.  **Basic Logging**: Use the convenience macros.
 *     ```cpp
 *     #include "utils/Logger.hpp"
 *     ...
 *     LOGGER_INFO("User {} logged in from IP {}", user_id, ip_address);
 *     LOGGER_ERROR("Failed to process request {}: {}", request_id, error_msg);
 *     ```
 *
 * 2.  **Initialization**: Before logging, configure the desired sink.
 *     ```cpp
 *     Logger& logger = Logger::instance();
 *     logger.init_file("/var/log/my_app.log");
 *     logger.set_level(Logger::Level::L_DEBUG);
 *     ```
 *
 * 3.  **Shutdown**: Call `shutdown()` for a graceful exit, ensuring all buffered
 *     messages are written. In many applications, this is handled automatically
 *     when the singleton is destroyed at program exit.
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

struct Impl;
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
    // init_file: open the given UTF-8 path for append. Returns true on success.
    // use_flock: on POSIX enables advisory flock() while writing.
    bool init_file(const std::string &utf8_path, bool use_flock = false, int mode = 0644);

    // init_syslog: POSIX only (no-op on Windows)
    void init_syslog(const char *ident = nullptr, int option = 0, int facility = 0);

    // init_eventlog: Windows only (takes wchar_t* source); returns true on success.
    bool init_eventlog(const wchar_t *source_name);

    void set_destination(Destination dest);
    void shutdown(); // close sinks, release handles

    // ---- Configuration & Diagnostics ----
    void set_level(Level lvl);
    Level level() const;

    void set_fsync_per_write(bool v);
    void set_write_error_callback(std::function<void(const std::string &)> cb);

    int last_errno() const;
    int last_write_error_code() const;
    std::string last_write_error_message() const;
    int write_failure_count() const;

    // Maximum allowed log body length (bytes). Declared noexcept so header templates can call it.
    void set_max_log_line_length(size_t bytes);
    size_t max_log_line_length() const noexcept;

    // Small accessor used by header-only templates. Declared noexcept and defined in .cpp.
    bool should_log(Level lvl) const noexcept;

    // ---- Formatting API (header-only templates) ----
    // Uses fmt-style format strings. These are convenience wrappers that format the message
    // into a UTF-8 buffer and hand it to write_formatted() (non-template sink).
    template <typename... Args> void log_fmt(Level lvl, fmt::format_string<Args...> fmt_str,
                                             Args &&...args) noexcept;

    template <typename... Args>
    void trace_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::L_TRACE, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void debug_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::L_DEBUG, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void info_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::L_INFO, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void warn_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::L_WARNING, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void error_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::L_ERROR, fmt_str, std::forward<Args>(args)...);
    }

    // Minimal compatibility printf-style helper (kept for legacy sites).
    // Prefer log_fmt with fmt-style format strings.
    void log_printf(const char *fmt, ...) noexcept;

  private:
    // Non-template sink: accepts an already-formatted UTF-8 body (no newline).
    // Implemented in Logger.cpp so it can access Impl.
    void write_formatted(Logger::Level lvl, std::string &&body) noexcept;

    // Pimpl pointer. Impl is forward declared (below) and defined in Logger.cpp only.
    std::shared_ptr<Impl> pImpl;
};

// ----------------- Template implementation (must be in header) -----------------
template <typename... Args>
void Logger::log_fmt(Level lvl, fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    // Fast path: check level without locking. should_log is implemented in .cpp.
    if (!this->should_log(lvl))
        return;

    try
    {
        fmt::memory_buffer mb;
        mb.reserve(static_cast<size_t>(LOGGER_FMT_BUFFER_RESERVE));
        // By using fmt::format_string, the format string is checked at compile time.
        // We no longer need the fmt::runtime() wrapper.
        fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);

        // enforce configured cap (max_log_line_length() defined in .cpp)
        const size_t max_line = max_log_line_length();
        static constexpr std::string_view trunc_marker = "...[TRUNCATED]";
        size_t cap = (max_line > trunc_marker.size()) ? (max_line - trunc_marker.size()) : 1;

        std::string body;
        if (mb.size() <= cap)
        {
            body.assign(mb.data(), mb.size());
        }
        else
        {
            // naive byte-level truncation; consider UTF-8-safe truncation if needed.
            body.assign(mb.data(), cap);
            body.append(trunc_marker);
        }

        // hand off to platform sink
        this->write_formatted(lvl, std::move(body));
    }
    catch (const std::exception &ex)
    {
        // never throw from logging template
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
// This uses a standard-compliant "macro-overloading" pattern to handle calls
// with and without variadic arguments. It avoids the non-standard `##__VA_ARGS__`
// extension and its associated pedantic compiler warnings.

// 1. Helper to get the name of the macro to call (_1 for 1 arg, _V for variadic).
#define PYLH_GET_MACRO_OVERLOAD(_1, _2, NAME, ...) NAME

// 2. Generic dispatcher that calls the correct underlying function.
#define PYLH_LOG_DISPATCH(log_function, ...)                                                       \
    PYLH_GET_MACRO_OVERLOAD(__VA_ARGS__, PYLH_LOG_V, PYLH_LOG_1)                                    \
    (log_function, __VA_ARGS__)

// 3. Macro implementation for 1 argument (format string only).
#define PYLH_LOG_1(log_function, fmt_str)                                                          \
    ::pylabhub::utils::Logger::instance().log_function(FMT_STRING(fmt_str))

// 4. Macro implementation for 2+ arguments (format string + variadic args).
#define PYLH_LOG_V(log_function, fmt_str, ...)                                                     \
    ::pylabhub::utils::Logger::instance().log_function(FMT_STRING(fmt_str), __VA_ARGS__)

// macros for convenience (fmt-style)
#if LOGGER_COMPILE_LEVEL >= 0
#define LOGGER_TRACE(...) PYLH_LOG_DISPATCH(trace_fmt, __VA_ARGS__)
#else
#define LOGGER_TRACE(...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 1
#define LOGGER_DEBUG(...) PYLH_LOG_DISPATCH(debug_fmt, __VA_ARGS__)
#else
#define LOGGER_DEBUG(...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 2
#define LOGGER_INFO(...) PYLH_LOG_DISPATCH(info_fmt, __VA_ARGS__)
#else
#define LOGGER_INFO(...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 3
#define LOGGER_WARN(...) PYLH_LOG_DISPATCH(warn_fmt, __VA_ARGS__)
#else
#define LOGGER_WARN(...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 4
#define LOGGER_ERROR(...) PYLH_LOG_DISPATCH(error_fmt, __VA_ARGS__)
#else
#define LOGGER_ERROR(...) ((void)0)
#endif
