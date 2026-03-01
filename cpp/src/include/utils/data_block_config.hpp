#pragma once
/**
 * @file data_block_config.hpp
 * @brief Layer 1 creation configuration for DataBlock segments.
 *
 * Provides DataBlockConfig (the POD-like struct passed to create_datablock_producer())
 * and DataBlockPageSize (physical allocation granularity), plus the timeout sentinel
 * constants used by acquire_*_slot().
 *
 * Includes data_block_policy.hpp — all policy enums (DataBlockPolicy,
 * ConsumerSyncPolicy, ChecksumType, ChecksumPolicy, DataBlockOpenMode, LoopPolicy)
 * are available when this header is included.
 *
 * Included automatically by data_block.hpp and data_block_fwd.hpp.
 * Users of the full DataBlock API should include data_block.hpp, not this file.
 */
#include "utils/data_block_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pylabhub::hub
{

// ============================================================================
// Physical Page Size
// ============================================================================

/**
 * @enum DataBlockPageSize
 * @brief Physical page size used as the allocation granularity for SHM slots.
 *
 * Each slot in the ring buffer is aligned to and sized as a multiple of
 * physical_page_size. Unset is a sentinel; every DataBlockConfig must supply
 * an explicit value before passing it to create_datablock_producer().
 */
enum class DataBlockPageSize : uint32_t
{
    Unset  = 0,          ///< Sentinel: must not be stored in header
    Size4K = 4096U,
    Size4M = 4194304U,
    Size16M = 16777216U
};

/**
 * @brief Return the byte count for a DataBlockPageSize value.
 * @return Size in bytes; 0 for Unset.
 */
inline size_t to_bytes(DataBlockPageSize u)
{
    return static_cast<size_t>(u);
}

// ============================================================================
// Slot Acquire Timeout Sentinels
// ============================================================================

/// @name Slot acquire timeout sentinel values (pass as timeout_ms to acquire_*_slot)
/// @{
inline constexpr int TIMEOUT_IMMEDIATE = 0;   ///< Non-blocking: return nullptr immediately if unavailable
inline constexpr int TIMEOUT_DEFAULT   = 100; ///< Default poll interval (100 ms)
inline constexpr int TIMEOUT_INFINITE  = -1;  ///< Block indefinitely until a slot is available
/// @}

// ============================================================================
// DataBlockConfig
// ============================================================================

/**
 * @struct DataBlockConfig
 * @brief Full creation configuration for a DataBlock shared-memory segment.
 *
 * Passed to create_datablock_producer() and find_datablock_consumer() factory
 * functions. All fields marked "must be set" will cause creation to fail or
 * produce undefined results if left at their sentinel/zero defaults.
 *
 * @note FlexibleZoneConfig (multi-zone) was removed in Phase 2 (2026-02-15).
 *       Use the single flex_zone_size field for flexible zone configuration.
 */
struct DataBlockConfig
{
    // ── Identity ──────────────────────────────────────────────────────────────
    std::string name; ///< SHM segment name; derived from channel_name in hub API.

    /** Access capability token. 0 = auto-generate random; non-zero = fixed for discovery. */
    uint64_t shared_secret = 0;

    // ── Memory Layout ─────────────────────────────────────────────────────────

    /** Physical page size (allocation granularity). Must be set explicitly. */
    DataBlockPageSize physical_page_size = DataBlockPageSize::Unset;

    /**
     * Logical slot size in bytes. Must be >= physical_page_size and a multiple of it.
     * 0 at config time means "use physical_page_size" (resolved before writing to header).
     * Stored value in the header is always >= physical_page_size (never 0).
     */
    size_t logical_unit_size = 0;

    /** Number of slots. 1=Single, 2=Double, N≥3=RingBuffer. 0 = unset, fails at create. */
    uint32_t ring_buffer_capacity = 0;

    // ── Policy Selection ──────────────────────────────────────────────────────

    /** Buffer management strategy. Must be set explicitly. */
    DataBlockPolicy policy = DataBlockPolicy::Unset;

    /** Reader synchronization contract. Must be set explicitly. */
    ConsumerSyncPolicy consumer_sync_policy = ConsumerSyncPolicy::Unset;

    /** Checksum algorithm. Default BLAKE2b. */
    ChecksumType checksum_type = ChecksumType::BLAKE2b;

    /** Checksum enforcement level. Default Enforced. */
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;

    // ── Flexible Zone ─────────────────────────────────────────────────────────

    /**
     * Single flexible zone size in bytes.
     * Must be 0 (no flex zone) or a multiple of 4096.
     */
    size_t flex_zone_size = 0;

    // ── Channel Identity ──────────────────────────────────────────────────────
    // Written into the SHM header at creation; empty = not set.
    // Empty strings are stored as all-zero fields (backward compatible).
    std::string hub_uid{};       ///< Hub unique ID (hex string, max 39 chars)
    std::string hub_name{};      ///< Hub human-readable name (max 63 chars)
    std::string producer_uid{};  ///< Producer unique ID (hex string, max 39 chars)
    std::string producer_name{}; ///< Producer human-readable name (max 63 chars)

    // ── Derived helpers ───────────────────────────────────────────────────────

    /** Effective slot stride in bytes. Returns physical_page_size when logical_unit_size == 0. */
    size_t effective_logical_unit_size() const
    {
        if (logical_unit_size != 0)
            return logical_unit_size;
        return to_bytes(physical_page_size);
    }

    /** Total ring buffer size in bytes (slot_count * effective_logical_unit_size). */
    size_t structured_buffer_size() const
    {
        uint32_t slots = (ring_buffer_capacity > 0) ? ring_buffer_capacity : 1U;
        return slots * effective_logical_unit_size();
    }
};

} // namespace pylabhub::hub
