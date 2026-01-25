// Tools for formatting string
#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>      // or the specific fmt headers used

namespace pylabhub::format_tools
{

/**
 * @brief Formats a system_clock time_point into a string with microsecond precision.
 * @param timestamp The time_point to format.
 * @return A string in the format "YYYY-MM-DD HH:MM:SS.us".
 */
PYLABHUB_UTILS_EXPORT std::string formatted_time(std::chrono::system_clock::time_point timestamp);

/**
 * @brief Extracts a value from a dictionary-like string.
 *
 * This function parses a string containing key-value pairs (e.g.,
 * "key1=val1; key2=val2") and returns the value for a specified key.
 * It handles whitespace around separators and assignment symbols.
 * 
 * @param keyword The key to search for.
 * @param input The string_view to parse.
 * @param separator The character separating key-value pairs.
 * @param assignment_symbol The character separating a key from its value.
 * @return An std::optional<std::string> containing the value if found,
 *         otherwise std::nullopt.
 */
PYLABHUB_UTILS_EXPORT std::optional<std::string>
extract_value_from_string(std::string_view keyword, std::string_view input, char separator = ';',
                          char assignment_symbol = '=');

/**
 * @brief Converts a path to its Windows long path representation (e.g., `\\?\C:\...`).
 * @param path The path to convert.
 * @return The long path as a wstring. Returns an empty string on non-Windows platforms.
 */
PYLABHUB_UTILS_EXPORT std::wstring win32_to_long_path(const std::filesystem::path &);
/**
 * @brief Generates a unique suffix for temporary filenames on Windows.
 * @return A unique wstring suffix. Returns an empty string on non-Windows platforms.
 */
PYLABHUB_UTILS_EXPORT std::wstring win32_make_unique_suffix();
/**
 * @brief Converts a UTF-8 encoded std::string to a std::wstring on Windows.
 * @param s The source string.
 * @return The converted wstring. Returns an empty string on non-Windows platforms.
 */
PYLABHUB_UTILS_EXPORT std::wstring s2ws(const std::string &s);
/**
 * @brief Converts a std::wstring to a UTF-8 encoded std::string on Windows.
 * @param w The source wstring.
 * @return The converted string. Returns an empty string on non-Windows platforms.
 */
PYLABHUB_UTILS_EXPORT std::string ws2s(const std::wstring &w);

/**
 * @brief Creates a `fmt::memory_buffer` from a compile-time format string and arguments.
 * @tparam Args Argument types for the format string.
 * @param fmt_str The `fmt`-style format string.
 * @param args The arguments to format.
 * @return A `fmt::memory_buffer` containing the formatted result.
 */
template <typename... Args>
fmt::memory_buffer make_buffer(fmt::format_string<Args...> fmt_str, Args &&...args)
{
    fmt::memory_buffer mb;
    mb.reserve(128); // small reserve to avoid many reallocs
    fmt::format_to(std::back_inserter(mb), fmt_str, std::forward<Args>(args)...);
    return mb;
}

/**
 * @brief Creates a `fmt::memory_buffer` from a runtime format string and arguments.
 * @tparam Args Argument types for the format string.
 * @param fmt_str The runtime `fmt`-style format string.
 * @param args The arguments to format.
 * @return A `fmt::memory_buffer` containing the formatted result.
 */
template <typename... Args>
fmt::memory_buffer make_buffer_rt(fmt::string_view fmt_str, Args &&...args)
{
    fmt::memory_buffer mb;
    mb.reserve(128);
    fmt::format_to(std::back_inserter(mb), fmt::runtime(fmt_str), std::forward<Args>(args)...);
    return mb;
}

/**
 * @brief Extracts the filename from a full path at compile time.
 * @param file_path A string_view of the full path.
 * @return A string_view of just the filename portion of the path.
 */
constexpr std::string_view filename_only(std::string_view file_path) noexcept
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
