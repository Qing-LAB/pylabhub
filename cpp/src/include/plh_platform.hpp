#pragma once
/**
 * @file plh_platform.hpp
 * @brief Layer 0: Platform detection, Windows headers, and platform utility declarations.
 *
 * This is the foundational umbrella for all platform-specific support. Every file that
 * needs platform macros (PYLABHUB_PLATFORM_WIN64, PYLABHUB_IS_POSIX, etc.) or Windows
 * headers should include this. It is self-contained and can be included at any point.
 *
 * Prefer build-system macros (PLATFORM_WIN64, etc.); fall back to compiler predefined macros.
 */
#include <cstdint>
#include <string>

#if defined(PLATFORM_WIN64)

#define PYLABHUB_PLATFORM_WIN64 1
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_LINUX
#undef PYLABHUB_PLATFORM_FREEBSD
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#elif defined(PLATFORM_APPLE)

#define PYLABHUB_PLATFORM_APPLE 1
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_LINUX
#undef PYLABHUB_PLATFORM_FREEBSD

#elif defined(PLATFORM_FREEBSD)

#define PYLABHUB_PLATFORM_FREEBSD 1
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_LINUX

#elif defined(PLATFORM_LINUX)

#define PYLABHUB_PLATFORM_LINUX 1
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_FREEBSD

#elif defined(PLATFORM_UNKNOWN)

#define PYLABHUB_PLATFORM_UNKNOWN 1
#undef PYLABHUB_PLATFORM_FREEBSD
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_WIN64

#else
// Fallback detection
#if defined(_WIN64)
#define PYLABHUB_PLATFORM_WIN64 1
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_LINUX
#undef PYLABHUB_PLATFORM_FREEBSD
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#elif defined(__APPLE__) && defined(__MACH__)
#define PYLABHUB_PLATFORM_APPLE 1
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_LINUX
#undef PYLABHUB_PLATFORM_FREEBSD

#elif defined(__FreeBSD__)
#define PYLABHUB_PLATFORM_FREEBSD 1
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_LINUX

#elif defined(__linux__)
#define PYLABHUB_PLATFORM_LINUX 1
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_FREEBSD

#else
#define PYLABHUB_PLATFORM_UNKNOWN 1
#undef PYLABHUB_PLATFORM_FREEBSD
#undef PYLABHUB_PLATFORM_APPLE
#undef PYLABHUB_PLATFORM_WIN64
#undef PYLABHUB_PLATFORM_WIN64
#endif
#endif

// Convenience booleans for source code usage:
#if defined(PYLABHUB_PLATFORM_WIN64)
#define PYLABHUB_IS_WINDOWS 1
#undef PYLABHUB_IS_POSIX
#elif defined(PYLABHUB_PLATFORM_APPLE) || defined(PYLABHUB_PLATFORM_FREEBSD) ||                    \
    defined(PYLABHUB_PLATFORM_LINUX)
#undef PYLABHUB_IS_WINDOWS
#define PYLABHUB_IS_POSIX 1
#else
#undef PYLABHUB_IS_WINDOWS
#undef PYLABHUB_IS_POSIX
#endif

// --- Require C++20 or later --------------------------------------------------
// This header (and the codebase) uses C++17 features: inline variables,
// std::scoped_lock, and others. Fail early with a clear message when an
// older language standard is used.
// For GCC/Clang use __cplusplus; for MSVC use _MSVC_LANG (MSVC sets // __cplusplus only when
// /Zc:__cplusplus is enabled).

#if defined(_MSC_VER)
#if !defined(_MSVC_LANG) || (_MSVC_LANG < 202002L)
#error "This project requires C++20 or later. Please compile with /std:c++20 or newer (MSVC)."
#endif
#else
#if __cplusplus < 202002L
#error "This project requires C++20 or later. Please compile with -std=c++20 or newer."
#endif
#endif

#include "pylabhub_utils_export.h"

namespace pylabhub::platform
{

/**
 * @brief Gets the native thread ID for the calling thread.
 * @return A 64-bit unsigned integer representing the thread ID.
 */
PYLABHUB_UTILS_EXPORT uint64_t get_native_thread_id() noexcept;
/**
 * @brief Gets the process ID (PID) for the current process.
 * @return A 64-bit unsigned integer representing the process ID.
 */
PYLABHUB_UTILS_EXPORT uint64_t get_pid();
/**
 * @brief Gets the name of the current executable.
 * @param include_path If `true`, returns the full absolute path to the executable.
 *                     If `false` (default), returns only the filename.
 * @return A string containing the name of the executable. Returns "unknown" on failure.
 */
PYLABHUB_UTILS_EXPORT std::string get_executable_name(bool include_path = false) noexcept;

/**
 * @brief Gets the major version number of the pylabhub package.
 * @return The major version (e.g., 0 from 0.1.42).
 */
PYLABHUB_UTILS_EXPORT int get_version_major() noexcept;
/**
 * @brief Gets the minor version number of the pylabhub package.
 * @return The minor version (e.g., 1 from 0.1.42).
 */
PYLABHUB_UTILS_EXPORT int get_version_minor() noexcept;
/**
 * @brief Gets the rolling version number (e.g., from git commit count).
 * @return The rolling version (e.g., 42 from 0.1.42).
 */
PYLABHUB_UTILS_EXPORT int get_version_rolling() noexcept;
/**
 * @brief Gets the full version string (major.minor.rolling).
 * @return A string such as "0.1.42".
 */
PYLABHUB_UTILS_EXPORT const char* get_version_string() noexcept;

} // namespace pylabhub::platform
