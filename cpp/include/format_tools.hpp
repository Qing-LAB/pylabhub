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
} // namespace pylabhub::format_tools
