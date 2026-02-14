#pragma once
/**
 * @file plh_base.hpp
 * @brief Layer 1: Basic modules built on plh_platform.
 *
 * Provides format_tools, debug_info, and foundational guards: in-process spinlock (SpinGuard),
 * recursion_guard, scope_guard. Also includes module_def for lifecycle module registration.
 * Include this when you need formatting, debug utilities, or basic RAII guards.
 *
 * Spin state (in-process): InProcessSpinState, SpinGuard, make_in_process_spin_state
 * (utils/in_process_spin_state.hpp). Guard performs locking; state owns the 32-byte state.
 * Cross-process SharedSpinLock is in utils/shared_memory_spinlock.hpp (used by DataBlock).
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

#include <fmt/format.h>
#include <fmt/chrono.h>

#include "utils/format_tools.hpp"
#include "utils/debug_info.hpp"
#include "utils/in_process_spin_state.hpp"
#include "utils/recursion_guard.hpp"
#include "utils/scope_guard.hpp"
#include "utils/module_def.hpp"
