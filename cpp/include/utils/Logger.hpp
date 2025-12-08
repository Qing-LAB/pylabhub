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
    void flush() noexcept;

    // ---- Configuration & Diagnostics ----
    void set_level(Level lvl);
    Level level() const;
    bool dirty() const noexcept;

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
    // This API uses compile-time format string checking for performance and safety.
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

  private:
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
template <Logger::Level level, typename... Args>
void Logger::log_fmt(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    if constexpr (static_cast<int>(level) >= LOGGER_COMPILE_LEVEL)
    {
        // After the compile-time check, do a runtime check.
        // This allows dynamically changing the log level (e.g., in a config file)
        // for levels that have been compiled in.
        if (!this->should_log(level))
            return;

        try
        {
            fmt::memory_buffer mb;
            mb.reserve(static_cast<size_t>(LOGGER_FMT_BUFFER_RESERVE));

            // Use the compile-time checked format string directly. This is faster
            // and safer than fmt::runtime().
            fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);

            // Enforce configured cap (max_log_line_length() is defined in .cpp)
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
                // Naive byte-level truncation.
                body.assign(mb.data(), cap);
                body.append(trunc_marker);
            }
#if defined(_LOGGER_DEBUG_ENABLED)
            fmt::print(stdout, "log_fmt called: level={} body='{}'\n", static_cast<int>(level), body);
            fflush(stdout); // Ensure it's printed immediately
#endif
            // Hand off to the asynchronous worker thread.
            this->write_formatted(level, std::move(body));
        }
        catch (const std::exception &ex)
        {
            // Never throw from logging. Report format errors as a log message.
            std::string err = std::string("[FORMAT ERROR] ") + ex.what();
            this->write_formatted(level, std::move(err));
        }
        catch (...)
        {
            this->write_formatted(level, std::string("[UNKNOWN FORMAT ERROR]"));
        }
    }
}

} // namespace pylabhub::utils

// --- Macro Implementation ---
// These convenience macros are the recommended way to use the logger.
// The compile-time filtering is handled by 'if constexpr' inside the
// log_fmt template. These macros resolve directly to the member function
// allowing for both calls with and without variadic arguments in a C++ standard way.
#define LOGGER_TRACE ::pylabhub::utils::Logger::instance().trace_fmt
#define LOGGER_DEBUG ::pylabhub::utils::Logger::instance().debug_fmt
#define LOGGER_INFO  ::pylabhub::utils::Logger::instance().info_fmt
#define LOGGER_WARN  ::pylabhub::utils::Logger::instance().warn_fmt
#define LOGGER_ERROR ::pylabhub::utils::Logger::instance().error_fmt
