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

const char* get_version_string() noexcept
{
    return PYLABHUB_VERSION_STRING;
}

} // namespace pylabhub::platform
