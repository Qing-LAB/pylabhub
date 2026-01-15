// Tools for formatting string
#pragma once

#include <chrono>
#include <filesystem>
#include <fmt/chrono.h>
#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::format_tools
{

// --- Helper: formatted local time with sub-second resolution (robust) ---
std::string formatted_time(std::chrono::system_clock::time_point timestamp);

/**
 * @brief Extracts a value from a dictionary-like string.
 *
 * This function parses a string containing key-value pairs (e.g.,
 * "key1=val1; key2=val2") and returns the value for a specified key.
 * It handles whitespace around separators and assignment symbols.
 *
 * @param input The string_view to parse.
 * @param separator The character separating key-value pairs.
 * @param assignment_symbol The character separating a key from its value.
 * @param keyword The key to search for.
 * @return An std::optional<std::string> containing the value if found,
 *         otherwise std::nullopt.
 */
std::optional<std::string> extract_value_from_string(std::string_view keyword,
                                                     std::string_view input, char separator = ';',
                                                     char assignment_symbol = '=');

std::wstring win32_to_long_path(const std::filesystem::path &);
std::wstring win32_make_unique_suffix();
std::wstring s2ws(const std::string &s);
std::string ws2s(const std::wstring &w);

// --- Helper: turn a string into a fmt::memory_buffer (compile-time format)
template <typename... Args>
fmt::memory_buffer make_buffer(fmt::format_string<Args...> fmt_str, Args &&...args)
{
    fmt::memory_buffer mb;
    mb.reserve(128); // small reserve to avoid many reallocs
    fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);
    return mb;
}
// --Helper: turn a string_view into a fmt::memory_buffer (runtime format)
template <typename... Args>
fmt::memory_buffer make_buffer_rt(fmt::string_view fmt_str, Args &&...args)
{
    fmt::memory_buffer mb;
    mb.reserve(128);
    fmt::format_to(std::back_inserter(mb), fmt::runtime(fmt_str), std::forward<Args>(args)...);
    return mb;
}

// Helper function to extract the basename at compile time
constexpr std::string_view filename_only(std::string_view file_path)
{
    const auto last_slash = file_path.find_last_of('/');
    const auto last_backslash = file_path.find_last_of('\\');

    // Find the position of the last separator
    // Use an if/else if structure for C++17 compatibility, or std::max in C++20
    const std::string_view::size_type last_separator_pos = [&]()
    {
        if (last_slash == std::string_view::npos)
        {
            return last_backslash;
        }
        if (last_backslash == std::string_view::npos)
        {
            return last_slash;
        }
        return last_slash > last_backslash ? last_slash : last_backslash;
    }();

    // If a separator was found, return the part after it; otherwise return the original string
    if (last_separator_pos == std::string_view::npos)
    {
        return file_path;
    }
    return file_path.substr(last_separator_pos + 1);
}

} // namespace pylabhub::format_tools
