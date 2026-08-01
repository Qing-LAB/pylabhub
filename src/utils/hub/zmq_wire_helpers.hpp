// src/utils/hub/zmq_wire_helpers.hpp
/**
 * @file zmq_wire_helpers.hpp
 * @brief Internal wire-format helpers shared by ZmqQueue and InboxQueue.
 *
 * Wire format: msgpack fixarray[5] = [magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N),
 * checksum:bin32] payload element i: scalar → native msgpack type; array/string/bytes →
 * bin(byte_size)
 *
 * **This is the single source of truth for the 5-tuple frame.** Both ZmqQueue
 * (HEP-CORE-0021 §13) and InboxQueue (HEP-CORE-0027 §3) serialize through this
 * codec and reference this description rather than restating it.
 *
 * The `checksum` element is a BLAKE2b-256 over the decrypted message content,
 * governed by the owner-chosen `ChecksumPolicy` (None / Manual / Enforced;
 * `data_block_policy.hpp`).  It is **defence-in-depth on top of the mandatory
 * CURVE transport** — CURVE already authenticates every frame against tampering,
 * so the checksum mainly catches accidental corruption after decrypt; `None`
 * sends zeros and skips verification.  This helper is policy-agnostic: the
 * caller computes the checksum (or zeros) and passes it in.
 *
 * Field layout computation is in schema_field_layout.hpp (shared by all queue types).
 * This header adds ZMQ-specific msgpack pack/unpack on top of the shared layout.
 *
 * This header is INTERNAL to the hub/ translation units. Do not include from public headers.
 */
#pragma once

#include "utils/schema_field_layout.hpp"

#include <msgpack.hpp>

#include <algorithm>
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

/// Elements in the outer frame array: magic, schema_tag, seq, payload, checksum.
static constexpr std::size_t kFrameTupleSize = 5;

/// Deepest nesting a valid frame reaches: outer array → payload array.
/// Stated as 4 rather than 2 so a future field that nests one level does
/// not silently become a decode failure, while still refusing the
/// unbounded recursion msgpack allows by default.
static constexpr std::size_t kMaxFrameDepth = 4;

// ============================================================================
// Backward-compatible aliases
// ============================================================================

/// Bring shared types and functions into wire_detail for backward compatibility.
using ::pylabhub::hub::field_align;
using ::pylabhub::hub::field_elem_size;
using ::pylabhub::hub::FieldLayout;
using ::pylabhub::hub::is_valid_type_str;
using ::pylabhub::hub::SchemaFieldDesc;
using ::pylabhub::hub::compute_field_layout;

/// WireFieldDesc is now FieldLayout (defined in schema_field_layout.hpp).
using WireFieldDesc = FieldLayout;

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
inline void pack_field(msgpack::packer<msgpack::sbuffer> &pk, const FieldLayout &fd,
                       const char *src)
{
    const char *p = src + fd.offset;
    if (fd.is_bin)
    {
        pk.pack_bin(static_cast<uint32_t>(fd.byte_size));
        pk.pack_bin_body(p, fd.byte_size);
        return;
    }
    // Scalar — native msgpack type preserves wire type tag for validation.
    if (fd.type_str == "bool")
        pk.pack(*reinterpret_cast<const bool *>(p));
    else if (fd.type_str == "int8")
        pk.pack(*reinterpret_cast<const int8_t *>(p));
    else if (fd.type_str == "uint8")
        pk.pack(*reinterpret_cast<const uint8_t *>(p));
    else if (fd.type_str == "int16")
        pk.pack(*reinterpret_cast<const int16_t *>(p));
    else if (fd.type_str == "uint16")
        pk.pack(*reinterpret_cast<const uint16_t *>(p));
    else if (fd.type_str == "int32")
        pk.pack(*reinterpret_cast<const int32_t *>(p));
    else if (fd.type_str == "uint32")
        pk.pack(*reinterpret_cast<const uint32_t *>(p));
    else if (fd.type_str == "int64")
        pk.pack(*reinterpret_cast<const int64_t *>(p));
    else if (fd.type_str == "uint64")
        pk.pack(*reinterpret_cast<const uint64_t *>(p));
    else if (fd.type_str == "float32")
        pk.pack(*reinterpret_cast<const float *>(p));
    else if (fd.type_str == "float64")
        pk.pack(*reinterpret_cast<const double *>(p));
    // No else: factory validated all type strings at construction time.
}

/// Decode one msgpack object into dst buffer at fd.offset.
/// Returns false on type mismatch or bin size mismatch.
inline bool unpack_field(const msgpack::object &obj, const FieldLayout &fd, char *dst) noexcept
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
        if (fd.type_str == "bool")
        {
            bool v;
            obj.convert(v);
            *reinterpret_cast<bool *>(p) = v;
        }
        else if (fd.type_str == "int8")
        {
            int8_t v;
            obj.convert(v);
            *reinterpret_cast<int8_t *>(p) = v;
        }
        else if (fd.type_str == "uint8")
        {
            uint8_t v;
            obj.convert(v);
            *reinterpret_cast<uint8_t *>(p) = v;
        }
        else if (fd.type_str == "int16")
        {
            int16_t v;
            obj.convert(v);
            *reinterpret_cast<int16_t *>(p) = v;
        }
        else if (fd.type_str == "uint16")
        {
            uint16_t v;
            obj.convert(v);
            *reinterpret_cast<uint16_t *>(p) = v;
        }
        else if (fd.type_str == "int32")
        {
            int32_t v;
            obj.convert(v);
            *reinterpret_cast<int32_t *>(p) = v;
        }
        else if (fd.type_str == "uint32")
        {
            uint32_t v;
            obj.convert(v);
            *reinterpret_cast<uint32_t *>(p) = v;
        }
        else if (fd.type_str == "int64")
        {
            int64_t v;
            obj.convert(v);
            *reinterpret_cast<int64_t *>(p) = v;
        }
        else if (fd.type_str == "uint64")
        {
            uint64_t v;
            obj.convert(v);
            *reinterpret_cast<uint64_t *>(p) = v;
        }
        else if (fd.type_str == "float32")
        {
            float v;
            obj.convert(v);
            *reinterpret_cast<float *>(p) = v;
        }
        else if (fd.type_str == "float64")
        {
            double v;
            obj.convert(v);
            *reinterpret_cast<double *>(p) = v;
        }
        else
            return false;
    }
    catch (...)
    {
        return false;
    }
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
                       const std::array<uint8_t, 8> &schema_tag, uint64_t seq,
                       const std::vector<WireFieldDesc> &defs, const void *slot_data,
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

/// A validated 5-tuple frame, destructured.
///
/// Every pointer here is a VIEW into the msgpack zone owned by the
/// `DecodedFrame` this came from — see that type for the lifetime rule.
/// `valid == false` means nothing else in the struct is meaningful.
struct FrameEnvelope
{
    bool valid{false};
    const uint8_t *recv_tag{nullptr}; ///< 8 bytes; points into msgpack buffer
    uint64_t seq{0};
    const msgpack::object *payload{nullptr}; ///< [3] — inner payload array
    uint32_t payload_size{0};                ///< payload->via.array.size
    const uint8_t *checksum{nullptr};        ///< 32 bytes; points into msgpack buffer
};

/// A frame decoded from the wire: the msgpack zone plus the validated
/// envelope that views into it.
///
/// These are one type because `FrameEnvelope::recv_tag` / `payload` /
/// `checksum` point INTO the zone that `handle` owns.  Held as two
/// separate locals, keeping the handle alive for exactly as long as the
/// envelope is read is an unwritten rule every call site has to know.
/// Bundling them makes that lifetime structural instead.
///
/// Moving a `DecodedFrame` is safe: the zone is heap-allocated and the
/// handle moves a pointer to it, so the addresses the envelope holds do
/// not change.
///
/// The decoded frame owns its bytes and may outlive the buffer it was
/// read from — `decode_frame` passes no reference function, and msgpack
/// then copies every str/bin/ext into the zone rather than pointing at
/// the caller's buffer (`unpack.hpp:173-177`).
struct DecodedFrame
{
    msgpack::object_handle handle;
    FrameEnvelope env;

    /// True iff the bytes parsed AND destructured into a valid 5-tuple.
    explicit operator bool() const noexcept { return env.valid; }
};

/// Parse and destructure wire bytes into a validated frame, with every
/// msgpack allocation bounded by the input that asked for it.
///
/// **This is the only way to produce a valid `FrameEnvelope`** — not by
/// convention but by construction: the destructuring lives inside this
/// function and is not separately callable.  It used to be a public
/// `unpack_envelope()` sitting beside this comment, which made the
/// sentence a request rather than a fact.  Calling `msgpack::unpack`
/// yourself re-opens the allocation hole described below and re-creates
/// the handle/envelope lifetime pairing `DecodedFrame` exists to remove.
///
/// `msgpack::unpack`'s default `unpack_limit` is `0xffffffff` on every
/// axis, and msgpack allocates on a *declared* size before it discovers
/// the bytes behind it were never sent (`unpack.hpp:114` checks the
/// limit, `:124` then allocates `n * sizeof(msgpack::object)`).  A
/// forty-byte frame declaring a `0xffffffff`-element array therefore
/// asks a 64-bit host for roughly 68 GB.  The frame-size cap callers
/// apply upstream bounds the *input*; it does not bound what the input
/// is allowed to ask for.  These limits close that gap:
///
///   - **array** — `max(kFrameTupleSize, max_payload_fields)`.  msgpack
///     exposes ONE array limit covering every array in the message, and
///     the outer frame is always a 5-tuple, so the bound cannot go below
///     `kFrameTupleSize`.  For a schema with fewer than 5 fields a
///     slightly-oversized payload array therefore still decodes.  That is
///     deliberate and not a hole: this limit bounds ALLOCATION, and five
///     elements is not an allocation concern.  Exact field-count
///     enforcement is the caller's `payload_size != defs.size()` check,
///     which every caller performs immediately after decoding — so the
///     envelope reports the true `payload_size` precisely so the caller
///     CAN reject it.
///   - **map**, **ext** — the frame format contains neither. Zero.
///   - **str**, **bin** — a blob cannot exceed the buffer it was read
///     from, so `size` is exact rather than merely conservative.
///   - **depth** — `kMaxFrameDepth`.
///
/// Never throws.  msgpack reports a limit breach by throwing, which here
/// means the same thing as a structural failure — a malformed frame —
/// so both arrive as `env.valid == false` and the caller applies its own
/// error-counting and rate-limiting policy.
inline DecodedFrame decode_frame(const void *data, std::size_t size,
                                 std::size_t max_payload_fields) noexcept
{
    DecodedFrame out;
    const msgpack::unpack_limit limit{
        /*array*/ std::max(kFrameTupleSize, max_payload_fields),
        /*map  */ 0,
        /*str  */ size,
        /*bin  */ size,
        /*ext  */ 0,
        /*depth*/ kMaxFrameDepth};
    try
    {
        out.handle =
            msgpack::unpack(static_cast<const char *>(data), size, nullptr, nullptr, limit);
    }
    catch (...)
    {
        return out; // env.valid stays false — caller counts it as a frame error
    }

    // ── Destructure the 5-tuple ──────────────────────────────────────────
    //
    // Inlined rather than a separate `unpack_envelope()` helper: this is the
    // ONLY sanctioned way to produce a `FrameEnvelope`, and while that helper
    // existed as its own name the claim was merely a comment someone could
    // step around by unpacking themselves and calling it directly.  With no
    // second entry point the rule is structural.
    //
    // Checks structure only — array shape, magic, field types and sizes.
    // Schema-tag match and checksum correctness are deliberately NOT checked
    // here: each caller counts and rate-limits those on its own policy.
    const msgpack::object &obj = out.handle.get();
    if (obj.type != msgpack::type::ARRAY || obj.via.array.size != kFrameTupleSize)
        return out;
    const auto *e = obj.via.array.ptr;

    // [0] magic
    uint32_t magic = 0;
    try
    {
        e[0].convert(magic);
    }
    catch (...)
    {
        return out;
    }
    if (magic != kFrameMagic)
        return out;

    // [1] schema_tag (bin, 8 bytes)
    if (e[1].type != msgpack::type::BIN || e[1].via.bin.size != 8)
        return out;
    out.env.recv_tag = reinterpret_cast<const uint8_t *>(e[1].via.bin.ptr);

    // [2] seq
    try
    {
        e[2].convert(out.env.seq);
    }
    catch (...)
    {
        return out;
    }

    // [3] payload array
    if (e[3].type != msgpack::type::ARRAY)
        return out;
    out.env.payload = &e[3];
    out.env.payload_size = e[3].via.array.size;

    // [4] checksum (bin, 32 bytes)
    if (e[4].type != msgpack::type::BIN || e[4].via.bin.size != 32)
        return out;
    out.env.checksum = reinterpret_cast<const uint8_t *>(e[4].via.bin.ptr);

    out.env.valid = true;
    return out;
}

/// Decode all payload fields into a pre-zeroed destination buffer.
/// Returns false on the first field that fails to unpack (caller increments
/// error counter). dst must be at least as large as the slot item_size.
inline bool unpack_payload(const msgpack::object &payload_array,
                           const std::vector<WireFieldDesc> &defs, void *dst) noexcept
{
    if (payload_array.via.array.size != defs.size())
        return false;
    char *d = static_cast<char *>(dst);
    for (size_t i = 0; i < defs.size(); ++i)
        if (!unpack_field(payload_array.via.array.ptr[i], defs[i], d))
            return false;
    return true;
}

} // namespace pylabhub::hub::wire_detail
