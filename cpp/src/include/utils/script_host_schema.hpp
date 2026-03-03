#pragma once
/**
 * @file script_host_schema.hpp
 * @brief Shared schema types for all script host binaries.
 *
 * Defines the slot/flexzone schema representation used by processor, producer,
 * and consumer script hosts.  Each per-component `*_schema.hpp` imports these
 * types via `using` declarations into its own namespace.
 *
 * This header is internal to the script host layer and is NOT part of the
 * public pylabhub-utils API.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// SlotExposure
// ============================================================================

enum class SlotExposure
{
    Ctypes,    ///< ctypes.LittleEndianStructure with typed named fields (default)
    NumpyArray ///< numpy.ndarray(dtype, shape) — zero-copy frombuffer view of SHM slot
};

// ============================================================================
// FieldDef — one typed field (ctypes mode)
// ============================================================================

struct FieldDef
{
    std::string name;
    std::string type_str;
    uint32_t    count{1};
    uint32_t    length{0};
};

// ============================================================================
// SchemaSpec — parsed schema for one buffer (slot or flexzone)
// ============================================================================

struct SchemaSpec
{
    bool has_schema{false};

    // Ctypes mode
    SlotExposure          exposure{SlotExposure::Ctypes};
    std::vector<FieldDef> fields;
    std::string           packing{"natural"};

    // NumpyArray mode
    std::string          numpy_dtype{};
    std::vector<int64_t> numpy_shape{};
};

} // namespace pylabhub::scripting
