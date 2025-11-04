#pragma once

// Use the canonical platform header for PLATFORM_* macros
#include "platform.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <cstdarg>

// Default initial reserve for fmt::memory_buffer used by Logger::log_fmt.
// Increase this if most log lines are large to reduce reallocations.
// Can be overridden by -DLOGGER_FMT_BUFFER_RESERVE=N via compiler flags.
#ifndef LOGGER_FMT_BUFFER_RESERVE
#define LOGGER_FMT_BUFFER_RESERVE 1024u
#endif

#ifndef LOGGER_COMPILE_LEVEL
#ifdef NDEBUG
#define LOGGER_COMPILE_LEVEL 1
// pylabhub::util::Logger::Level::DEBUG
#else
#define LOGGER_COMPILE_LEVEL 4
// pylabhub::util::Logger::Level::ERROR
#endif
#endif

namespace pylabhub::util
{

struct Impl; // forward declaration but details not exposed
class Logger
{
  public:
    enum class Level
    {
        TRACE = 0,
        DEBUG,
        INFO,
        WARNING,
        ERROR
    };

    enum class Destination
    {
        CONSOLE,
        FILE,
        SYSLOG,
        EVENTLOG
    };

    // singleton
    static Logger &instance();

    Logger();
    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    // sinks
    bool init_file(const std::string &path, bool use_flock = false, int mode = 0644);
    void init_syslog(const char *ident = nullptr, int option = 0, int facility = 0);
    bool init_eventlog(const wchar_t *source_name);
    void set_destination(Destination dest);
    void shutdown();

    // config
    void set_level(Level lvl);
    Level level() const;
    void set_fsync_per_write(bool v);
    void set_write_error_callback(std::function<void(const std::string &)> cb);

    // diagnostics
    int last_errno() const;
    int last_write_error_code() const;
    std::string last_write_error_message() const;
    int write_failure_count() const;

    // max length
    void set_max_log_line_length(size_t bytes);
    size_t max_log_line_length() const noexcept;

    // --- fmt-based formatting API (header-only template) ---
    // Uses fmt-style format strings, e.g. "value={}".
    template <typename... Args>
    void log_fmt(Level lvl, std::string_view fmt_str, Args &&...args) noexcept;

    // convenience wrappers
    template <typename... Args> void trace_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::TRACE, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void debug_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::DEBUG, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void info_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::INFO, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void warn_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::WARNING, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void error_fmt(std::string_view fmt_str, Args &&...args) noexcept
    {
        log_fmt(Level::ERROR, fmt_str, std::forward<Args>(args)...);
    }

    // compatibility printf-style helper (kept minimal)
    void log(const char *fmt, ...);

  private:
    // non-template sink (implemented in .cpp) - accepts an already formatted UTF-8 body (no
    // newline)
    void write_formatted(Level lvl, std::string &&body) noexcept;
    struct ImplAccessorLocal;
    Impl *pImpl;
  public:
    // function used by the template
    bool should_log(Level lvl) const noexcept;
};

extern "C" Logger *get_global_logger();

// Template implementation (must be in header) --------------------------------
template <typename... Args>
void Logger::log_fmt(Level lvl, std::string_view fmt_str, Args &&...args) noexcept
{
    // cheap level check first
    if (!pImpl)
        return;
    if (!should_log(lvl))
        return;

    try
    {
        fmt::memory_buffer mb;
        mb.reserve(static_cast<size_t>(LOGGER_FMT_BUFFER_RESERVE));
        fmt::format_to(mb, fmt_str, std::forward<Args>(args)...);

        // enforce max length cap and append truncation marker if necessary
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
            // naive UTF-8 truncation; later we can make this unicode-safe if needed
            body.assign(mb.data(), cap);
            body.append(trunc_marker);
        }
        write_formatted(lvl, std::move(body));
    }
    catch (const std::exception &ex)
    {
        // never throw from logger
        std::string err = std::string("[FORMAT ERROR] ") + ex.what();
        write_formatted(lvl, std::move(err));
    }
    catch (...)
    {
        write_formatted(lvl, std::string("[UNKNOWN FORMAT ERROR]"));
    }
}

} // namespace pylabhub::util

// macros for convenience (fmt-style)
#if LOGGER_COMPILE_LEVEL >= 0
// pylabhub::util::Logger::Level::TRACE
#define LOGGER_TRACE(fmt_str, ...)                                                                 \
    ::pylabhub::util::Logger::instance().trace_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_TRACE(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 1
// pylabhub::util::Logger::Level::DEBUG
#define LOGGER_DEBUG(fmt_str, ...)                                                                 \
    ::pylabhub::util::Logger::instance().debug_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_DEBUG(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVE >= 2
// pylabhub::util::Logger::Level::INFO
#define LOGGER_INFO(fmt_str, ...)                                                                  \
    ::pylabhub::util::Logger::instance().info_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_INFO(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 3
// pylabhub::util::Logger::Level::WARNING
#define LOGGER_WARN(fmt_str, ...)                                                                  \
    ::pylabhub::util::Logger::instance().warn_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_WARN(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 4
// pylabhub::util::Logger::Level::ERROR
#define LOGGER_ERROR(fmt_str, ...)                                                                 \
    ::pylabhub::util::Logger::instance().error_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOGGER_ERROR(fmt_str, ...) ((void)0)
#endif
