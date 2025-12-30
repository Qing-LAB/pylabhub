// Tools for formatting string
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <fmt/chrono.h>


namespace pylabhub::format_tools {

// --- Helper: formatted local time with sub-second resolution (robust) ---
std::string formatted_time(std::chrono::system_clock::time_point timestamp);

inline std::wstring win32_to_long_path(const std::filesystem::path &);
inline std::wstring win32_make_unique_suffix();

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
}
