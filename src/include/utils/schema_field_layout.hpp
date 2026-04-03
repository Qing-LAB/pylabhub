#pragma once
/**
 * @file schema_field_layout.hpp
 * @brief Schema field layout computation — authoritative struct size from field definitions.
 *
 * Computes C-struct-compatible field offsets and total size from a list of typed
 * fields + packing mode. Used by all queue types (SHM, ZMQ, Inbox) and by
 * engine register_slot_type for validation.
 *
 * Alignment rules match ctypes.LittleEndianStructure (Python) and standard C
 * struct layout (Lua FFI, Native). Packed mode (_pack_=1 / __attribute__((packed)))
 * disables all alignment.
 *
 * @see schema_types.hpp for SchemaFieldDesc, FieldLayout type definitions.
 */

#include "utils/schema_types.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace pylabhub::hub
{

// ============================================================================
// Type utilities
// ============================================================================

/// Validate a type_str against the 13 supported types.
inline bool is_valid_type_str(const std::string &t) noexcept
{
    return t == "bool"    || t == "int8"   || t == "uint8"  ||
           t == "int16"   || t == "uint16" || t == "int32"  ||
           t == "uint32"  || t == "int64"  || t == "uint64" ||
           t == "float32" || t == "float64"||
           t == "string"  || t == "bytes";
}

/// Element size in bytes for a type_str. For string/bytes, caller uses length.
inline size_t field_elem_size(const std::string &t) noexcept
{
    if (t == "bool" || t == "int8"  || t == "uint8")          return 1;
    if (t == "int16" || t == "uint16")                         return 2;
    if (t == "int32" || t == "uint32" || t == "float32")       return 4;
    if (t == "int64" || t == "uint64" || t == "float64")       return 8;
    return 1; // string/bytes: caller uses length directly
}

/// Natural alignment for a type_str. String/bytes are byte-aligned.
inline size_t field_align(const std::string &t) noexcept
{
    if (t == "string" || t == "bytes") return 1;
    return field_elem_size(t);
}

// ============================================================================
// Layout computation
// ============================================================================

/// Compute per-field offsets using C struct alignment rules.
/// "aligned" = natural alignment per field, tail padding to max alignment.
/// "packed"  = no alignment, no padding.
/// Returns {field_layouts, total_struct_size}.
inline std::pair<std::vector<FieldLayout>, size_t>
compute_field_layout(const std::vector<SchemaFieldDesc> &fields,
                     const std::string &packing)
{
    const bool packed = (packing == "packed");
    std::vector<FieldLayout> result;
    size_t offset    = 0;
    size_t max_align = 1;

    for (const auto &f : fields)
    {
        const bool is_blob = (f.type_str == "string" || f.type_str == "bytes");
        const bool is_bin  = is_blob || (f.count > 1);

        size_t esz   = is_blob ? static_cast<size_t>(f.length)
                               : field_elem_size(f.type_str);
        size_t align = (packed || is_blob) ? size_t{1} : field_align(f.type_str);
        size_t total = is_blob ? static_cast<size_t>(f.length)
                               : esz * static_cast<size_t>(f.count);

        if (align > 1)
            offset = (offset + align - 1) & ~(align - 1);
        max_align = std::max(max_align, align);

        result.push_back({offset, total, f.type_str, is_bin});
        offset += total;
    }

    // Pad struct total to max field alignment (aligned packing only).
    if (!packed && max_align > 1)
        offset = (offset + max_align - 1) & ~(max_align - 1);

    return {result, offset};
}

// ============================================================================
// FieldDef → SchemaFieldDesc conversion
// ============================================================================

/// Convert schema fields to layout descriptors for compute_field_layout().
/// Drops the field name — layout computation needs only type, count, and length.
inline std::vector<SchemaFieldDesc> to_field_descs(const std::vector<FieldDef> &fields)
{
    std::vector<SchemaFieldDesc> descs;
    descs.reserve(fields.size());
    for (const auto &f : fields)
        descs.push_back({f.type_str, f.count, f.length});
    return descs;
}

} // namespace pylabhub::hub
