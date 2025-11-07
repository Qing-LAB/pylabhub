// Logger.hpp
//
// Lightweight, header-only template APIs for formatting (fmt).
// Implementation details (Impl) are hidden in Logger.cpp (pimpl).
//
// Design notes:
//  - Templates must not access Impl directly because Impl is incomplete here.
//  - Template formatting uses fmt::memory_buffer with a configurable reserve macro.
//  - Use should_log() and max_log_line_length() to query Impl-visible state.
//  - write_formatted(...) is a non-template sink implemented in Logger.cpp.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "platform.hpp" // use your canonical platform macros

#if !defined(PLATFORM_WIN64)
#include <syslog.h> // provides LOG_PID, LOG_CONS, LOG_USER, etc.
#endif

// Default initial reserve for fmt::memory_buffer used by Logger::log_fmt.
// Can be overridden by -DLOGGER_FMT_BUFFER_RESERVE=N on the compiler command line.
#ifndef LOGGER_FMT_BUFFER_RESERVE
#define LOGGER_FMT_BUFFER_RESERVE (1024u)
#endif

namespace pylabhub::util
{

struct Impl;
class Logger
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
    ~Logger();

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
    template <typename... Args>
    void log_fmt(Logger::Level lvl, std::string_view fmt_str, const Args &...args) noexcept;

    template <typename... Args> void trace_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Logger::Level::L_TRACE, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void debug_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Logger::Level::L_DEBUG, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void info_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Logger::Level::L_INFO, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void warn_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Logger::Level::L_WARNING, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void error_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Logger::Level::L_ERROR, fmt_str, std::forward<Args>(args)...);
    }

    // Minimal compatibility printf-style helper (kept for legacy sites).
    // Prefer log_fmt with fmt-style format strings.
    void log_printf(const char *fmt, ...) noexcept;

  private:
    // Non-template sink: accepts an already-formatted UTF-8 body (no newline).
    // Implemented in Logger.cpp so it can access Impl.
    void write_formatted(Logger::Level lvl, std::string &&body) noexcept;

    // Internal helper that records a write failure, updates counters and last error message,
    // copies a user callback under lock and invokes it outside the lock. Implemented in .cpp.
    void record_write_error(int errcode, const char *msg) noexcept;

    // Pimpl pointer. Impl is forward declared (below) and defined in Logger.cpp only.
    std::unique_ptr<Impl> pImpl;
};

// ----------------- Template implementation (must be in header) -----------------
template <typename... Args>
void Logger::log_fmt(Logger::Level lvl, std::string_view fmt_str, const Args &...args) noexcept
{
    // Fast path: check level without locking. should_log is implemented in .cpp.
    if (!this->should_log(lvl))
        return;

    try
    {
        fmt::memory_buffer mb;
        mb.reserve(static_cast<size_t>(LOGGER_FMT_BUFFER_RESERVE));
        fmt::format_to(std::back_inserter(mb), fmt::runtime(fmt_str), args...);
        // the following has been tried.
        // fmt::format_to(mb, fmt::runtime(fmt_str), std::forward<const Args>(args)...);
        // fmt::vformat_to(mb, fmt::string_view(fmt_str), fmt::make_format_args(args...));

        // std::string formatted = fmt::vformat(fmt::string_view(fmt_str),
        // fmt::make_format_args(args...)); std::string mb=formatted;
        //  fmt::vformat_to(std::back_inserter(mb),
        //          fmt::string_view(fmt_str),
        //          fmt::make_format_args(args...));

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

} // namespace pylabhub::util

// macros for convenience (fmt-style)
#if LOGGER_COMPILE_LEVEL >= 0
// pylabhub::util::Logger::Level::L_TRACE
#define LOGGER_TRACE(fmt_str, ...)                                                                 \
    ::pylabhub::util::Logger::instance().trace_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_TRACE(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 1
// pylabhub::util::Logger::Level::L_DEBUG
#define LOGGER_DEBUG(fmt_str, ...)                                                                 \
    ::pylabhub::util::Logger::instance().debug_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_DEBUG(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVE >= 2
// pylabhub::util::Logger::Level::L_INFO
#define LOGGER_INFO(fmt_str, ...)                                                                  \
    ::pylabhub::util::Logger::instance().info_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_INFO(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 3
// pylabhub::util::Logger::Level::L_WARNING
#define LOGGER_WARN(fmt_str, ...)                                                                  \
    ::pylabhub::util::Logger::instance().warn_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_WARN(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 4
// pylabhub::util::Logger::Level::L_ERROR
#define LOGGER_ERROR(fmt_str, ...)                                                                 \
    ::pylabhub::util::Logger::instance().error_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_ERROR(fmt_str, ...) ((void)0)
#endif
