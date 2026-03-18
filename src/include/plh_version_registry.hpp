#pragma once
/**
 * @file plh_version_registry.hpp
 * @brief Centralized version registry for all pyLabHub components.
 *
 * Aggregates compile-time version constants for the library, SHM layout,
 * wire protocol, script API surface, and messaging facade ABI into a single
 * queryable struct.  Exposed to scripts via version_info_json().
 *
 * Design: HEP-CORE-0026.
 *
 * ## Versioning semantics (per-component major.minor)
 *
 * - Major mismatch → incompatible (reject at boundary)
 * - Minor mismatch → additive feature gap (log warning, proceed)
 *
 * ## Usage
 *
 * @code{.cpp}
 *   const auto v = pylabhub::version::current();
 *   LOGGER_INFO("Starting: {}", pylabhub::version::version_info_string());
 * @endcode
 */

#include "pylabhub_utils_export.h"

#include <cstdint>
#include <string>

namespace pylabhub::version
{

/**
 * @brief Compile-time version constants for every independently-versioned component.
 *
 * Each component pair (major, minor) follows semantic versioning:
 * - Major: breaking change (incompatible layout/protocol/API)
 * - Minor: additive change (new field/method, backward-compatible)
 *
 * Facade sizes are ABI canaries — caught at compile time by static_assert,
 * not negotiated at runtime.
 */
struct ComponentVersions
{
    // --- Library identity (CMake project() VERSION + git rev-list --count) ---
    uint16_t library_major;
    uint16_t library_minor;
    uint16_t library_rolling;

    // --- Shared memory header layout (data_block.hpp HEADER_VERSION_*) ---
    uint8_t shm_major;
    uint8_t shm_minor;

    // --- Control-plane wire protocol (REG_REQ, DISC_ACK field set) ---
    uint8_t wire_major;
    uint8_t wire_minor;

    // --- Python/Lua API surface version ---
    uint8_t script_api_major;
    uint8_t script_api_minor;

    // --- Messaging facade sizeof — ABI canary (no major/minor needed) ---
    uint16_t facade_producer_size;
    uint16_t facade_consumer_size;
};

// ============================================================================
// Query API
// ============================================================================

/**
 * @brief Returns the compile-time component version constants.
 */
PYLABHUB_UTILS_EXPORT ComponentVersions current() noexcept;

/**
 * @brief PEP 440 release version string (e.g., "0.1.0a0").
 */
PYLABHUB_UTILS_EXPORT const char *release_version() noexcept;

/**
 * @brief Bundled Python runtime version (e.g., "3.14.3+20260211").
 */
PYLABHUB_UTILS_EXPORT const char *python_runtime_version() noexcept;

/**
 * @brief Human-readable one-liner for logging.
 *
 * Example: "pylabhub 0.1.42 (shm=1.0, wire=1.0, script=1.0, facade=64/48)"
 */
PYLABHUB_UTILS_EXPORT std::string version_info_string();

/**
 * @brief JSON object for script consumption.
 *
 * Example:
 * @code{.json}
 * {
 *   "library": "0.1.42",
 *   "shm_major": 1, "shm_minor": 0,
 *   "wire_major": 1, "wire_minor": 0,
 *   "script_api_major": 1, "script_api_minor": 0,
 *   "facade_producer": 64, "facade_consumer": 48
 * }
 * @endcode
 */
PYLABHUB_UTILS_EXPORT std::string version_info_json();

} // namespace pylabhub::version

// ============================================================================
// C-linkage ABI query (stable symbol for ctypes / dlsym / FFI consumers)
// ============================================================================

/**
 * @brief Returns all version info as a JSON string (C-linkage, ABI-stable).
 *
 * The returned pointer is valid for the lifetime of the process (static buffer).
 * This is the only symbol Python scripts need to query ABI/protocol versions
 * from the shared library via ctypes.
 */
extern "C" PYLABHUB_UTILS_EXPORT const char *pylabhub_abi_info_json(void);
