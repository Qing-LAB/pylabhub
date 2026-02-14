#pragma once
/**
 * @file deterministic_checksum.hpp
 * @brief Helpers to build a deterministic byte blob from primitive values for hashing.
 *
 * Use these when you need a stable checksum of a logical "structure" (e.g. config or
 * header fields) that must match across processes or builds. Writing raw struct memory
 * is not safe (padding/alignment can differ). Instead, append each field in a fixed
 * order and byte format; then hash the resulting buffer with BLAKE2b (or similar).
 *
 * All multi-byte values are written little-endian. Callers define their "structure"
 * by the sequence of append_* calls.
 *
 * @example
 *   uint8_t buf[24];
 *   size_t off = 0;
 *   append_le_u32(buf, off, header->ring_buffer_capacity);
 *   append_le_u32(buf, off, header->physical_page_size);
 *   append_le_u64(buf, off, static_cast<uint64_t>(header->flexible_zone_size));
 *   append_u8(buf, off, header->checksum_type);
 *   // ... then: compute_blake2b(hash_out, buf, off);
 */
#include <cstddef>
#include <cstdint>

namespace pylabhub::utils
{

/** Append a uint32_t in little-endian order. Advances *offset by 4. */
inline void append_le_u32(uint8_t *buf, size_t &offset, uint32_t v) noexcept
{
    if (buf)
    {
        buf[offset] = static_cast<uint8_t>(v);
        buf[offset + 1] = static_cast<uint8_t>(v >> 8);
        buf[offset + 2] = static_cast<uint8_t>(v >> 16);
        buf[offset + 3] = static_cast<uint8_t>(v >> 24);
    }
    offset += 4;
}

/** Append a uint64_t in little-endian order. Advances *offset by 8. */
inline void append_le_u64(uint8_t *buf, size_t &offset, uint64_t v) noexcept
{
    append_le_u32(buf, offset, static_cast<uint32_t>(v));
    append_le_u32(buf, offset, static_cast<uint32_t>(v >> 32));
}

/** Append a single byte. Advances *offset by 1. */
inline void append_u8(uint8_t *buf, size_t &offset, uint8_t v) noexcept
{
    if (buf)
        buf[offset] = v;
    offset += 1;
}

} // namespace pylabhub::utils
