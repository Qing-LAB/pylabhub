// src/utils/hub/zmq_wire_helpers.hpp
/**
 * @file zmq_wire_helpers.hpp
 * @brief Internal wire-format helpers shared by ZmqQueue and InboxQueue.
 *
 * Wire format: msgpack fixarray[4] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N)]
 *   payload element i: scalar → native msgpack type; array/string/bytes → bin(byte_size)
 *
 * This header is INTERNAL to the hub/ translation units. Do not include from public headers.
 */
#pragma once

#include "utils/hub_zmq_queue.hpp" // ZmqSchemaField

#include <msgpack.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pylabhub::hub::wire_detail
{

// ============================================================================
// Magic constant
// ============================================================================

/// Frame magic: 'P','L','H','Q'
/// Fixed at compile time — serves as a framing sanity check to detect frames
/// from non-pylabhub senders.  Application-level channel isolation uses
/// schema_tag (BLAKE2b-256) and ZMQ port separation, not the magic value.
static constexpr uint32_t kFrameMagic = 0x51484C50u;

// ============================================================================
// WireFieldDesc — computed layout for one field
// ============================================================================

/// Computed byte-level layout for a single schema field.
/// Produced by compute_field_layout(); consumed by pack_field()/unpack_field().
struct WireFieldDesc
{
    size_t      offset;    ///< byte offset in the struct buffer
    size_t      byte_size; ///< total bytes (count × elem_size, or length for string/bytes)
    std::string type_str;  ///< canonical type name, e.g. "float32"
    bool        is_bin;    ///< encode/decode as msgpack bin (arrays, string, bytes)
};

// ============================================================================
// Type-string helpers
// ============================================================================

inline bool is_valid_type_str(const std::string& t) noexcept
{
    return t == "bool"    || t == "int8"   || t == "uint8"  ||
           t == "int16"   || t == "uint16" || t == "int32"  ||
           t == "uint32"  || t == "int64"  || t == "uint64" ||
           t == "float32" || t == "float64"||
           t == "string"  || t == "bytes";
}

inline size_t field_elem_size(const std::string& t) noexcept
{
    if (t == "bool" || t == "int8"  || t == "uint8")          return 1;
    if (t == "int16" || t == "uint16")                         return 2;
    if (t == "int32" || t == "uint32" || t == "float32")       return 4;
    if (t == "int64" || t == "uint64" || t == "float64")       return 8;
    return 1; // string/bytes: caller uses length directly
}

inline size_t field_align(const std::string& t) noexcept
{
    if (t == "string" || t == "bytes") return 1;
    return field_elem_size(t);
}

// ============================================================================
// Layout computation
// ============================================================================

/// Compute per-field offsets using ctypes.LittleEndianStructure alignment rules.
/// Returns {field_layouts, total_struct_size}.
inline std::pair<std::vector<WireFieldDesc>, size_t>
compute_field_layout(const std::vector<ZmqSchemaField>& fields,
                     const std::string& packing)
{
    const bool packed = (packing == "packed");
    std::vector<WireFieldDesc> result;
    size_t offset    = 0;
    size_t max_align = 1;

    for (const auto& f : fields)
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

/// Compute recv frame buffer size for schema mode.
/// Outer envelope: fixarray(1)+uint32(5)+bin8(10)+uint64(9) = 25 bytes.
/// Payload array header: 3 bytes.  Per scalar: 9 bytes max.  Per bin: 5+byte_size.
inline size_t max_frame_size(const std::vector<WireFieldDesc>& defs)
{
    size_t sz = 25 + 3 + 4; // outer + inner array header + slack
    for (const auto& d : defs)
        sz += d.is_bin ? (5 + d.byte_size) : 9;
    return sz;
}

// ============================================================================
// Pack / unpack
// ============================================================================

/// Encode one WireFieldDesc from src buffer into msgpack packer.
inline void pack_field(msgpack::packer<msgpack::sbuffer>& pk,
                       const WireFieldDesc& fd, const char* src)
{
    const char* p = src + fd.offset;
    if (fd.is_bin)
    {
        pk.pack_bin(static_cast<uint32_t>(fd.byte_size));
        pk.pack_bin_body(p, fd.byte_size);
        return;
    }
    // Scalar — native msgpack type preserves wire type tag for validation.
    if      (fd.type_str == "bool")    pk.pack(*reinterpret_cast<const bool*>(p));
    else if (fd.type_str == "int8")    pk.pack(*reinterpret_cast<const int8_t*>(p));
    else if (fd.type_str == "uint8")   pk.pack(*reinterpret_cast<const uint8_t*>(p));
    else if (fd.type_str == "int16")   pk.pack(*reinterpret_cast<const int16_t*>(p));
    else if (fd.type_str == "uint16")  pk.pack(*reinterpret_cast<const uint16_t*>(p));
    else if (fd.type_str == "int32")   pk.pack(*reinterpret_cast<const int32_t*>(p));
    else if (fd.type_str == "uint32")  pk.pack(*reinterpret_cast<const uint32_t*>(p));
    else if (fd.type_str == "int64")   pk.pack(*reinterpret_cast<const int64_t*>(p));
    else if (fd.type_str == "uint64")  pk.pack(*reinterpret_cast<const uint64_t*>(p));
    else if (fd.type_str == "float32") pk.pack(*reinterpret_cast<const float*>(p));
    else if (fd.type_str == "float64") pk.pack(*reinterpret_cast<const double*>(p));
    // No else: factory validated all type strings at construction time.
}

/// Decode one msgpack object into dst buffer at fd.offset.
/// Returns false on type mismatch or bin size mismatch.
inline bool unpack_field(const msgpack::object& obj,
                         const WireFieldDesc& fd, char* dst) noexcept
{
    char* p = dst + fd.offset;
    try
    {
        if (fd.is_bin)
        {
            if (obj.type != msgpack::type::BIN || obj.via.bin.size != fd.byte_size)
                return false;
            std::memcpy(p, obj.via.bin.ptr, fd.byte_size);
            return true;
        }
        // Scalar: msgpack::convert() checks type compatibility and throws on mismatch.
        if      (fd.type_str == "bool")    { bool     v; obj.convert(v); *reinterpret_cast<bool*>(p)     = v; }
        else if (fd.type_str == "int8")    { int8_t   v; obj.convert(v); *reinterpret_cast<int8_t*>(p)   = v; }
        else if (fd.type_str == "uint8")   { uint8_t  v; obj.convert(v); *reinterpret_cast<uint8_t*>(p)  = v; }
        else if (fd.type_str == "int16")   { int16_t  v; obj.convert(v); *reinterpret_cast<int16_t*>(p)  = v; }
        else if (fd.type_str == "uint16")  { uint16_t v; obj.convert(v); *reinterpret_cast<uint16_t*>(p) = v; }
        else if (fd.type_str == "int32")   { int32_t  v; obj.convert(v); *reinterpret_cast<int32_t*>(p)  = v; }
        else if (fd.type_str == "uint32")  { uint32_t v; obj.convert(v); *reinterpret_cast<uint32_t*>(p) = v; }
        else if (fd.type_str == "int64")   { int64_t  v; obj.convert(v); *reinterpret_cast<int64_t*>(p)  = v; }
        else if (fd.type_str == "uint64")  { uint64_t v; obj.convert(v); *reinterpret_cast<uint64_t*>(p) = v; }
        else if (fd.type_str == "float32") { float    v; obj.convert(v); *reinterpret_cast<float*>(p)    = v; }
        else if (fd.type_str == "float64") { double   v; obj.convert(v); *reinterpret_cast<double*>(p)   = v; }
        else return false;
    }
    catch (...) { return false; }
    return true;
}

} // namespace pylabhub::hub::wire_detail
