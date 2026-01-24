/**
 * @file debug_info.hpp
 * @brief Provides cross-platform debugging utilities, including stack trace printing,
 *        panic handling for fatal errors, and debug messaging.
 *
 * This header defines a set of functions and macros within the `pylabhub::debug`
 * namespace designed for robust error reporting and debugging. It leverages `fmt`
 * for compile-time format string checks and `std::source_location` for automatic
 * source code location reporting.
 */

// -- Debugging utilities: stack trace printing, panic and debug messages
#pragma once

#include <fmt/format.h>           // for fmt::format_string, fmt::print, fmt::format
#include <source_location>        // for std::source_location
#include <string_view>            // for std::string_view
#include <string>                 // for std::string
#include <cstdio>                 // for fflush
#include "utils/format_tools.hpp" // for pylabhub::format_tools::filename_only

namespace pylabhub::debug
{

/**
 * @brief Prints the current call stack (stack trace) to `stderr`.
 *
 * This function provides a platform-specific implementation to capture and print
 * the program's call stack. On Windows, it uses `CaptureStackBackTrace` and `DbgHelp`
 * functions to resolve symbols and line numbers. On POSIX systems, it uses
 * `backtrace`, `backtrace_symbols`, `dladdr`, and `addr2line` (if available)
 * to provide detailed stack information.
 *
 * Errors during stack trace capture or symbol resolution are reported to `stderr`.
 */
PYLABHUB_UTILS_EXPORT void print_stack_trace() noexcept;

/**
 * @brief Halts program execution with a fatal error message and prints a stack trace.
 *
 * This function is intended for unrecoverable errors. It formats and prints an
 * error message to `stderr`, along with the source location where `panic` was called.
 * It then calls `print_stack_trace()` and `std::abort()`, ensuring the program terminates
 * immediately.
 *
 * The format string is checked at compile-time using `fmt::format_string`.
 * Exception handling is included to prevent further issues if formatting itself fails.
 *
 * @tparam Args Variadic template arguments for the format string.
 * @param loc The source location (file, line, function) where `panic` was called.
 *            Automatically captured by `PLH_HERE` or `std::source_location::current()`.
 * @param fmt_str The `fmt`-style format string for the error message.
 * @param args The arguments to be formatted into `fmt_str`.
 * @noreturn This function never returns.
 */
template <typename... Args>
[[noreturn]] inline void panic(std::source_location loc, fmt::format_string<Args...> fmt_str,
                               Args &&...args) noexcept
{
    try
    {
        const auto body = fmt::format(fmt_str, std::forward<Args>(args)...);
        fmt::print(stderr, "[PANIC] {} -- {}\n", SRCLOC_TO_STR(loc), body);
    }
    catch (const fmt::format_error &e)
    {
        fmt::print(stderr,
                   "[PANIC] {} -- FATAL FORMAT ERROR WHEN PANIC fmt_str['{}']\n"
                   "[PANIC]  Exception: '{}'\n",
                   SRCLOC_TO_STR(loc), fmt_str.get(), e.what());
        std::fflush(stderr);
    }
    catch (...)
    {
        fmt::print(stderr, "[PANIC] {} -- FATAL UNKNOWN EXCEPTION DURING PANIC: fmt_str['{}']\n",
                   SRCLOC_TO_STR(loc), fmt_str.get());
        std::fflush(stderr);
    }
    print_stack_trace();
    std::abort();
}

/**
 * @brief Prints a debug message to `stderr` with compile-time format string checking.
 *
 * This function formats and prints a debug message to `stderr`, including the
 * source location where `debug_msg` was called. It uses `fmt` for efficient and
 * type-safe formatting, with the format string checked at compile-time.
 *
 * This function is intended for general debugging output that can be easily
 * enabled/disabled or filtered.
 *
 * @tparam Args Variadic template arguments for the format string.
 * @param loc The source location (file, line, function) where `debug_msg` was called.
 *            Automatically captured by `PLH_HERE` or `std::source_location::current()`.
 * @param fmt_str The `fmt`-style format string for the debug message.
 * @param args The arguments to be formatted into `fmt_str`.
 */
template <typename... Args>
inline void debug_msg(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    try
    {
        const auto body = fmt::format(fmt_str, std::forward<Args>(args)...);
        fmt::print(stderr, "[DBG]  {}\n", body);
    }
    catch (const fmt::format_error &e)
    {
        fmt::print(stderr,
                   "[DBG]  FATAL FORMAT ERROR DURING DEBUG_MSG: fmt_str['{}']\n"
                   "[DBG]  Exception: '{}'\n",
                   fmt_str.get(), e.what());
        std::fflush(stderr);
    }
    catch (...)
    {
        fmt::print(stderr, "[DBG]  FATAL EXCEPTION DURING DEBUG_MSG: fmt_str['{}']\n",
                   fmt_str.get());
        std::fflush(stderr);
    }
}

// debug_msg_rt: runtime format string, take args by const& so make_format_args binds
/**
 * @brief Prints a debug message to `stderr` using a runtime-determined format string.
 *
 * This function provides similar functionality to `debug_msg` but accepts a `std::string_view`
 * for the format string, allowing the format to be determined at runtime.
 * This comes at the cost of compile-time format string validation.
 *
 * The arguments are passed by `const&` to ensure `fmt::make_format_args` can bind
 * them correctly.
 *
 * @tparam Args Variadic template arguments for the format string.
 * @param loc The source location (file, line, function) where `debug_msg_rt` was called.
 *            Automatically captured by `PLH_HERE` or `std::source_location::current()`.
 * @param fmt_str A `std::string_view` representing the `fmt`-style format string.
 * @param args The arguments to be formatted into `fmt_str`.
 */
template <typename... Args>
inline void debug_msg_rt(std::string_view fmt_str, const Args &...args) noexcept
{
    try
    {
        fmt::print(stderr, "[DBG]  ");
        fmt::vprint(stderr, fmt_str, fmt::make_format_args(args...));
        fmt::print(stderr, "\n");
    }
    catch (const fmt::format_error &e)
    {
        fmt::print(stderr,
                   "[DBG]  FATAL FORMAT ERROR DURING DEBUG_MSG_RT: fmt_str['{}']\n"
                   "[DBG]  Exception: '{}'\n",
                   fmt_str, e.what());
        std::fflush(stderr);
    }
    catch (...)
    {
        fmt::print(stderr, "[DBG]  FATAL UNKNOWN EXCEPTION DURING DEBUG_MSG_RT: fmt_str['{}']\n",
                   fmt_str);
        std::fflush(stderr);
    }
}

} // namespace pylabhub::debug

// ---------------- thin macros for convenience --------------
/**
 * @brief Macro to capture the current source location.
 * @details Expands to `std::source_location::current()`.
 */
inline std::string SRCLOC_TO_STR(std::source_location loc)
{
    return fmt::format("{}:{}:{}", pylabhub::format_tools::filename_only(loc.file_name()),
                       loc.line(), loc.function_name());
}

#ifndef PLH_LOC_HERE_STR
#define PLH_LOC_HERE_STR (SRCLOC_TO_STR(std::source_location::current()))
#endif

/**
 * @brief Macro for calling `pylabhub::debug::panic` with automatic source location.
 * @details This macro provides a convenient way to trigger a fatal error with
 *          a compile-time checked format string.
 * @param fmt The `fmt`-style format string literal.
 * @param ... Variable arguments to be formatted into `fmt`.
 * @see pylabhub::debug::panic
 */
#ifndef PLH_PANIC
#define PLH_PANIC(fmt, ...)                                                                        \
    ::pylabhub::debug::panic(std::source_location::current(), FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#endif

/**
 * @brief Macro for calling `pylabhub::debug::debug_msg` with automatic source location.
 * @details This macro provides a convenient way to print debug messages with
 *          a compile-time checked format string.
 * @param fmt The `fmt`-style format string literal.
 * @param ... Variable arguments to be formatted into `fmt`.
 * @see pylabhub::debug::debug_msg
 */
#ifndef PLH_DEBUG
#if defined(PYLABHUB_ENABLE_DEBUG_MESSAGES)
#define PLH_DEBUG(fmt, ...) ::pylabhub::debug::debug_msg(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#else
#define PLH_DEBUG(fmt, ...)                                                                        \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#endif
#endif

/**
 * @brief Macro for calling `pylabhub::debug::debug_msg_rt` with automatic source location.
 * @details This macro provides a convenient way to print debug messages where the
 *          format string is determined at runtime.
 * @param fmt The runtime format string (e.g., `std::string_view` or `const char*`).
 * @param ... Variable arguments to be formatted into `fmt`.
 * @see pylabhub::debug::debug_msg_rt
 */
#ifndef PLH_DEBUG_RT
#if defined(PYLABHUB_ENABLE_DEBUG_MESSAGES)
#define PLH_DEBUG_RT(fmt, ...) ::pylabhub::debug::debug_msg_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define PLH_DEBUG_RT(fmt, ...)                                                                     \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#endif
#endif