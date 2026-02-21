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
#include <cstddef>
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
#undef PYLABHUB_PLATFORM_LINUX

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
#undef PYLABHUB_PLATFORM_LINUX
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

#include <cstddef>

namespace pylabhub::platform
{

// ============================================================================
// Shared Memory (cross-platform abstraction)
// ============================================================================

/**
 * @brief Opaque handle for a mapped shared memory segment.
 * @details Use shm_create() or shm_attach() to obtain; shm_close() to release.
 *          base is the mapped address; size is the segment size in bytes;
 *          opaque holds platform-specific data (do not use directly).
 */
struct ShmHandle
{
    void *base = nullptr;   ///< Mapped address (nullptr if invalid)
    size_t size = 0;        ///< Segment size in bytes
    void *opaque = nullptr; ///< Platform handle (HANDLE on Windows, fd on POSIX)
};

/** Flags for shm_create(). Combine with bitwise OR. */
enum ShmCreateFlags : unsigned
{
    SHM_CREATE_NONE = 0,
    /** Create only if segment does not exist; fail if it exists (POSIX O_EXCL; Windows: check). */
    SHM_CREATE_EXCLUSIVE = 1,
    /** POSIX: unlink name before create (clean slate). Windows: no-op. */
    SHM_CREATE_UNLINK_FIRST = 2,
};

/**
 * @brief Creates a new shared memory segment and maps it.
 * @param name Segment name (system-specific: e.g. "/myname" on POSIX, "Global\\myname" on Windows).
 * @param size Size in bytes.
 * @param flags Optional flags: SHM_CREATE_EXCLUSIVE, SHM_CREATE_UNLINK_FIRST (see enum).
 * @return ShmHandle with base != nullptr on success. On failure, base is nullptr.
 * @note SHM_CREATE_UNLINK_FIRST is POSIX-only; on Windows it has no effect.
 */
PYLABHUB_UTILS_EXPORT ShmHandle shm_create(const char *name, size_t size,
                                           unsigned flags = SHM_CREATE_NONE);

/**
 * @brief Attaches to an existing shared memory segment.
 * @param name Segment name (must match the name used by the creator).
 * @return ShmHandle with base != nullptr on success. Size is populated from the segment.
 */
PYLABHUB_UTILS_EXPORT ShmHandle shm_attach(const char *name);

/**
 * @brief Unmaps and closes a shared memory handle.
 * @param h Handle to close. After return, h->base is invalid.
 */
PYLABHUB_UTILS_EXPORT void shm_close(ShmHandle *h);

/**
 * @brief Removes the shared memory name (POSIX: shm_unlink; Windows: no-op).
 * @details On POSIX, removes the name so new attaches fail; existing mappings remain valid
 *          until unmapped. On Windows, the name is released when the last handle closes.
 * @param name Segment name.
 */
PYLABHUB_UTILS_EXPORT void shm_unlink(const char *name);


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
PYLABHUB_UTILS_EXPORT const char *get_version_string() noexcept;

/**
 * @brief Checks if a process with the given PID is currently alive.
 * @details Uses platform-specific APIs:
 *          - Windows: OpenProcess() + GetExitCodeProcess()
 *          - POSIX: kill(pid, 0) with errno check
 * @param pid The process ID to check.
 * @return True if the process is alive, false otherwise.
 * @note PID 0 always returns false (invalid/system PID).
 * @note On POSIX, EPERM (permission denied) is treated as "alive".
 */
PYLABHUB_UTILS_EXPORT bool is_process_alive(uint64_t pid) noexcept;

/**
 * @brief Gets a monotonic timestamp in nanoseconds.
 * @details Uses std::chrono::high_resolution_clock for maximum precision.
 *          This clock is monotonic (never goes backwards) and suitable for
 *          measuring elapsed time, timeouts, and performance metrics.
 * @return Monotonic timestamp in nanoseconds since an unspecified epoch.
 * @note The absolute value is meaningless; use for computing time deltas only.
 * @note This is the preferred timestamp source for all IPC timeouts and metrics.
 */
PYLABHUB_UTILS_EXPORT uint64_t monotonic_time_ns() noexcept;

/**
 * @brief Computes elapsed time in nanoseconds since a start timestamp.
 * @param start_ns A previous timestamp from monotonic_time_ns().
 * @return Nanoseconds elapsed since start_ns.
 * @note If start_ns is in the future (clock skew), returns 0.
 */
PYLABHUB_UTILS_EXPORT uint64_t elapsed_time_ns(uint64_t start_ns) noexcept;

} // namespace pylabhub::platform
