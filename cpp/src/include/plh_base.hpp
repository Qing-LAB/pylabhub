#pragma once
/**
 * @file plh_base.hpp
 * @brief Layer 1: Basic modules built on plh_platform.
 *
 * Provides format_tools, debug_info, and foundational guards (atomic, recursion, scope).
 * Also includes module_def for lifecycle module registration.
 * Include this when you need formatting, debug utilities, or basic RAII guards.
 */
#include "plh_platform.hpp"

// Standard library support required by format_tools, debug_info, and guards
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "utils/format_tools.hpp"
#include "utils/debug_info.hpp"
#include "utils/atomic_guard.hpp"
#include "utils/recursion_guard.hpp"
#include "utils/scope_guard.hpp"
#include "utils/module_def.hpp"
