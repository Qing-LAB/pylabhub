#pragma once

// This umbrella header groups foundational utilities.

#include <atomic>
#include <cstdint>
#include <source_location>

#include <filesystem>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "plh_platform.hpp"

#include "utils/format_tools.hpp"

#include "utils/debug_info.hpp"

#include "utils/atomic_guard.hpp"
#include "utils/recursion_guard.hpp"
#include "utils/scope_guard.hpp"
#include "utils/ModuleDef.hpp"
