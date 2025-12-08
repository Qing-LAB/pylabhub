/*******************************************************************************
  * @file include/platform.hpp
  * @brief Platform detection macros
  * @author Quan Qing
  * @date 2025-11-15
  * Reviewed by Quan Qing on 2025-11-15
  * First version created by Quan Qing with ChatGPT assistance.
 ******************************************************************************/
#pragma once

#include <fmt/ostream.h> // For fmt::print to FILE*
#include <cstdio>  // For fprintf, stderr
#include <cstdlib> // For std::abort

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

// --- Require C++17 or later --------------------------------------------------
// This header (and the codebase) uses C++17 features: inline variables,
// std::scoped_lock, and others. Fail early with a clear message when an
// older language standard is used.
//
// For GCC/Clang use __cplusplus; for MSVC use _MSVC_LANG (MSVC sets
// __cplusplus only when /Zc:__cplusplus is enabled).
#if defined(_MSC_VER)
#if !defined(_MSVC_LANG) || (_MSVC_LANG < 202002L)
#error                                                                                             \
    "This project requires C++20 or later. Please compile with /std:c++20 or newer (MSVC)."
#endif
#else
#if __cplusplus < 202002L
#error "This project requires C++20 or later. Please compile with -std=c++20 or newer."
#endif
#endif

// --- Fatal Error Handling ---------------------------------------------------
#ifndef PANIC
/**
 * @brief Macro for handling fatal, unrecoverable errors.
 *
 * By default, this macro prints a message to stderr and calls `std::abort()`,
 * which terminates the program immediately. Users can override this behavior by
 * defining `PANIC()` before including this header, for example, to integrate
 * with a custom crash reporting framework.
 *
 * Example of overriding PANIC:
 * ```cpp
 * #define PANIC(msg) my_custom_panic_handler(__FILE__, __LINE__, msg)
 * #include "platform.hpp"
 * ```
 */
#define PANIC(msg)                                                                                 \
    do                                                                                             \
    {                                                                                              \
        fmt::print(stderr, "FATAL ERROR: {}\n", msg);                                              \
        std::abort();                                                                              \
    } while (0)
#endif
// ----------------------------------------------------------------------------

// --- Shared Library API Export Macros ---------------------------------------
// When building a shared library, symbols (classes, functions) intended for
// public use must be explicitly marked for export. This macro handles the
// platform-specific syntax.
//
// The build system (CMake) should define PYLABHUB_BUILD_DLL when building this
// project as a shared library.
#if PYLABHUB_IS_WINDOWS
    #if defined(PYLABHUB_BUILD_DLL)
        #define PYLABHUB_API __declspec(dllexport)
    #else
        #define PYLABHUB_API __declspec(dllimport)
    #endif
#else // GCC, Clang, etc.
    #define PYLABHUB_API __attribute__((visibility("default")))
#endif

// Fallback for static builds or when the macro isn't defined.
#ifndef PYLABHUB_API
#define PYLABHUB_API
#endif
// ----------------------------------------------------------------------------
