#pragma once
// PathUtil.hpp - centralized path helpers (Windows long-path support)
// Place at: include/utils/PathUtil.hpp
//
// Provides:
//  - win32_to_long_path(std::filesystem::path) -> std::wstring
//  - win32_make_unique_suffix() -> std::wstring
//
// On POSIX these functions are no-ops or return empty wstrings (not used).

#include <filesystem>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <chrono>
#include <random>
#include <sstream>
#include <windows.h>
#endif

namespace pylabhub::utils
{

#if defined(_WIN32)
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

} // namespace pylabhub::utils
