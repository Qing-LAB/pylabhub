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

/**
 * @struct FieldDef
 * @brief One typed field in a slot or flexzone ctypes schema.
 *
 * @par Supported type_str values (13 total — identical to ZmqSchemaField::type_str)
 *
 * | type_str  | C type     | Python ctypes | Size | Notes                        |
 * |-----------|------------|---------------|------|------------------------------|
 * | "bool"    | bool       | c_bool        | 1 B  | scalar only                  |
 * | "int8"    | int8_t     | c_int8        | 1 B  |                              |
 * | "uint8"   | uint8_t    | c_uint8       | 1 B  |                              |
 * | "int16"   | int16_t    | c_int16       | 2 B  |                              |
 * | "uint16"  | uint16_t   | c_uint16      | 2 B  |                              |
 * | "int32"   | int32_t    | c_int32       | 4 B  |                              |
 * | "uint32"  | uint32_t   | c_uint32      | 4 B  |                              |
 * | "int64"   | int64_t    | c_int64       | 8 B  |                              |
 * | "uint64"  | uint64_t   | c_uint64      | 8 B  |                              |
 * | "float32" | float      | c_float       | 4 B  |                              |
 * | "float64" | double     | c_double      | 8 B  |                              |
 * | "string"  | char[N]    | c_char * N    | N B  | requires length > 0          |
 * | "bytes"   | uint8_t[N] | c_uint8 * N   | N B  | requires length > 0          |
 *
 * Validation is enforced by parse_schema_json() (script_host_helpers.hpp).
 *
 * @par Named schema compatibility (SchemaFieldDef)
 * The BLDS named schema registry uses `"char"` (with count=N) where FieldDef
 * uses `"string"` (with length=N).  The conversion is performed by
 * schema_entry_to_spec() in script_host_helpers.hpp.  There is no direct BLDS
 * equivalent for `"bytes"` — use `uint8[N]` in the JSON schema as a workaround.
 *
 * @par Arrays vs blobs
 * For numeric types: `count > 1` → fixed-size array (e.g., `float64[4]`).
 * For "string"/"bytes": `count` must be 1; `length` holds the byte size.
 */
struct FieldDef
{
    std::string name;
    std::string type_str;  ///< One of the 13 valid type strings listed above.
    uint32_t    count{1};  ///< Elements (>1 = array for numeric types; must be 1 for string/bytes).
    uint32_t    length{0}; ///< Byte length (required for "string"/"bytes"; ignored for numeric types).
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
    std::string           packing{"aligned"};

    // NumpyArray mode
    std::string          numpy_dtype{};
    std::vector<int64_t> numpy_shape{};
};

} // namespace pylabhub::scripting
