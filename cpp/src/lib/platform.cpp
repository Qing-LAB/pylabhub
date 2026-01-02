#include "platform.hpp"

#include <filesystem>
#include <string>
#if defined(PYLABHUB_PLATFORM_WIN64)
#include <windows.h>
#include <dbghelp.h> // For CaptureStackBackTrace, StackWalk64, SymInitialize
#include <memory>    // For std::unique_ptr
#pragma comment(lib, "dbghelp.lib") // Link with Dbghelp.lib
#else
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(PYLABHUB_IS_POSIX)
#include <execinfo.h> // For backtrace, backtrace_symbols
#include <cxxabi.h>   // For __cxa_demangle
#endif
#endif
#if defined(PYLABHUB_PLATFORM_APPLE)
#include <limits.h> // For PATH_MAX
#endif

#include "fmt/core.h"

namespace pylabhub::platform
{
// Anonymous namespace for helpers local to this translation unit.

// Helper to get the current process ID in a cross-platform way.
long get_pid()
{
#if defined(PYLABHUB_PLATFORM_WIN64)
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
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
std::string get_executable_name()
{
    try
    {
#if defined(PYLABHUB_PLATFORM_WIN64)
        wchar_t path[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0)
        {
            return "unknown_win";
        }
        return std::filesystem::path(path).filename().string();
#elif defined(PYLABHUB_PLATFORM_LINUX)
        char result[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
        if (count > 0)
        {
            return std::filesystem::path(std::string(result, count)).filename().string();
        }
        return "unknown_linux";
#elif defined(PYLABHUB_PLATFORM_APPLE)
        char path[PATH_MAX];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) == 0)
        {
            return std::filesystem::path(path).filename().string();
        }
        return "unknown_apple";
#else
        return "unknown";
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

void print_stack_trace() noexcept
{
    fmt::print(stderr, "Stack Trace:\n");

#if defined(PYLABHUB_PLATFORM_WIN64)
    // Windows implementation
    const int max_frames = 62; // Maximum frames to capture
    void *stack[max_frames];
    SYMBOL_INFO *symbol;
    HANDLE process;
    DWORD displacement;

    process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE); // Initialize symbol handler

    USHORT frames = CaptureStackBackTrace(0, max_frames, stack, NULL);

    symbol = (SYMBOL_INFO *)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    for (USHORT i = 0; i < frames; i++)
    {
        SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
        fmt::print(stderr, "  {}: {} - 0x{:X}\n", frames - i - 1, symbol->Name, symbol->Address);
    }

    free(symbol);
    SymCleanup(process);

#elif defined(PYLABHUB_IS_POSIX)
    // POSIX implementation (Linux, macOS)
    const int max_frames = 100;
    void *callstack[max_frames];
    int frames = backtrace(callstack, max_frames);
    char **strs = backtrace_symbols(callstack, frames);

    if (strs == nullptr)
    {
        fmt::print(stderr, "  [Unable to get stack symbols]\n");
        return;
    }

    for (int i = 0; i < frames; ++i)
    {
        std::string s(strs[i]);
#if defined(__linux__) // Demangling for GCC/Clang on Linux
        size_t funcstart = s.find('(');
        size_t funcend = s.find('+', funcstart);
        if (funcstart != std::string::npos && funcend != std::string::npos)
        {
            std::string funcname = s.substr(funcstart + 1, funcend - (funcstart + 1));
            int status;
            char *demangled_name = abi::__cxa_demangle(funcname.c_str(), nullptr, nullptr, &status);
            if (status == 0)
            {
                // Replace mangled name with demangled one
                s.replace(funcstart + 1, funcend - (funcstart + 1), demangled_name);
            }
            free(demangled_name);
        }
#endif
        fmt::print(stderr, "  {}\n", s);
    }
    free(strs);

#else
    // Fallback for unknown platforms
    fmt::print(stderr, "  [Stack trace not available on this platform]\n");
#endif
}

} // namespace pylabhub::platform
