/**
 * @file version_registry.cpp
 * @brief Implementation of the centralized version registry (HEP-CORE-0026).
 */

#include "plh_version_registry.hpp"
#include "plh_platform.hpp"
#include "pylabhub_version.h"  // PYLABHUB_RELEASE_VERSION, PYLABHUB_PYTHON_RUNTIME_VERSION
#include "utils/data_block.hpp" // HEADER_VERSION_MAJOR/MINOR

#include <fmt/format.h>

// ============================================================================
// Component version constants
// ============================================================================

// SHM layout version — re-exported from data_block.hpp
static constexpr uint8_t kShmMajor = static_cast<uint8_t>(pylabhub::hub::detail::HEADER_VERSION_MAJOR);
static constexpr uint8_t kShmMinor = static_cast<uint8_t>(pylabhub::hub::detail::HEADER_VERSION_MINOR);

// Wire protocol version — REG_REQ/DISC_ACK field set.
// Bump major on breaking changes (removed/renamed fields, changed semantics).
// Bump minor on additive changes (new optional fields).
static constexpr uint8_t kWireMajor = 1;
static constexpr uint8_t kWireMinor = 0;

// Script API surface version — Python (pybind11) and Lua API.
// Bump major on breaking changes (removed/renamed methods).
// Bump minor on additive changes (new methods/properties).
static constexpr uint8_t kScriptApiMajor = 1;
static constexpr uint8_t kScriptApiMinor = 0;

namespace pylabhub::version
{

ComponentVersions current() noexcept
{
    return ComponentVersions{
        static_cast<uint16_t>(platform::get_version_major()),
        static_cast<uint16_t>(platform::get_version_minor()),
        static_cast<uint16_t>(platform::get_version_rolling()),
        kShmMajor,
        kShmMinor,
        kWireMajor,
        kWireMinor,
        kScriptApiMajor,
        kScriptApiMinor,
    };
}

const char *release_version() noexcept
{
    return PYLABHUB_RELEASE_VERSION;
}

const char *python_runtime_version() noexcept
{
    return PYLABHUB_PYTHON_RUNTIME_VERSION;
}

std::string version_info_string()
{
    const auto v = current();
    return fmt::format(
        "pylabhub {} (lib={}.{}.{}, shm={}.{}, wire={}.{}, script={}.{}, python_rt={})",
        PYLABHUB_RELEASE_VERSION,
        v.library_major, v.library_minor, v.library_rolling,
        v.shm_major, v.shm_minor,
        v.wire_major, v.wire_minor,
        v.script_api_major, v.script_api_minor,
        PYLABHUB_PYTHON_RUNTIME_VERSION);
}

std::string version_info_json()
{
    const auto v = current();
    return fmt::format(
        R"({{"release":"{}","library":"{}.{}.{}","python_runtime":"{}","shm_major":{},"shm_minor":{},"wire_major":{},"wire_minor":{},"script_api_major":{},"script_api_minor":{}}})",
        PYLABHUB_RELEASE_VERSION,
        v.library_major, v.library_minor, v.library_rolling,
        PYLABHUB_PYTHON_RUNTIME_VERSION,
        v.shm_major, v.shm_minor,
        v.wire_major, v.wire_minor,
        v.script_api_major, v.script_api_minor);
}

} // namespace pylabhub::version

// ============================================================================
// C-linkage ABI query — stable symbol for ctypes / dlsym / FFI consumers.
// Returns a pointer to a process-lifetime static buffer.
// ============================================================================

extern "C" const char *pylabhub_abi_info_json(void)
{
    static const std::string s = pylabhub::version::version_info_json();
    return s.c_str();
}
