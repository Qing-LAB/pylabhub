#pragma once
/**
 * @file schema_types.hpp
 * @brief Core schema data types — FieldDef, SchemaSpec, SchemaFieldDesc, FieldLayout.
 *
 * Lightweight header: data structures only, no function bodies, minimal includes.
 * Used by role_host_core.hpp, role_api_base.hpp, and all schema-related headers.
 *
 * All types are in pylabhub::hub namespace — schema describes data layout,
 * which is a hub-level (infrastructure) concern, not scripting-specific.
 *
 * @par Type vocabulary (13 types, shared across all layers):
 * bool, int8, uint8, int16, uint16, int32, uint32, int64, uint64,
 * float32, float64, string, bytes
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// FieldDef — one typed field in a schema
// ============================================================================

/**
 * @struct FieldDef
 * @brief One typed field in a slot, flexzone, or inbox schema.
 *
 * @par Supported type_str values (13 total)
 *
 * | type_str  | C type     | Size | Notes                        |
 * |-----------|------------|------|------------------------------|
 * | "bool"    | bool       | 1 B  | scalar only                  |
 * | "int8"    | int8_t     | 1 B  |                              |
 * | "uint8"   | uint8_t    | 1 B  |                              |
 * | "int16"   | int16_t    | 2 B  |                              |
 * | "uint16"  | uint16_t   | 2 B  |                              |
 * | "int32"   | int32_t    | 4 B  |                              |
 * | "uint32"  | uint32_t   | 4 B  |                              |
 * | "int64"   | int64_t    | 8 B  |                              |
 * | "uint64"  | uint64_t   | 8 B  |                              |
 * | "float32" | float      | 4 B  |                              |
 * | "float64" | double     | 8 B  |                              |
 * | "string"  | char[N]    | N B  | requires length > 0          |
 * | "bytes"   | uint8_t[N] | N B  | requires length > 0          |
 *
 * Validation is enforced by parse_schema_json() (schema_utils.hpp).
 *
 * @par Arrays
 * For numeric types: `count > 1` creates a fixed-size array (e.g., `float64[4]`).
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

// ============================================================================
// SchemaFieldDesc — layout-level field descriptor (no name, for size computation)
// ============================================================================

/// Matches ZmqSchemaField but lives in the shared header.
/// Used by compute_field_layout() for C-struct size computation.
struct SchemaFieldDesc
{
    std::string type_str;
    uint32_t    count{1};
    uint32_t    length{0};
};

// ============================================================================
// FieldLayout — computed byte-level layout for one field
// ============================================================================

/// Produced by compute_field_layout(); consumed by wire pack/unpack and size queries.
struct FieldLayout
{
    size_t      offset{0};     ///< Byte offset within the struct.
    size_t      byte_size{0};  ///< Total bytes for this field (elem_size * count, or length for string/bytes).
    std::string type_str;      ///< Original type string (for pack/unpack dispatch).
    bool        is_bin{false}; ///< True for arrays (count>1), string, bytes — packed as binary blob.
};

} // namespace pylabhub::hub
