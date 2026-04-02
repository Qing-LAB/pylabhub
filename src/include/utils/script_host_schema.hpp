#pragma once
/**
 * @file script_host_schema.hpp
 * @brief Shared schema types for all script host binaries.
 *
 * Defines the slot/flexzone schema representation used by processor, producer,
 * and consumer script hosts.  Each per-component `*_schema.hpp` imports these
 * types via `using` declarations into its own namespace.
 *
 * All schemas are field-based: a list of typed named fields with optional
 * fixed-size arrays (count > 1). The engine presents array fields using the
 * language's native array type: numpy.ndarray (Python), FFI array (Lua),
 * std::span / raw pointer (C++).
 *
 * Public header — required by role_host_core.hpp (SchemaSpec is a member of
 * RoleHostCore). No external dependencies beyond the standard library.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// FieldDef — one typed field
// ============================================================================

/**
 * @struct FieldDef
 * @brief One typed field in a slot, flexzone, or inbox schema.
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
 * Validation is enforced by parse_schema_json() (schema_utils.hpp).
 *
 * @par Arrays
 * For numeric types: `count > 1` creates a fixed-size array (e.g., `float64[4]`).
 * Python exposes arrays as numpy.ndarray views automatically.
 * Lua exposes them as FFI arrays. Native C++ uses std::span or raw pointers.
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
// SchemaSpec — parsed schema for one buffer (slot, flexzone, or inbox)
// ============================================================================

/**
 * @struct SchemaSpec
 * @brief Field-based schema definition.
 *
 * All data exchange in pylabHub uses field-based schemas. Each schema is a list
 * of typed named fields. Array fields (count > 1) are supported for numeric
 * types and are presented as language-native array views by each engine.
 */
struct SchemaSpec
{
    bool                  has_schema{false};
    std::vector<FieldDef> fields;
    std::string           packing{"aligned"}; ///< "aligned" or "packed"
};

} // namespace pylabhub::scripting
