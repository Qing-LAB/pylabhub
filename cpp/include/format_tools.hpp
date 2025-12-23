// Tools for formatting string
#pragma once

#include <chrono>
#include <string>
#include <fmt/chrono.h>

namespace pylabhub::basic::tools {

// --- Helper: formatted local time with sub-second resolution (robust) ---
std::string formatted_time(std::chrono::system_clock::time_point timestamp);

}
