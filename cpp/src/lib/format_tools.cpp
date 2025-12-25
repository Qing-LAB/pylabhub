#include <filesystem>
#include <string>

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <chrono>
#include <random>
#include <sstream>
#include <windows.h>
#endif

#include "platform.hpp"
#include "format_tools.hpp"

namespace pylabhub::format_tools {

// --- Helper: formatted local time with sub-second resolution (robust) ---
// Replaces previous formatted_time(...) implementation.
// Behaviour:
//  - If the build system detected fmt chrono subseconds support (HAVE_FMT_CHRONO_SUBSECONDS),
//    use single-step fmt formatting on a microsecond-truncated time_point.
//  - Otherwise, fall back to computing the fractional microsecond part and append it
//    manually using a two-step format.
std::string formatted_time(std::chrono::system_clock::time_point timestamp)
{
#if defined(HAVE_FMT_CHRONO_SUBSECONDS) && HAVE_FMT_CHRONO_SUBSECONDS
    auto tp_us = std::chrono::time_point_cast<std::chrono::microseconds>(timestamp);
#if defined(FMT_CHRONO_FMT_STYLE) && (FMT_CHRONO_FMT_STYLE == 1)
    // use %fonon
    return fmt::format("{:%Y-%m-%d %H:%M:%S.%f}", tp_us);
#elif defined(FMT_CHRONO_FMT_STYLE) && (FMT_CHRONO_FMT_STYLE == 2)
    // fmt prints fraction without %f
    return fmt::format("{:%Y-%m-%d %H:%M:%S}", tp_us);
#else
    // defensive fallback to manual two-step
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp_us);
    int fractional_us = static_cast<int>((tp_us - secs).count());
    auto sec_part = fmt::format("{:%Y-%m-%d %H:%M:%S}", secs);
    return fmt::format("{}.{:06d}", sec_part, fractional_us);
#endif
#else
    // no runtime support detected — fallback to manual two-step method
    auto tp_us = std::chrono::time_point_cast<std::chrono::microseconds>(timestamp);
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp_us);
    int fractional_us = static_cast<int>((tp_us - secs).count());
    auto sec_part = fmt::format("{:%Y-%m-%d %H:%M:%S}", secs);
    return fmt::format("{}.{:06d}", sec_part, fractional_us);
#endif
}

#if defined(PLATFORM_WIN64)
static inline std::wstring normalize_backslashes(std::wstring s)
{
    for (auto &c : s)
        if (c == L'/')
            c = L'\\';
    return s;
}

/// Convert a path to Win32 long-path form with \\?\ or \\?\UNC\ prefix.
/// If path is already prefixed, return it unchanged. Caller should pass an absolute or relative
/// path.
inline std::wstring win32_to_long_path(const std::filesystem::path &p_in)
{
    std::filesystem::path abs = p_in;
    if (!abs.is_absolute())
        abs = std::filesystem::absolute(abs);
    std::wstring ws = abs.wstring();
    ws = normalize_backslashes(ws);

    if (ws.rfind(L"\\\\?\\", 0) == 0)
    {
        return ws;
    }
    if (ws.rfind(L"\\\\?\\UNC\\", 0) == 0)
    {
        return ws;
    }
    if (ws.rfind(L"\\\\", 0) == 0)
    {
        std::wstring rest = ws.substr(2);
        return std::wstring(L"\\\\?\\UNC\\") + rest;
    }
    return std::wstring(L"\\\\?\\") + ws;
}

/// Generate a reasonably-unique suffix for temp filenames.
inline std::wstring win32_make_unique_suffix()
{
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t r = gen();

    std::wstringstream ss;
    ss << L"." << pid << L"." << tid << L"." << now << L"." << std::hex << r;
    return ss.str();
}

#else

// POSIX stubs (not used on POSIX)
inline std::wstring win32_to_long_path(const std::filesystem::path &)
{
    return std::wstring();
}

inline std::wstring win32_make_unique_suffix()
{
    return std::wstring();
}

#endif

} // namespace pylabhub::basic::tools