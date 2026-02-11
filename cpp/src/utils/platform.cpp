/**
 * @file platform.cpp
 * @brief Provides cross-platform implementations for core OS-specific utilities.
 *
 * This file contains the platform-specific logic for functions declared in the
 * `pylabhub::platform` namespace, such as retrieving process and thread IDs,
 * getting the current executable's path, and package version information. It uses
 * preprocessor directives to select the correct implementation for Windows, macOS,
 * Linux, and other POSIX-compliant systems.
 */
#include "plh_base.hpp"
#include "pylabhub_version.h"
#include <chrono>

#if defined(PYLABHUB_PLATFORM_WIN64)

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 5105) // Disable warning about including deprecated header dbghelp.h
#endif

#include <dbghelp.h> // For CaptureStackBackTrace, StackWalk64, SymInitialize

#pragma comment(lib, "dbghelp.lib") // Link with Dbghelp.lib

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif // for WINDOWS PLATFORM

#include <thread>

#if defined(PYLABHUB_IS_POSIX)
#include <sys/syscall.h>
#include <sys/types.h>
#include <cxxabi.h>   // For __cxa_demangle
#include <dlfcn.h>    // For dladdr
#include <execinfo.h> // For backtrace, backtrace_symbols
#include <unistd.h>
#include <errno.h>  // For ESRCH
#include <signal.h> // For kill
#include <fcntl.h>  // For O_CREAT, O_RDWR
#include <sys/mman.h> // For mmap, munmap
#include <sys/stat.h> // For fstat
#endif

#ifdef PYLABHUB_PLATFORM_FREEBSD
#include <sys/sysctl.h>
#endif

#if defined(PYLABHUB_PLATFORM_APPLE)
#include <libproc.h>     // proc_pidpath
#include <limits.h>      // PATH_MAX
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif

#include "fmt/core.h"
#include "fmt/format.h"

namespace pylabhub::platform
{
// Anonymous namespace for helpers local to this translation unit.

/**
 * @brief Gets the current process ID in a cross-platform way.
 * @return The process ID (PID) of the calling process.
 */
uint64_t get_pid()
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

/**
 * @brief Gets a platform-native thread ID.
 * @details This is suitable for logging and debugging, providing a unique
 *          identifier for the current thread. It uses the most efficient OS-specific
 *          API available (`GetCurrentThreadId`, `pthread_threadid_np`, `syscall(SYS_gettid)`).
 * @return A unique 64-bit integer representing the native thread ID.
 */
uint64_t get_native_thread_id() noexcept
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    return static_cast<uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid;
    pthread_threadid_np(nullptr, &tid);
    return tid;
#elif defined(__linux__)
    return static_cast<uint64_t>(syscall(SYS_gettid));
#else
    // Fallback for other POSIX or unknown systems.
    return std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

/**
 * @brief Discovers the name and optionally the full path of the current executable.
 * @details This function is useful for logging, configuration, and finding resources
 *          relative to the application binary. It uses OS-specific APIs like
 *          `GetModuleFileNameW` (Windows), `readlink` on `/proc/self/exe` (Linux),
 *          `_NSGetExecutablePath` (macOS), and `sysctl` (FreeBSD).
 *
 * @param include_path If `true`, returns the full, absolute path to the executable.
 *                     If `false` (default), returns only the filename.
 * @return The name or path of the executable. Returns a platform-specific
 *         "unknown" string on failure.
 */
std::string get_executable_name(bool include_path) noexcept
{
    try
    {
        std::string full_path;
#if defined(PYLABHUB_PLATFORM_WIN64)
        std::vector<wchar_t> buf;
        buf.resize(MAX_PATH);
        DWORD len = 0;
        for (;;)
        {
            len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (len == 0)
            {
                return "unknown_win";
            }
            if (len < buf.size() - 1)
            {
                break;
            }
            buf.resize(buf.size() * 2);
        }
        full_path = pylabhub::format_tools::ws2s(std::wstring(buf.data(), len));

#elif defined(PYLABHUB_PLATFORM_LINUX)
        std::vector<char> buf;
        buf.resize(PATH_MAX);
        ssize_t count = readlink("/proc/self/exe", buf.data(), buf.size());
        if (count == -1)
        {
            return "unknown_linux";
        }
        if (static_cast<size_t>(count) >= buf.size())
        {
            buf.resize(buf.size() * 2);
            count = readlink("/proc/self/exe", buf.data(), buf.size());
            if (count == -1)
                return "unknown_linux";
        }
        full_path.assign(buf.data(), static_cast<size_t>(count));

#elif defined(PYLABHUB_PLATFORM_APPLE)
        uint32_t size = 0;
        if (_NSGetExecutablePath(nullptr, &size) == -1 && size > 0)
        {
            std::vector<char> buf(size);
            if (_NSGetExecutablePath(buf.data(), &size) == 0)
            {
                char resolved[PATH_MAX];
                if (realpath(buf.data(), resolved) != nullptr)
                {
                    full_path = resolved;
                }
                else
                {
                    full_path.assign(buf.data(), size);
                }
            }
        }

        if (full_path.empty())
        {
            char procbuf[PROC_PIDPATHINFO_MAXSIZE];
            int ret = proc_pidpath(getpid(), procbuf, sizeof(procbuf));
            if (ret > 0)
            {
                char resolved[PATH_MAX];
                if (realpath(procbuf, resolved) != nullptr)
                {
                    full_path = resolved;
                }
                else
                {
                    full_path = procbuf;
                }
            }
        }

        if (full_path.empty())
        {
            return "unknown_macos";
        }
#elif defined(PYLABHUB_PLATFORM_FREEBSD)
        // For FreeBSD, use sysctl to get the executable path.
        // There are a few ways, but kern.proc.pathname is generally reliable.
        int mib[4];
        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PATHNAME;
        mib[3] = -1; // Current process

        size_t buffer_size;
        // First call to sysctl to get the required buffer size
        if (sysctl(mib, 4, nullptr, &buffer_size, nullptr, 0) == -1)
        {
            return "unknown_freebsd_sysctl_size_fail";
        }

        std::vector<char> buf(buffer_size);
        if (sysctl(mib, 4, buf.data(), &buffer_size, nullptr, 0) == -1)
        {
            return "unknown_freebsd_sysctl_read_fail";
        }
        full_path.assign(buf.data(), buffer_size - 1); // -1 to remove null terminator
#else
        (void)include_path;
        return "unknown";
#endif

        if (include_path)
        {
            return full_path;
        }
        return std::filesystem::path(full_path).filename().string();
    } // try
    catch (const std::exception &e)
    {
        // std::filesystem operations can throw on invalid paths.
        fmt::print(stderr, "Warning: get_executable_name failed: {}.\n", e.what());
    }
    catch (...)
    {
        fmt::print(stderr, "Warning: get_executable_name failed with unknown exception.\n");
    }
    return "unknown";
}

// --- Version information (from pylabhub_version.h, generated at configure time) ---

int get_version_major() noexcept
{
    return PYLABHUB_VERSION_MAJOR;
}

int get_version_minor() noexcept
{
    return PYLABHUB_VERSION_MINOR;
}

int get_version_rolling() noexcept
{
    return PYLABHUB_VERSION_ROLLING;
}

const char *get_version_string() noexcept
{
    return PYLABHUB_VERSION_STRING;
}

/**
 * @brief Checks if a process with the given PID is currently alive.
 * @details This function uses platform-specific mechanisms to determine
 *          if a process is still running. This is critical for detecting
 *          zombie locks and stale PID-based synchronization primitives.
 *
 * @param pid The process ID to check. PID 0 is always considered invalid.
 * @return True if the process exists, false otherwise.
 *
 * @note Windows Implementation:
 *       - Uses OpenProcess() with PROCESS_QUERY_INFORMATION
 *       - Checks GetExitCodeProcess() for STILL_ACTIVE status
 *       - ERROR_INVALID_PARAMETER indicates non-existent PID
 *
 * @note POSIX Implementation:
 *       - Uses kill(pid, 0) which checks process existence without sending signal
 *       - ESRCH (errno 3) means "No such process" -> dead
 *       - EPERM (errno 1) means "Operation not permitted" -> alive but inaccessible
 *       - Success (0) means process exists and we can signal it
 */
bool is_process_alive(uint64_t pid) noexcept
{
    if (pid == 0)
    {
        // PID 0 is typically invalid or refers to the system/kernel
        return false;
    }

#if defined(PYLABHUB_PLATFORM_WIN64)
    // On Windows, check if process handle can be opened and is still active
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == NULL)
    {
        // GetLastError() could be ERROR_INVALID_PARAMETER if PID doesn't exist
        DWORD error = GetLastError();
        // Process doesn't exist if we get invalid parameter error
        return error != ERROR_INVALID_PARAMETER;
    }

    DWORD exitCode = 0;
    BOOL result = GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);

    if (!result)
    {
        // Could not get exit code, assume dead or inaccessible
        return false;
    }

    // STILL_ACTIVE (259) means process is running
    return exitCode == STILL_ACTIVE;

#else // POSIX systems
    // On POSIX, kill(pid, 0) checks for existence without sending a signal
    // Returns 0 on success (process exists), -1 on failure
    if (kill(static_cast<pid_t>(pid), 0) == 0)
    {
        return true; // Process exists and we can signal it
    }

    // Check errno to distinguish between different failure modes
    // ESRCH (3): No such process -> dead
    // EPERM (1): Operation not permitted -> alive but we lack permissions
    // Other errors: treat as "not alive" for safety
    return errno != ESRCH;
#endif
}

/**
 * @brief Gets a monotonic timestamp in nanoseconds.
 * @details Uses std::chrono::steady_clock for guaranteed monotonicity across
 *          all platforms (C++11). This is crucial for:
 *          - Timeout calculations in SlotRWState acquisition
 *          - Performance metrics in SharedMemoryHeader
 *          - Heartbeat timestamps
 *          - Logger timestamps
 *
 * @return Nanoseconds since an unspecified epoch (typically boot time).
 *         The absolute value is meaningless; use for deltas only.
 *
 * @note Thread-safe. steady_clock is monotonic on Windows, Linux, macOS, FreeBSD.
 */
uint64_t monotonic_time_ns() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

/**
 * @brief Computes elapsed time in nanoseconds since a start timestamp.
 * @details Convenience function for timeout and performance measurement.
 *          Handles potential clock skew by returning 0 if start_ns is in future.
 *
 * @param start_ns A previous timestamp from monotonic_time_ns().
 * @return Nanoseconds elapsed. Returns 0 if start_ns > current time (clock skew).
 *
 * @note Example usage:
 *       uint64_t start = monotonic_time_ns();
 *       // ... do work ...
 *       uint64_t elapsed = elapsed_time_ns(start);
 *       LOGGER_DEBUG("Operation took {} ns", elapsed);
 */
uint64_t elapsed_time_ns(uint64_t start_ns) noexcept
{
    uint64_t now = monotonic_time_ns();
    // Guard against clock skew (should never happen with monotonic clock)
    if (now < start_ns)
    {
        return 0;
    }
    return now - start_ns;
}

// ============================================================================
// Shared Memory
// ============================================================================

#if defined(PYLABHUB_PLATFORM_WIN64)

ShmHandle shm_create(const char *name, size_t size, unsigned flags)
{
    ShmHandle h{};
    if (!name || size == 0)
        return h;
    HANDLE mapping =
        CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                          static_cast<DWORD>(size), name);
    if (mapping == NULL)
        return h;
    if ((flags & SHM_CREATE_EXCLUSIVE) != 0 && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mapping);
        return h;
    }
    void *base = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (base == NULL)
    {
        CloseHandle(mapping);
        return h;
    }
    h.base = base;
    h.size = size;
    h.opaque = mapping;
    return h;
}

ShmHandle shm_attach(const char *name)
{
    ShmHandle h{};
    if (!name)
        return h;
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (mapping == NULL)
        return h;
    void *base = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (base == NULL)
    {
        CloseHandle(mapping);
        return h;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0)
    {
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        return h;
    }
    h.base = base;
    h.size = mbi.RegionSize;
    h.opaque = mapping;
    return h;
}

void shm_close(ShmHandle *handle)
{
    if (!handle)
        return;
    if (handle->base)
    {
        UnmapViewOfFile(handle->base);
        handle->base = nullptr;
    }
    if (handle->opaque)
    {
        CloseHandle(static_cast<HANDLE>(handle->opaque));
        handle->opaque = nullptr;
    }
    handle->size = 0;
}

void shm_unlink(const char * /*name*/)
{
    // Windows: no explicit unlink; name is released when last handle closes.
}

#else // POSIX

ShmHandle shm_create(const char *name, size_t size, unsigned flags)
{
    ShmHandle h{};
    if (!name || size == 0)
        return h;
    if ((flags & SHM_CREATE_UNLINK_FIRST) != 0)
        ::shm_unlink(name);
    int open_flags = O_CREAT | O_RDWR;
    if ((flags & SHM_CREATE_EXCLUSIVE) != 0)
        open_flags |= O_EXCL;
    int fd = shm_open(name, open_flags, 0666);
    if (fd == -1)
        return h;
    if (ftruncate(fd, static_cast<off_t>(size)) == -1)
    {
        close(fd);
        shm_unlink(name);
        return h;
    }
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        close(fd);
        shm_unlink(name);
        return h;
    }
    h.base = base;
    h.size = size;
    h.opaque = reinterpret_cast<void *>(static_cast<intptr_t>(fd));
    return h;
}

ShmHandle shm_attach(const char *name)
{
    ShmHandle h{};
    if (!name)
        return h;
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd == -1)
        return h;
    struct stat st;
    if (fstat(fd, &st) == -1)
    {
        close(fd);
        return h;
    }
    size_t size = static_cast<size_t>(st.st_size);
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        close(fd);
        return h;
    }
    h.base = base;
    h.size = size;
    h.opaque = reinterpret_cast<void *>(static_cast<intptr_t>(fd));
    return h;
}

void shm_close(ShmHandle *handle)
{
    if (!handle)
        return;
    if (handle->base && handle->size > 0)
    {
        munmap(handle->base, handle->size);
        handle->base = nullptr;
    }
    if (handle->opaque)
    {
        close(static_cast<int>(reinterpret_cast<intptr_t>(handle->opaque)));
        handle->opaque = nullptr;
    }
    handle->size = 0;
}

void shm_unlink(const char *name)
{
    if (name)
        ::shm_unlink(name);
}

#endif

} // namespace pylabhub::platform
