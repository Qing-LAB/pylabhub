#pragma once

/*******************************************************************************
 * @file PathUtil.hpp
 * @brief Internal path manipulation helpers, primarily for Windows.
 *
 * **Design and Purpose**
 * This header provides centralized helper functions for path manipulation. Its
 * primary purpose is to abstract away platform-specific complexities,
 * particularly the legacy `MAX_PATH` (260 character) limitation on Windows.
 *
 * **Internal Use Only**
 * This is an **internal utility header** for the `pylabhub-utils` library. It is
 * **not** part of the public API and should not be included by consumers of the
 * library. Its functions are `inline` and intended to be compiled directly into
 * the translation units that use them, such as `FileLock.cpp` and `JsonConfig.cpp`.
 * Because it is not part of the public API, it does not require ABI stability
 * measures (like Pimpl) or symbol export macros.
 *
 * **Key Functions (Windows)**:
 * - `win32_to_long_path()`: Converts a standard path to its `\\?\` prefixed
 *   long path equivalent. This allows Windows APIs to correctly handle paths
 *   exceeding `MAX_PATH`. This is used by `FileLock` and `JsonConfig` before
 *   calling `CreateFileW` or `ReplaceFileW`.
 * - `win32_make_unique_suffix()`: Generates a unique-ish string for creating
 *   temporary filenames, used by `JsonConfig::atomic_write_json`.
 *
 * **Platform Behavior**:
 * - **Windows**: The functions are fully implemented.
 * - **POSIX**: The functions are defined as empty stubs that do nothing, as
 *   these path limitations do not exist on POSIX systems.
 ******************************************************************************/

#include <filesystem>
#include <string>

#include "platform.hpp"

#if defined(PLATFORM_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <chrono>
#include <random>
#include <sstream>
#include <windows.h>
#endif

namespace pylabhub::utils
{

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

} // namespace pylabhub::utils
