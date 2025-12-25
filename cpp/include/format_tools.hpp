// Tools for formatting string
#pragma once

#include <chrono>
#include <string>
#include <fmt/chrono.h>
#include <filesystem>

namespace pylabhub::format_tools {

// --- Helper: formatted local time with sub-second resolution (robust) ---
std::string formatted_time(std::chrono::system_clock::time_point timestamp);

inline std::wstring win32_to_long_path(const std::filesystem::path &);
inline std::wstring win32_make_unique_suffix();

}
