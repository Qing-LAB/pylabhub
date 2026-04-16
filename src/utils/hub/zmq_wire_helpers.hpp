// src/utils/hub/zmq_wire_helpers.hpp
/**
 * @file zmq_wire_helpers.hpp
 * @brief Internal wire-format helpers shared by ZmqQueue and InboxQueue.
 *
 * Wire format: msgpack fixarray[5] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N), checksum:bin32]
 *   payload element i: scalar → native msgpack type; array/string/bytes → bin(byte_size)
 *
 * Field layout computation is in schema_field_layout.hpp (shared by all queue types).
 * This header adds ZMQ-specific msgpack pack/unpack on top of the shared layout.
 *
 * This header is INTERNAL to the hub/ translation units. Do not include from public headers.
 */
#pragma once

#include "utils/schema_field_layout.hpp"

#include <msgpack.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pylabhub::hub::wire_detail
{

// ============================================================================
// Magic constant
// ============================================================================

/// Frame magic: 'P','L','H','Q'
static constexpr uint32_t kFrameMagic = 0x51484C50u;

// ============================================================================
// Backward-compatible aliases
// ============================================================================

/// Bring shared types and functions into wire_detail for backward compatibility.
using ::pylabhub::hub::FieldLayout;
using ::pylabhub::hub::SchemaFieldDesc;
using ::pylabhub::hub::is_valid_type_str;
using ::pylabhub::hub::field_elem_size;
using ::pylabhub::hub::field_align;

/// WireFieldDesc is now FieldLayout (defined in schema_field_layout.hpp).
using WireFieldDesc = FieldLayout;

// ============================================================================
// Overload compute_field_layout for ZmqSchemaField (backward-compatible)
// ============================================================================

/// compute_field_layout taking ZmqSchemaField vector.
/// ZmqSchemaField is an alias for SchemaFieldDesc, so this delegates directly.
inline std::pair<std::vector<FieldLayout>, size_t>
compute_field_layout(const std::vector<ZmqSchemaField> &fields,
                     const std::string &packing)
{
    // ZmqSchemaField == SchemaFieldDesc, so just call the canonical version.
    const auto &base = reinterpret_cast<const std::vector<SchemaFieldDesc> &>(fields);
    return ::pylabhub::hub::compute_field_layout(base, packing);
}

/// Compute recv frame buffer size for schema mode.
/// Outer envelope: fixarray(1)+uint32(5)+bin8(10)+uint64(9) = 25 bytes.
/// Payload array header: 3 bytes.  Per scalar: 9 bytes max.  Per bin: 5+byte_size.
inline size_t max_frame_size(const std::vector<FieldLayout> &defs)
{
    size_t sz = 25 + 3 + 4 + 34; // outer + inner array header + slack + checksum(bin32)
    for (const auto &d : defs)
        sz += d.is_bin ? (5 + d.byte_size) : 9;
    return sz;
}

// ============================================================================
// Pack / unpack
// ============================================================================

/// Encode one FieldLayout from src buffer into msgpack packer.
inline void pack_field(msgpack::packer<msgpack::sbuffer> &pk,
                       const FieldLayout &fd, const char *src)
{
    const char *p = src + fd.offset;
    if (fd.is_bin)
    {
        pk.pack_bin(static_cast<uint32_t>(fd.byte_size));
        pk.pack_bin_body(p, fd.byte_size);
        return;
    }
    // Scalar — native msgpack type preserves wire type tag for validation.
    if      (fd.type_str == "bool")    pk.pack(*reinterpret_cast<const bool *>(p));
    else if (fd.type_str == "int8")    pk.pack(*reinterpret_cast<const int8_t *>(p));
    else if (fd.type_str == "uint8")   pk.pack(*reinterpret_cast<const uint8_t *>(p));
    else if (fd.type_str == "int16")   pk.pack(*reinterpret_cast<const int16_t *>(p));
    else if (fd.type_str == "uint16")  pk.pack(*reinterpret_cast<const uint16_t *>(p));
    else if (fd.type_str == "int32")   pk.pack(*reinterpret_cast<const int32_t *>(p));
    else if (fd.type_str == "uint32")  pk.pack(*reinterpret_cast<const uint32_t *>(p));
    else if (fd.type_str == "int64")   pk.pack(*reinterpret_cast<const int64_t *>(p));
    else if (fd.type_str == "uint64")  pk.pack(*reinterpret_cast<const uint64_t *>(p));
    else if (fd.type_str == "float32") pk.pack(*reinterpret_cast<const float *>(p));
    else if (fd.type_str == "float64") pk.pack(*reinterpret_cast<const double *>(p));
    // No else: factory validated all type strings at construction time.
}

/// Decode one msgpack object into dst buffer at fd.offset.
/// Returns false on type mismatch or bin size mismatch.
inline bool unpack_field(const msgpack::object &obj,
                         const FieldLayout &fd, char *dst) noexcept
{
    char *p = dst + fd.offset;
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
        if      (fd.type_str == "bool")    { bool     v; obj.convert(v); *reinterpret_cast<bool *>(p)     = v; }
        else if (fd.type_str == "int8")    { int8_t   v; obj.convert(v); *reinterpret_cast<int8_t *>(p)   = v; }
        else if (fd.type_str == "uint8")   { uint8_t  v; obj.convert(v); *reinterpret_cast<uint8_t *>(p)  = v; }
        else if (fd.type_str == "int16")   { int16_t  v; obj.convert(v); *reinterpret_cast<int16_t *>(p)  = v; }
        else if (fd.type_str == "uint16")  { uint16_t v; obj.convert(v); *reinterpret_cast<uint16_t *>(p) = v; }
        else if (fd.type_str == "int32")   { int32_t  v; obj.convert(v); *reinterpret_cast<int32_t *>(p)  = v; }
        else if (fd.type_str == "uint32")  { uint32_t v; obj.convert(v); *reinterpret_cast<uint32_t *>(p) = v; }
        else if (fd.type_str == "int64")   { int64_t  v; obj.convert(v); *reinterpret_cast<int64_t *>(p)  = v; }
        else if (fd.type_str == "uint64")  { uint64_t v; obj.convert(v); *reinterpret_cast<uint64_t *>(p) = v; }
        else if (fd.type_str == "float32") { float    v; obj.convert(v); *reinterpret_cast<float *>(p)    = v; }
        else if (fd.type_str == "float64") { double   v; obj.convert(v); *reinterpret_cast<double *>(p)   = v; }
        else return false;
    }
    catch (...) { return false; }
    return true;
}

// ============================================================================
// Frame envelope helpers (5-tuple: magic, tag, seq, payload, checksum)
// ============================================================================

/// Pack a complete 5-tuple frame into an sbuffer.
///
/// Caller computes the checksum and passes it in — Enforced vs Manual vs None
/// is the caller's concern; this helper just serializes the envelope.
inline void pack_frame(msgpack::packer<msgpack::sbuffer> &pk,
                       const std::array<uint8_t, 8> &schema_tag,
                       uint64_t seq,
                       const std::vector<WireFieldDesc> &defs,
                       const void *slot_data,
                       const uint8_t (&checksum)[32])
{
    pk.pack_array(5);
    pk.pack(kFrameMagic);
    pk.pack_bin(8);
    pk.pack_bin_body(reinterpret_cast<const char *>(schema_tag.data()), 8);
    pk.pack(seq);
    pk.pack_array(static_cast<uint32_t>(defs.size()));
    const char *src = static_cast<const char *>(slot_data);
    for (const auto &fd : defs)
        pack_field(pk, fd, src);
    pk.pack_bin(32);
    pk.pack_bin_body(reinterpret_cast<const char *>(checksum), 32);
}

/// Parsed frame envelope returned by unpack_envelope().
struct FrameEnvelope
{
    bool                    valid{false};
    const uint8_t          *recv_tag{nullptr};       ///< 8 bytes; points into msgpack buffer
    uint64_t                seq{0};
    const msgpack::object  *payload{nullptr};        ///< [3] — inner payload array
    uint32_t                payload_size{0};         ///< payload->via.array.size
    const uint8_t          *checksum{nullptr};       ///< 32 bytes; points into msgpack buffer
};

/// Validate and destructure a 5-tuple frame. Returns {valid=false} on any
/// structural error (wrong array size, bad magic, missing/mistyped fields).
/// Does NOT check schema-tag match or checksum correctness — callers handle
/// those with their own error-counting and rate-limiting policies.
inline FrameEnvelope unpack_envelope(const msgpack::object &obj) noexcept
{
    FrameEnvelope r;
    if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 5)
        return r;
    const auto *e = obj.via.array.ptr;

    // [0] magic
    uint32_t magic = 0;
    try { e[0].convert(magic); } catch (...) { return r; }
    if (magic != kFrameMagic) return r;

    // [1] schema_tag (bin, 8 bytes)
    if (e[1].type != msgpack::type::BIN || e[1].via.bin.size != 8) return r;
    r.recv_tag = reinterpret_cast<const uint8_t *>(e[1].via.bin.ptr);

    // [2] seq
    try { e[2].convert(r.seq); } catch (...) { return r; }

    // [3] payload array
    if (e[3].type != msgpack::type::ARRAY) return r;
    r.payload      = &e[3];
    r.payload_size = e[3].via.array.size;

    // [4] checksum (bin, 32 bytes)
    if (e[4].type != msgpack::type::BIN || e[4].via.bin.size != 32) return r;
    r.checksum = reinterpret_cast<const uint8_t *>(e[4].via.bin.ptr);

    r.valid = true;
    return r;
}

/// Decode all payload fields into a pre-zeroed destination buffer.
/// Returns false on the first field that fails to unpack (caller increments
/// error counter). dst must be at least as large as the slot item_size.
inline bool unpack_payload(const msgpack::object &payload_array,
                           const std::vector<WireFieldDesc> &defs,
                           void *dst) noexcept
{
    if (payload_array.via.array.size != defs.size()) return false;
    char *d = static_cast<char *>(dst);
    for (size_t i = 0; i < defs.size(); ++i)
        if (!unpack_field(payload_array.via.array.ptr[i], defs[i], d))
            return false;
    return true;
}

} // namespace pylabhub::hub::wire_detail
