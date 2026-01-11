#pragma once
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
#if defined(_WIN64)
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

#include <cstdint>
#include <string>

namespace pylabhub::platform
{

// keep your other forward declarations...
uint64_t get_native_thread_id() noexcept;
uint64_t get_pid();
std::string get_executable_name(bool include_path = false) noexcept;

} // namespace pylabhub::platform
