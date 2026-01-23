#include "plh_base.hpp"

#if defined(PYLABHUB_PLATFORM_WIN64)
#if defined(MSVC_VER)
#pragma warning(push)
#pragma warning(disable : 5105) // Disable warning about including deprecated header dbghelp.h
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h> // For CaptureStackBackTrace, StackWalk64, SymInitialize

#pragma comment(lib, "dbghelp.lib") // Link with Dbghelp.lib

#if defined(MSVC_VER)
#pragma warning(pop)
#endif

#else
#include <sys/syscall.h>
#include <sys/types.h>

#if defined(PYLABHUB_IS_POSIX)
#include <cxxabi.h>   // For __cxa_demangle
#include <dlfcn.h>    // For dladdr
#include <execinfo.h> // For backtrace, backtrace_symbols
#include <unistd.h>
#endif
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

// Helper to get the current process ID in a cross-platform way.
uint64_t get_pid()
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

// Gets a platform-native thread ID for logging.
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

// Helper to automatically discover the current executable's name for logging.
std::string get_executable_name(bool include_path) noexcept
{
    try
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        // Loop to handle long paths: start with MAX_PATH and grow if truncated.
        std::vector<wchar_t> buf;
        buf.resize(MAX_PATH);
        DWORD len = 0;
        for (;;)
        {
            len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (len == 0)
            {
                // failure
                return std::string("unknown_win");
            }
            if (len < buf.size() - 1)
            {
                // success (not truncated)
                break;
            }
            // truncated: enlarge and try again
            buf.resize(buf.size() * 2);
        }

        std::filesystem::path p{std::wstring(buf.data(), len)};
        if (include_path)
        {
            // convert to UTF-8 explicitly
            return pylabhub::format_tools::ws2s(p.native());
        }
        else
        {
            return pylabhub::format_tools::ws2s(p.filename().native());
        }

#elif defined(PYLABHUB_PLATFORM_LINUX)
        std::vector<char> buf;
        buf.resize(PATH_MAX);
        ssize_t count = readlink("/proc/self/exe", buf.data(), buf.size());
        if (count == -1)
        {
            return std::string("unknown_linux");
        }
        // if output filled the buffer, it might be truncated:
        if (static_cast<size_t>(count) >= buf.size())
        {
            // grow and retry
            buf.resize(buf.size() * 2);
            count = readlink("/proc/self/exe", buf.data(), buf.size());
            if (count == -1)
                return std::string("unknown_linux");
        }

        std::string full(buf.data(), static_cast<size_t>(count));
        std::filesystem::path p{full};
        if (include_path)
            return full;
        return p.filename().string();

#elif defined(PYLABHUB_PLATFORM_APPLE)

        // 1) Preferred: _NSGetExecutablePath to get path (may be a symlink)
        uint32_t size = 0;
        if (_NSGetExecutablePath(nullptr, &size) == -1 && size > 0)
        {
            std::vector<char> buf(size);
            if (_NSGetExecutablePath(buf.data(), &size) == 0) // success
            {
                // normalize path to resolve symlinks
                char resolved[PATH_MAX];
                if (realpath(buf.data(), resolved) != nullptr)
                {
                    std::filesystem::path p{resolved};
                    if (include_path)
                        return std::string(resolved);
                    return p.filename().string();
                }
                else
                {
                    std::filesystem::path p{std::string(buf.data(), size)};
                    if (include_path)
                        return p.string();
                    return p.filename().string();
                }
            }
        }

        // 2) Fallback: proc_pidpath
        {
            char procbuf[PROC_PIDPATHINFO_MAXSIZE];
            int ret = proc_pidpath(getpid(), procbuf, sizeof(procbuf));
            if (ret > 0)
            {
                char resolved[PATH_MAX];
                if (realpath(procbuf, resolved) != nullptr)
                {
                    if (include_path)
                        return std::string(resolved);
                    return std::filesystem::path(resolved).filename().string();
                }
                if (include_path)
                    return std::string(procbuf);
                return std::filesystem::path(procbuf).filename().string();
            }
        }

        return std::string("unknown_macos");

#else
        (void)include_path;
        return std::string("unknown");
#endif
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

} // namespace pylabhub::platform
