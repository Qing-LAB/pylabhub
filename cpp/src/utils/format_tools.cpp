// format_tools.cpp
#include "plh_base.hpp"

#if defined(PYLABHUB_PLATFORM_WIN64)
#include <chrono>
#include <random>
#include <sstream>
#endif

namespace pylabhub::format_tools
{

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
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(tp_us - secs).count();
    // normalize to 0..999999 even for negative timestamps
    int fractional_us = static_cast<int>(us % 1000000);
    if (fractional_us < 0)
        fractional_us += 1000000;
    auto sec_part = fmt::format("{:%Y-%m-%d %H:%M:%S}", secs);
    return fmt::format("{}.{:06d}", sec_part, fractional_us);
#endif
}

#if defined(PYLABHUB_PLATFORM_WIN64)
static inline std::wstring normalize_backslashes(std::wstring s)
{
    for (auto &c : s)
        if (c == L'/')
            c = L'\\';
    return s;
}

/// Convert a path to Win32 long-path form with \\?\ or \\?\UNC\ prefix.
/// If path is already prefixed, return it unchanged. Caller should pass an absolute or relative
/// path. When error encountered, return a blank wstring
std::wstring win32_to_long_path(const std::filesystem::path &p_in)
{
    std::error_code ec;
    std::filesystem::path abs = p_in;
    if (!abs.is_absolute())
    {
        abs = std::filesystem::absolute(abs, ec);
        if (ec)
        {
            return std::wstring{};
        }
    }
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
std::wstring win32_make_unique_suffix()
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

std::wstring s2ws(const std::string &s)
{
    if (s.empty())
        return {};

    int required =
        MultiByteToWideChar(CP_UTF8, // UTF-8 input
                            MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);

    if (required <= 0)
        return {};

    std::wstring w(required, L'\0');

    int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                      static_cast<int>(s.size()), w.data(), required);

    if (written == 0)
        return {};

    return w;
}

std::string ws2s(const std::wstring &w)
{
    if (w.empty())
        return {};

    int required = WideCharToMultiByte(CP_UTF8, // UTF-8 output
                                       WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()),
                                       nullptr, 0, nullptr, nullptr);

    if (required <= 0)
        return {};

    std::string s(required, '\0');

    int written =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()),
                            s.data(), required, nullptr, nullptr);

    if (written == 0)
        return {};

    return s;
}

#else

// POSIX stubs (not used on POSIX)
std::wstring win32_to_long_path([[maybe_unused]] const std::filesystem::path &path)
{
    return {};
}

std::wstring win32_make_unique_suffix()
{
    return {};
}

std::wstring s2ws([[maybe_unused]] const std::string &str)
{
    return {};
}

std::string ws2s([[maybe_unused]] const std::wstring &wstr)
{
    return {};
}

#endif

// Helper to trim whitespace from both ends of a string_view.
// It's defined in an anonymous namespace to limit its scope to this file.
constexpr std::string_view trim_whitespace(std::string_view str) noexcept
{
    constexpr std::string_view whitespace = " \t\n\r\f\v";

    auto first = str.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
    {
        // all whitespace
        return str.substr(0, 0);
    }
    auto last = str.find_last_not_of(whitespace);
    // substr takes (pos, count). last >= first so count = last-first+1
    return str.substr(first, last - first + 1);
}

std::optional<std::string> extract_value_from_string(std::string_view keyword, char separator,
                                                     std::string_view input,
                                                     char assignment_symbol)
{
    std::string_view::size_type start = 0;
    while (start < input.size())
    {
        // Find the next separator or the end of the string
        std::string_view::size_type end = input.find(separator, start);
        if (end == std::string_view::npos)
        {
            end = input.size();
        }

        // Get the current key-value pair segment
        std::string_view segment = input.substr(start, end - start);
        start = end + 1;

        // Find the assignment symbol within the segment
        std::string_view::size_type assignment_pos = segment.find(assignment_symbol);
        if (assignment_pos == std::string_view::npos)
        {
            continue; // No assignment symbol, so it's not a valid pair
        }

        // Extract and trim the key
        std::string_view key_sv = segment.substr(0, assignment_pos);
        key_sv = trim_whitespace(key_sv);

        // Compare with the desired keyword
        if (key_sv == keyword)
        {
            // Extract, trim, and return the value
            std::string_view value_sv = segment.substr(assignment_pos + 1);
            value_sv = trim_whitespace(value_sv);
            return std::string(value_sv);
        }
    }
    return std::nullopt; // Keyword not found
}

} // namespace pylabhub::format_tools