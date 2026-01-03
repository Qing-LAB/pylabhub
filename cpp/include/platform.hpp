/*******************************************************************************
 * @file include/platform.hpp
 * @brief Platform detection macros
 * @author Quan Qing
 * @date 2025-11-15
 * Reviewed by Quan Qing on 2025-11-15
 * First version created by Quan Qing with ChatGPT assistance.
 ******************************************************************************/
#pragma once

#include <cstdint>
#include <cstdio>  // stderr, std::fprintf
#include <cstdlib> // std::abort
#include <fmt/core.h>
#include <fmt/format.h>
#include <source_location> // std::source_location
#include <string>
#include <utility> // std::forward

// Prefer the build-system provided macros (PLATFORM_WIN64, PLATFORM_APPLE, PLATFORM_FREEBSD,
// PLATFORM_LINUX, PLATFORM_UNKNOWN). If they are not defined by the build system, fall back to
// compiler predefined macros.

#if defined(PLATFORM_WIN64)
#define PYLABHUB_PLATFORM_WIN64 1
#elif defined(PLATFORM_APPLE)
#define PYLABHUB_PLATFORM_APPLE 1
#elif defined(PLATFORM_FREEBSD)
#define PYLABHUB_PLATFORM_FREEBSD 1
#elif defined(PLATFORM_LINUX)
#define PYLABHUB_PLATFORM_LINUX 1
#elif defined(PLATFORM_UNKNOWN)
#define PYLABHUB_PLATFORM_UNKNOWN 1
#else
// Fallback detection
#if defined(_WIN64) || (defined(_M_X64) || defined(__x86_64__))
#define PYLABHUB_PLATFORM_WIN64 1
#elif defined(__APPLE__) && defined(__MACH__)
#define PYLABHUB_PLATFORM_APPLE 1
#elif defined(__FreeBSD__)
#define PYLABHUB_PLATFORM_FREEBSD 1
#elif defined(__linux__)
#define PYLABHUB_PLATFORM_LINUX 1
#else
#define PYLABHUB_PLATFORM_UNKNOWN 1
#endif
#endif

// Convenience booleans for source code usage:
#if defined(PYLABHUB_PLATFORM_WIN64)
#define PYLABHUB_IS_WINDOWS 1
#define PYLABHUB_IS_POSIX 0
#elif defined(PYLABHUB_PLATFORM_APPLE) || defined(PYLABHUB_PLATFORM_FREEBSD) ||                    \
    defined(PYLABHUB_PLATFORM_LINUX)
#define PYLABHUB_IS_WINDOWS 0
#define PYLABHUB_IS_POSIX 1
#else
#define PYLABHUB_IS_WINDOWS 0
#define PYLABHUB_IS_POSIX 0
#endif

// Optionally, define short aliases without the PYLABHUB_ prefix if you want to keep
// the original names in code, but using the PYLABHUB_ prefix avoids macro collisions.
#if defined(PYLABHUB_PLATFORM_WIN64) && !defined(PLATFORM_WIN64)
#define PLATFORM_WIN64 1
#endif
#if defined(PYLABHUB_PLATFORM_APPLE) && !defined(PLATFORM_APPLE)
#define PLATFORM_APPLE 1
#endif
#if defined(PYLABHUB_PLATFORM_FREEBSD) && !defined(PLATFORM_FREEBSD)
#define PLATFORM_FREEBSD 1
#endif
#if defined(PYLABHUB_PLATFORM_LINUX) && !defined(PLATFORM_LINUX)
#define PLATFORM_LINUX 1
#endif
#if defined(PYLABHUB_PLATFORM_UNKNOWN) && !defined(PLATFORM_UNKNOWN)
#define PLATFORM_UNKNOWN 1
#endif

// --- Require C++20 or later --------------------------------------------------
// This header (and the codebase) uses C++17 features: inline variables,
// std::scoped_lock, and others. Fail early with a clear message when an
// older language standard is used.
//
// For GCC/Clang use __cplusplus; for MSVC use _MSVC_LANG (MSVC sets
// __cplusplus only when /Zc:__cplusplus is enabled).
#if defined(_MSC_VER)
#if !defined(_MSVC_LANG) || (_MSVC_LANG < 202002L)
#error "This project requires C++20 or later. Please compile with /std:c++20 or newer (MSVC)."
#endif
#else
#if __cplusplus < 202002L
#error "This project requires C++20 or later. Please compile with -std=c++20 or newer."
#endif
#endif

namespace pylabhub::platform
{

// --- platform helpers (declare, define elsewhere) ---------------------------
uint64_t get_native_thread_id() noexcept;
long get_pid();
std::string get_executable_name();
void print_stack_trace() noexcept;

// ------------------------ panic (compile-time checked) ----------------------
template <typename... Args>
[[noreturn]] inline void panic(fmt::format_string<Args...> fmt_str, Args &&...args,
                               std::source_location loc = std::source_location::current())
{
    // Print a fatal error and abort. No stack trace here by design.
    fmt::print(stderr, "FATAL ERROR: ");
    fmt::print(stderr, fmt_str, std::forward<Args>(args)...);
    fmt::print(stderr, " in {} at line {}\n", loc.file_name(), loc.line());
    std::abort();
}

// ------------------------ debug_msg (compile-time checked) -----------------
// Fast path: compile-time checked format string, no exceptions.
template <typename... Args>
inline void debug_msg(fmt::format_string<Args...> fmt_str, Args &&...args,
                      std::source_location loc = std::source_location::current())
{
    fmt::print(stderr, "DEBUG MESSAGE: ");
    fmt::print(stderr, fmt_str, std::forward<Args>(args)...);
    fmt::print(stderr, " in {} at line {}\n", loc.file_name(), loc.line());
}

// ------------------------ debug_msg_rt (runtime format, safe) ---------------
// Runtime path: accepts runtime format strings (e.g. from config). Never throws.
template <typename... Args>
inline void debug_msg_rt(std::string_view fmt_str, Args &&...args,
                         std::source_location loc = std::source_location::current()) noexcept
{
    try
    {
        fmt::print(stderr, "DEBUG MESSAGE: ");
        fmt::vprint(stderr, fmt_str, fmt::make_format_args(std::forward<Args>(args)...));
        fmt::print(stderr, " in {} at line {}\n", loc.file_name(), loc.line());
    }
    catch (const fmt::format_error &e)
    {
        fmt::print(stderr, "DEBUG MESSAGE FORMAT ERROR: '{}' ({}) in {} at line {}\n", fmt_str,
                   e.what(), loc.file_name(), loc.line());
    }
    catch (...)
    {
        fmt::print(stderr, "DEBUG MESSAGE ERROR: '{}' (unknown) in {} at line {}\n", fmt_str,
                   loc.file_name(), loc.line());
    }
}

} // namespace pylabhub::platform

// ---------------------------------------------------------------------------
// Default PANIC macro (callers may define their own PANIC before including this)
#ifndef PANIC
#define PANIC(...) pylabhub::platform::panic(__VA_ARGS__)
#endif
