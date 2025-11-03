#pragma once

// Use the canonical platform header for PLATFORM_* macros
#include "platform.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <mutex>
#include <cstdint>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <cstdarg>

// Default initial reserve for fmt::memory_buffer used by Logger::log_fmt.
// Increase this if most log lines are large to reduce reallocations.
// Can be overridden by -DLOGGER_FMT_BUFFER_RESERVE=N via compiler flags.
#ifndef LOGGER_FMT_BUFFER_RESERVE
#define LOGGER_FMT_BUFFER_RESERVE 1024u
#endif

#if defined(_WIN32) || defined(_WIN64)

#if defined(LOGGER_EXPORTS)
#define LOGGER_API __declspec(dllexport)
#else
#define LOGGER_API __declspec(dllimport)
#endif

#else // POSIX

#if __GNUC__ >= 4
#define LOGGER_API __attribute__((visibility("default")))
#else
#define LOGGER_API
#endif

#endif

#ifndef LOGGER_COMPILE_LEVEL
#ifdef NDEBUG
#define LOGGER_COMPILE_LEVEL pylabhub::util::Logger::Level::DEBUG
#else
#define LOGGER_COMPILE_LEVEL pylabhub::util::Logger::Level::ERROR
#endif
#endif

namespace pylabhub::util {

class LOGGER_API Logger {
public:
    enum class Level { TRACE = 0, DEBUG, INFO, WARNING, ERROR };

    enum class Destination { CONSOLE, FILE, SYSLOG, EVENTLOG };

    // singleton
    static Logger &instance();
    extern "C" LOGGER_API Logger *get_global_logger();

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
    size_t max_log_line_length() const;

    // --- fmt-based formatting API (header-only template) ---
    // Uses fmt-style format strings, e.g. "value={}".
    template <typename... Args>
    void log_fmt(Level lvl, std::string_view fmt_str, Args &&... args) noexcept;

    // convenience wrappers
    template <typename... Args> void trace_fmt(std::string_view fmt_str, Args &&... args) noexcept {
        log_fmt(Level::TRACE, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void debug_fmt(std::string_view fmt_str, Args &&... args) noexcept {
        log_fmt(Level::DEBUG, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void info_fmt(std::string_view fmt_str, Args &&... args) noexcept {
        log_fmt(Level::INFO, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void warn_fmt(std::string_view fmt_str, Args &&... args) noexcept {
        log_fmt(Level::WARNING, fmt_str, std::forward<Args>(args)...);
    }
    template <typename... Args> void error_fmt(std::string_view fmt_str, Args &&... args) noexcept {
        log_fmt(Level::ERROR, fmt_str, std::forward<Args>(args)...);
    }

    // compatibility printf-style helper (kept minimal)
    void log(const char *fmt, ...);

private:
    // non-template sink (implemented in .cpp) - accepts an already formatted UTF-8 body (no newline)
    void write_formatted(Level lvl, std::string &&body) noexcept;

    struct Impl;
    Impl *pImpl;
};

// Template implementation (must be in header) --------------------------------
template <typename... Args>
void Logger::log_fmt(Level lvl, std::string_view fmt_str, Args &&... args) noexcept {
    // cheap level check first
    if (!pImpl) return;
    if (static_cast<int>(lvl) < pImpl->level.load(std::memory_order_relaxed)) return;

    try {
        fmt::memory_buffer mb;
        mb.reserve(static_cast<size_t>(LOGGER_FMT_BUFFER_RESERVE));
        fmt::format_to(mb, fmt_str, std::forward<Args>(args)...);

        // enforce max length cap and append truncation marker if necessary
        const size_t max_line = pImpl->max_log_line_length.load(std::memory_order_relaxed);
        static constexpr std::string_view trunc_marker = "...[TRUNCATED]";
        size_t cap = (max_line > trunc_marker.size()) ? (max_line - trunc_marker.size()) : 1;

        std::string body;
        if (mb.size() <= cap) {
            body.assign(mb.data(), mb.size());
        } else {
            // naive UTF-8 truncation; later we can make this unicode-safe if needed
            body.assign(mb.data(), cap);
            body.append(trunc_marker);
        }
        write_formatted(lvl, std::move(body));
    } catch (const std::exception &ex) {
        // never throw from logger
        std::string err = std::string("[FORMAT ERROR] ") + ex.what();
        write_formatted(lvl, std::move(err));
    } catch (...) {
        write_formatted(lvl, std::string("[UNKNOWN FORMAT ERROR]"));
    }
}

} // namespace pylabhub::util

// macros for convenience (fmt-style)
#if LOGGER_COMPILE_LEVEL >= 0 // pylabhub::util::Logger::Level::TRACE
#define LOG_TRACE(fmt_str, ...) ::pylabhub::util::Logger::instance().trace_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOG_TRACE(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 1 // pylabhub::util::Logger::Level::DEBUG
#define LOG_DEBUG(fmt_str, ...) ::pylabhub::util::Logger::instance().debug_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVE >= 2 // pylabhub::util::Logger::Level::INFO
#define LOG_INFO(fmt_str, ...)  ::pylabhub::util::Logger::instance().info_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOG_INFO(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 3 // pylabhub::util::Logger::Level::WARNING
#define LOG_WARN(fmt_str, ...)  ::pylabhub::util::Logger::instance().warn_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOG_WARN(fmt_str, ...) ((void)0)
#endif

#if LOGGER_COMPILE_LEVEL >= 4 // pylabhub::util::Logger::Level::ERROR
#define LOG_ERROR(fmt_str, ...) ::pylabhub::util::Logger::instance().error_fmt(fmt_str, ##__VA_ARGS__)
#else
#define LOG_ERROR(fmt_str, ...) ((void)0)
#endif

