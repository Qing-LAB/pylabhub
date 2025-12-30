#include "platform.hpp"

#include <filesystem>
#include <string>
#if defined(PLATFORM_WIN64)
#include <chrono>
#include <random>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if defined(PLATFORM_APPLE)
#include <limits.h> // For PATH_MAX
#endif

#include "fmt/core.h"

namespace pylabhub::platform
{
// Anonymous namespace for helpers local to this translation unit.

// Helper to get the current process ID in a cross-platform way.
long get_pid()
{
#if defined(PLATFORM_WIN64)
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

// Gets a platform-native thread ID for logging.
uint64_t get_native_thread_id() noexcept
{
#if defined(PLATFORM_WIN64)
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
std::string get_executable_name()
{
    try
    {
#if defined(PLATFORM_WIN64)
        wchar_t path[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0)
        {
            return "unknown_win";
        }
        return std::filesystem::path(path).filename().string();
#elif defined(PLATFORM_LINUX)
        char result[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
        if (count > 0)
        {
            return std::filesystem::path(std::string(result, count)).filename().string();
        }
        return "unknown_linux";
#elif defined(PLATFORM_APPLE)
        char path[PATH_MAX];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) == 0)
        {
            return std::filesystem::path(path).filename().string();
        }
        return "unknown_apple";
#endif
    }
    catch (const std::exception& e)
    {
        // std::filesystem operations can throw on invalid paths.
        // Log the error but don't crash the lifecycle manager.
        fmt::print(stderr, "[pylabhub-lifecycle] Warning: get_executable_name failed: {}.\n", e.what());
    }
    catch (...)
    {
        fmt::print(stderr, "[pylabhub-lifecycle] Warning: get_executable_name failed with unknown exception.\n");
    }
    return "unknown";
}

} // namespace pylabhub::platform