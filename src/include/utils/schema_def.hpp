#pragma once
/**
 * @file schema_def.hpp
 * @brief Parsed schema data structures for the Named Schema Registry (HEP-CORE-0016).
 *
 * This header is the data-only layer of the schema system.  It defines the
 * in-memory representation of a parsed schema JSON file, including field
 * definitions, computed BLDS strings, BLAKE2b hashes, and struct sizes.
 *
 * It has no dependency on JSON or filesystem.  Those concerns belong to
 * SchemaLibrary (schema_library.hpp/.cpp).
 *
 * **Schema ID format**: `{namespace}.{name}@{version}`
 * Example: `$lab.sensors.temperature.raw.v1`
 *
 * @see HEP-CORE-0016-Named-Schema-Registry.md
 * @see schema_library.hpp — loading, forward lookup (id→entry), reverse lookup (hash→id)
 */

#include "utils/schema_blds.hpp" // SchemaInfo, BLDSBuilder, BLDSTypeID

#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::schema
{

// ============================================================================
// SchemaFieldDef
// ============================================================================

/**
 * @struct SchemaFieldDef
 * @brief Parsed representation of one field in a schema JSON `"fields"` array.
 *
 * JSON field format:
 * @code{.json}
 * { "name": "ts",      "type": "float64"             }   // scalar
 * { "name": "samples", "type": "float32", "count": 8 }   // array of 8 floats
 * { "name": "label",   "type": "char",    "count": 32}   // char array (string)
 * @endcode
 *
 * JSON type → BLDS token mapping (HEP-CORE-0016 §6.1):
 * @code
 *   float32 → f32    float64 → f64
 *   int8    → i8     int16 → i16    int32 → i32    int64 → i64
 *   uint8   → u8     uint16 → u16   uint32 → u32   uint64 → u64
 *   bool    → b      char   → c
 * @endcode
 *
 * Arrays of any primitive type are supported via `"count": N`.
 *
 * @par Differences vs FieldDef / ZmqSchemaField
 * This type set uses `"char"` (not `"string"` or `"bytes"`).  When a SchemaEntry
 * is converted to a FieldDef for ctypes slot exposure, `"char"` with `count=N`
 * becomes `FieldDef{type_str="string", length=N}` via schema_entry_to_spec().
 * There is no BLDS equivalent for `"bytes"` — use `uint8[N]` as a workaround.
 * @see FieldDef (schema_types.hpp) — the runtime type set (13 types)
 * @see ZmqSchemaField (hub_zmq_queue.hpp) — identical 13-type set used for ZMQ transport
 */
struct SchemaFieldDef
{
    std::string name;      ///< Field name (as it appears in the BLDS string)
    std::string type;      ///< JSON type string: "float64", "uint32", "char", …
    uint32_t    count{1};  ///< 1 = scalar; >1 = fixed-size array
    std::string unit;      ///< Optional unit annotation (e.g. "s", "°C")
    std::string description; ///< Optional human description
};

// ============================================================================
// SchemaLayoutDef
// ============================================================================

/**
 * @struct SchemaLayoutDef
 * @brief Field list + packing for one layout section (slot or flexzone).
 *
 * `packing` is one of `"aligned"` (natural C-struct layout) or `"packed"`
 * (no padding, `_pack_=1`).  It MUST be declared explicitly in the JSON
 * `slot` / `flexzone` block (HEP-CORE-0034 §6.2); the parser rejects
 * blocks that omit it.  Packing is part of the schema fingerprint
 * (HEP-CORE-0034 §6.3) — two layouts with the same fields and different
 * packing have different hashes.
 *
 * Offsets and struct size are computed by SchemaLibrary from field order
 * and the declared packing rule — they are not stored in the JSON.
 */
struct SchemaLayoutDef
{
    std::vector<SchemaFieldDef> fields;
    std::string                 packing{"aligned"}; ///< "aligned" or "packed"
};

// ============================================================================
// SchemaEntry
// ============================================================================

/**
 * @struct SchemaEntry
 * @brief Complete parsed and computed representation of one named schema.
 *
 * Populated by SchemaLibrary::load_from_file() or load_from_string().
 * The `slot_info` and `flexzone_info` members are computed (not stored in JSON):
 *   - `blds`  — semicolon-separated `name:type_id` pairs, built from `fields`
 *   - `hash`  — BLAKE2b-256 of the BLDS string
 *   - `struct_size` — computed via natural alignment rules
 *
 * C++ producers pass the schema to `ShmQueue::create_writer()` /
 * `ShmQueue::create_reader()` which compute sizes via `compute_field_layout()`.
 */
struct SchemaEntry
{
    std::string    schema_id;    ///< Full ID: "$lab.sensors.temperature.raw.v1"
    uint32_t       version{1};   ///< Schema version integer (the N in @N)
    std::string    description;  ///< Optional human description from JSON

    SchemaLayoutDef slot;     ///< Slot layout (always present for valid entries)
    SchemaLayoutDef flexzone; ///< FlexZone layout (may be empty if not declared)

    /// Computed from `slot.fields` by SchemaLibrary.
    SchemaInfo slot_info;
    /// Computed from `flexzone.fields` by SchemaLibrary. Zero-filled if empty.
    SchemaInfo flexzone_info;

    /// True if the schema declares any flexzone fields.
    [[nodiscard]] bool has_flexzone() const noexcept
    {
        return !flexzone.fields.empty();
    }
};

} // namespace pylabhub::schema
