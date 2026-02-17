#pragma once
/**
 * @file data_block.hpp
 * @brief Shared memory data block with producer/consumer coordination.
 *
 * Design: Single shared-memory block, counters/flags, slot iterator.
 * All public classes use pImpl for ABI stability.
 * See docs/HEP/HEP-CORE-0002-DataHub-FINAL.md for complete design specification.
 *
 * @par Lifecycle
 * create_datablock_producer() and find_datablock_consumer() require the Data Exchange Hub
 * module to be initialized. In main(), create a LifecycleGuard with
 * pylabhub::hub::GetLifecycleModule() (and typically Logger, CryptoUtils). See hubshell.cpp
 * or docs/IMPLEMENTATION_GUIDANCE.md.
 */
#include "pylabhub_utils_export.h"
#include "shared_memory_spinlock.hpp" // SharedSpinLockState and SharedSpinLock (abstraction over shared memory)
#include "schema_blds.hpp"         // For SchemaInfo and generate_schema_info

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "slot_rw_coordinator.h" // Include C interface header (lock/multithread safety is user-managed at C API level)

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::hub
{

// ============================================================================
// Phase 3: C++ RAII Layer - Forward Declarations
// ============================================================================

// Forward declarations for RAII layer types
template <typename T, typename E> class Result;
template <typename DataBlockT, bool IsMutable> class SlotRef;
template <typename FlexZoneT, bool IsMutable> class ZoneRef;
template <typename FlexZoneT, typename DataBlockT, bool IsWrite> class TransactionContext;
template <typename DataBlockT, bool IsWrite> class SlotIterator;

// Type aliases
template <typename FlexZoneT, typename DataBlockT>
using WriteTransactionContext = TransactionContext<FlexZoneT, DataBlockT, true>;

template <typename FlexZoneT, typename DataBlockT>
using ReadTransactionContext = TransactionContext<FlexZoneT, DataBlockT, false>;

// ============================================================================
// SharedMemoryHeader Layout Constants (Version 1.0)
// ============================================================================
// CRITICAL: These constants define the ABI layout of SharedMemoryHeader.
// Changing these values requires incrementing DATABLOCK_VERSION_MAJOR.
// All size calculations must use these constants, never hardcoded literals.

namespace detail
{
// Version 1.0 layout constants
inline constexpr uint16_t HEADER_VERSION_MAJOR = 1;
inline constexpr uint16_t HEADER_VERSION_MINOR = 0;

// Fixed pool sizes (changing these breaks ABI compatibility)
inline constexpr size_t MAX_SHARED_SPINLOCKS = 8;
inline constexpr size_t MAX_CONSUMER_HEARTBEATS = 8;
/** Max number of flexible zone checksum entries in the header (must match array size). */
inline constexpr size_t MAX_FLEXIBLE_ZONE_CHECKSUMS = 8;

// Size assertions (verify at compile time)
static_assert(sizeof(SharedSpinLockState) == 32, "SharedSpinLockState must be 32 bytes");
static_assert(MAX_SHARED_SPINLOCKS == 8, "V1.0 requires exactly 8 spinlocks");
static_assert(MAX_CONSUMER_HEARTBEATS == 8, "V1.0 requires exactly 8 consumer heartbeat slots");
static_assert(MAX_FLEXIBLE_ZONE_CHECKSUMS == 8,
              "V1.0 requires exactly 8 flexible zone checksum slots");
inline constexpr size_t CHECKSUM_BYTES = 32;
inline constexpr size_t SLOT_CHECKSUM_ENTRY_SIZE = 33; // 32 hash + 1 valid byte

/** Offset within reserved_header[] where header layout hash is stored (for protocol check). */
inline constexpr size_t HEADER_LAYOUT_HASH_OFFSET = 0;
/** Size in bytes of the header layout hash (BLAKE2b-256). */
inline constexpr size_t HEADER_LAYOUT_HASH_SIZE = 32;
/** Offset within reserved_header[] where segment layout checksum is stored (layout-defining values). */
inline constexpr size_t LAYOUT_CHECKSUM_OFFSET = 32;
/** Size in bytes of the layout checksum (BLAKE2b-256). */
inline constexpr size_t LAYOUT_CHECKSUM_SIZE = 32;
/** Offset in reserved_header[] for Sync_reader per-consumer next-read slot ids (8 * uint64_t). */
inline constexpr size_t CONSUMER_READ_POSITIONS_OFFSET = 64;
/** Offset in reserved_header[] for producer heartbeat: producer_id (uint64_t), producer_last_heartbeat_ns (uint64_t). */
inline constexpr size_t PRODUCER_HEARTBEAT_OFFSET = 128;
/** Staleness threshold: if (now - last_heartbeat_ns) > this, heartbeat is stale; fall back to is_process_alive. */
inline constexpr uint64_t PRODUCER_HEARTBEAT_STALE_THRESHOLD_NS = 5'000'000'000ULL; // 5 seconds

/** DataBlock shared memory header magic number ('PLHB'). */
inline constexpr uint32_t DATABLOCK_MAGIC_NUMBER = 0x504C4842;

/**
 * @brief Checks that the header magic number matches the expected value (acquire load).
 * @param magic_ptr Pointer to SharedMemoryHeader::magic_number (may be null).
 * @param expected Expected value (e.g. detail::DATABLOCK_MAGIC_NUMBER).
 */
inline bool is_header_magic_valid(const std::atomic<uint32_t> *magic_ptr,
                                   uint32_t expected) noexcept
{
    return magic_ptr && magic_ptr->load(std::memory_order_acquire) == expected;
}

} // namespace detail

/** Returns true if writer (pid) is alive. Uses producer heartbeat if fresh; otherwise is_process_alive.
 * Use for liveness checks: only fall back to PID check when heartbeat is missing or stale. */
PYLABHUB_UTILS_EXPORT bool is_writer_alive(const SharedMemoryHeader *header, uint64_t pid) noexcept;

// Forward declarations
class MessageHub;
struct DataBlockProducerImpl;
struct DataBlockConsumerImpl;
struct DataBlockDiagnosticHandleImpl;

/**
 * @struct SlotRWState
 * @brief Per-slot coordination state in shared memory (48 bytes, cache-aligned).
 *
 * ABI-sensitive: layout and size must match C API and recovery tools.
 * write_lock is PID-based; 0 means free. Reader path uses double-check (TOCTTOU
 * mitigation); see acquire_read and HEP-CORE-0002.
 */
struct PYLABHUB_UTILS_EXPORT alignas(64) SlotRWState
{
    // === Writer Coordination ===
    std::atomic<uint64_t> write_lock; ///< PID-based exclusive lock (0 = free)

    // === Reader Coordination ===
    std::atomic<uint32_t> reader_count; // Active readers (multi-reader)

    // === State Machine ===
    enum class SlotState : uint8_t
    {
        FREE = 0,      // Available for writing
        WRITING = 1,   // Producer is writing
        COMMITTED = 2, // Data ready for reading
        DRAINING = 3   // Waiting for readers to finish (wrap-around)
    };
    std::atomic<SlotState> slot_state;

    // === Backpressure and Coordination ===
    std::atomic<uint8_t> writer_waiting; // Producer blocked on readers

    // === TOCTTOU Detection ===
    std::atomic<uint64_t> write_generation; // Incremented on each commit

    // === Padding ===
    uint8_t padding[24]; // Pad to 48 bytes
};

constexpr size_t raw_size_SlotRWState =
    offsetof(SlotRWState, padding) + sizeof(SlotRWState::padding);
static_assert(raw_size_SlotRWState == 48, "SlotRWState must be 48 bytes");
static_assert(alignof(SlotRWState) >= 64, "SlotRWState should be cache-line aligned");

/** @enum DataBlockPageSize
 *  @brief Physical page size for allocation. Each slot is aligned to page boundaries.
 *  Unset is a sentinel; must not be stored in header; config must set explicitly. */
enum class DataBlockPageSize : uint32_t
{
    Unset = 0,       ///< Sentinel: must not be stored in header
    Size4K = 4096u,
    Size4M = 4194304u,
    Size16M = 16777216u
};

/** @brief Return byte size for DataBlockPageSize (e.g. 4096 for Size4K).
 *  @return Size in bytes; 0 for Unset. */
inline size_t to_bytes(DataBlockPageSize u)

{

    return static_cast<size_t>(u);
}

/**

 * @enum DataBlockPolicy

 * @brief Defines the buffer management strategy for a DataBlock.

 */

enum class DataBlockPolicy : uint32_t
{
    Single = 0,
    DoubleBuffer = 1,
    RingBuffer = 2,
    Unset = 255 // Sentinel: must not be stored in header; config must set Single/DoubleBuffer/RingBuffer
};

/**
 * @enum ChecksumType
 * @brief Algorithm used for slot and flexible-zone checksums. Always one active.
 */
enum class ChecksumType
{
    BLAKE2b = 0,
    Unset = 255 // Sentinel: must not be stored; config must set BLAKE2b (or future algorithm)
};

/**
 * @enum ChecksumPolicy
 * @brief When to run update/verify. Checksum storage is always present (ChecksumType).
 *
 * - None: No enforcement (update/verify are no-ops or optional).
 * - Manual: Caller must call update_checksum_* and verify_checksum_* explicitly.
 * - Enforced: System automatically updates on release_write_slot and verifies on release_consume_slot.
 */
enum class ChecksumPolicy
{
    None,    // No checksum enforcement
    Manual,  // User calls update/verify explicitly
    Enforced // Automatic update on write release, verify on consume release
};

/**
 * @enum ConsumerSyncPolicy
 * @brief How the reader(s) advance and when the writer may overwrite slots.
 *
 * - Latest_only: Reader only follows the latest committed slot; older slots may be
 *   overwritten. No per-consumer or global read_index; writer never blocks on readers.
 * - Single_reader: One consumer only. One shared read_index (tail); consumer reads in
 *   order; writer blocks when (write_index - read_index) >= capacity. Simplest ordered mode.
 * - Sync_reader: Multiple consumers. Per-consumer positions; read_index = min(positions).
 *   Iterator blocks until slowest reader has consumed; writer blocks when ring full.
 */
enum class ConsumerSyncPolicy : uint32_t
{
    Latest_only = 0,  // Reader follows latest committed only; writer can overwrite
    Single_reader = 1, // One consumer, one read_index; read in order; writer blocks when full
    Sync_reader = 2,   // Multi-consumer; read_index = min(positions); writer blocks when full
    Unset = 255        // Sentinel: must not be stored in header; config must set one of the above
};

/**
 * @struct DataBlockConfig
 * @brief Configuration for creating a new DataBlock.
 * 
 * @note FlexibleZoneConfig was removed in Phase 2 refactoring (2026-02-15).
 *       Use flex_zone_size field instead for single flexible zone configuration.
 */

struct DataBlockConfig
{

    std::string name;

    /** 0 = generate random; non-zero = use for discovery/capability. */
    uint64_t shared_secret = 0;

    /** Physical page size (allocation granularity). Must be set explicitly (no default). */
    DataBlockPageSize physical_page_size = DataBlockPageSize::Unset;
    /**
     * Logical slot size (bytes per ring-buffer slot). Must be >= physical_page_size and a multiple
     * of physical_page_size. 0 at config input means "use physical" (resolved to physical_page_size).
     * Stored value is always >= physical_page_size (never 0).
     */
    size_t logical_unit_size = 0;

    /** Slot count: 1=Single, 2=Double, N=RingBuffer. Must be set explicitly (>= 1); 0 = unset, will fail at create. */
    uint32_t ring_buffer_capacity = 0;

    /** Buffer policy. Must be set explicitly (no default). */
    DataBlockPolicy policy = DataBlockPolicy::Unset;

    /** Consumer sync policy. Must be set explicitly (no default). */
    ConsumerSyncPolicy consumer_sync_policy = ConsumerSyncPolicy::Unset;

    /** Checksum algorithm. Always present; default BLAKE2b. */
    ChecksumType checksum_type = ChecksumType::BLAKE2b;

    /** When to update/verify checksums. */
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;

    /**
     * Single flexible zone size in bytes.
     * Must be 0 (no flex zone) or a multiple of 4096 (page size).
     * This replaces the old flexible_zone_configs vector.
     */
    size_t flex_zone_size = 0;

    /** Effective logical unit size (slot stride in bytes). 0 at config means use physical_page_size. */
    size_t effective_logical_unit_size() const
    {
        if (logical_unit_size != 0)
            return logical_unit_size;
        return to_bytes(physical_page_size);
    }

    /** Total structured buffer size (slot_count * effective_logical). slot_count >= 1, so at least one logical unit fits. */
    size_t structured_buffer_size() const
    {
        uint32_t slots = (ring_buffer_capacity > 0) ? ring_buffer_capacity : 1u;
        return slots * effective_logical_unit_size();
    }
};

/**

 * @struct SharedMemoryHeader

 * @brief The header structure for every DataBlock shared memory segment.

 *

 * Single-block design. Expansion is handled by creating a new larger block and

 * handing over to it (old block remains valid until all consumers detach).

 * Layout is ABI-sensitive (4KB alignment). See HEP-CORE-0002.

 */
struct alignas(4096) SharedMemoryHeader
{
    // === Identification and Versioning ===
    std::atomic<uint32_t> magic_number; // 0x504C4842 ('PLHB')
    uint16_t version_major;             // ABI compatibility
    uint16_t version_minor;
    uint64_t total_block_size; // Total shared memory size

    // === Security and Schema ===
    uint8_t shared_secret[64]; // Access capability token
    
    // Phase 4: Dual schema support (FlexZone + DataBlock)
    uint8_t flexzone_schema_hash[32];   // BLAKE2b hash of FlexZone schema
    uint8_t datablock_schema_hash[32];  // BLAKE2b hash of DataBlock/slot schema
    uint32_t schema_version;            // Schema version number
    
    // Note: Old layout had schema_hash[32] + padding_sec[28] = 60 bytes
    // New layout: flexzone_schema_hash[32] + datablock_schema_hash[32] + schema_version[4] = 68 bytes
    // No padding needed (net +8 bytes absorbed from reserved_header padding at end)

    // === Ring Buffer Configuration ===
    DataBlockPolicy policy;               // Single/DoubleBuffer/RingBuffer
    ConsumerSyncPolicy consumer_sync_policy; // Latest_only / Single_reader / Sync_reader
    uint32_t physical_page_size;          // Physical page size (bytes); allocation granularity
    uint32_t logical_unit_size;           // Logical slot size (bytes); always >= physical_page_size (legacy 0 = use physical)
    uint32_t ring_buffer_capacity;        // Number of slots
    uint32_t flexible_zone_size;    // Total TABLE 1 size (u32: 4 GB max is sufficient for metadata)
    uint8_t checksum_type;          // ChecksumType; always present (BLAKE2b)
    ChecksumPolicy checksum_policy;

    // === Ring Buffer State (Hot Path) ===
    std::atomic<uint64_t> write_index;  // Next slot to write (producer)
    std::atomic<uint64_t> commit_index; // Last committed slot (producer)
    std::atomic<uint64_t> read_index;   // Oldest unread slot (system)
    std::atomic<uint32_t> active_consumer_count;

    // === Metrics Section (256 bytes) ===
    // Slot Coordination (64 bytes)
    std::atomic<uint64_t> writer_timeout_count;        // Total writer timeouts (all causes)
    std::atomic<uint64_t> writer_lock_timeout_count;   // Timeouts while waiting for write_lock
    std::atomic<uint64_t> writer_reader_timeout_count; // Timeouts while waiting for readers to drain
    std::atomic<uint64_t> writer_blocked_total_ns;
    std::atomic<uint64_t> write_lock_contention;
    std::atomic<uint64_t> write_generation_wraps;
    std::atomic<uint64_t> reader_not_ready_count;
    std::atomic<uint64_t> reader_race_detected;
    std::atomic<uint64_t> reader_validation_failed;
    std::atomic<uint64_t> reader_peak_count;
    std::atomic<uint64_t> reader_timeout_count;

    // Error Tracking (96 bytes)
    std::atomic<uint64_t> last_error_timestamp_ns;
    std::atomic<uint32_t> last_error_code;
    std::atomic<uint32_t> error_sequence;
    std::atomic<uint64_t> slot_acquire_errors;
    std::atomic<uint64_t> slot_commit_errors;
    std::atomic<uint64_t> checksum_failures;
    std::atomic<uint64_t> zmq_send_failures;
    std::atomic<uint64_t> zmq_recv_failures;
    std::atomic<uint64_t> zmq_timeout_count;
    std::atomic<uint64_t> recovery_actions_count;
    std::atomic<uint64_t> schema_mismatch_count;
    std::atomic<uint64_t> reserved_errors[2];

    // Heartbeat Statistics (32 bytes)
    std::atomic<uint64_t> heartbeat_sent_count;
    std::atomic<uint64_t> heartbeat_failed_count;
    std::atomic<uint64_t> last_heartbeat_ns;
    std::atomic<uint64_t> reserved_hb;

    // Performance Counters (64 bytes)
    std::atomic<uint64_t> total_slots_written;
    std::atomic<uint64_t> total_slots_read;
    std::atomic<uint64_t> total_bytes_written;
    std::atomic<uint64_t> total_bytes_read;
    std::atomic<uint64_t> uptime_seconds;
    std::atomic<uint64_t> creation_timestamp_ns;
    std::atomic<uint64_t> reserved_perf[2];

    // === Consumer Heartbeats (512 bytes) ===
    struct ConsumerHeartbeat
    {
        std::atomic<uint64_t> consumer_id;       // PID or UUID
        std::atomic<uint64_t> last_heartbeat_ns; // Monotonic timestamp
        uint8_t padding[48];                     // Cache line (64 bytes total)
    } consumer_heartbeats[detail::MAX_CONSUMER_HEARTBEATS];

    // === SharedSpinLock States (256 bytes) ===
    SharedSpinLockState spinlock_states[detail::MAX_SHARED_SPINLOCKS];

    // === Flexible zone checksums (MAX_FLEXIBLE_ZONE_CHECKSUMS * 64 bytes) ===
    struct FlexibleZoneChecksumEntry
    {
        uint8_t checksum_bytes[32];
        std::atomic<uint8_t> valid{0};
        uint8_t padding[31];
    } flexible_zone_checksums[detail::MAX_FLEXIBLE_ZONE_CHECKSUMS];

    // === Padding to 4096 bytes ===
    // reserved_header size chosen so total header is exactly 4KB.
    uint8_t reserved_header[2320]; // 4096 - (offset up to here); exact size for 4KB total
};
constexpr size_t raw_size_SharedMemoryHeader =
    offsetof(SharedMemoryHeader, reserved_header) + sizeof(SharedMemoryHeader::reserved_header);
static_assert(raw_size_SharedMemoryHeader == 4096, "Header must be exactly 4KB");
static_assert(alignof(SharedMemoryHeader) >= 4096,
              "SharedMemoryHeader should be page-border aligned");

/**
 * Schema field list for SharedMemoryHeader — canonical order and types for schema hash.
 * Use as: PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS(OP) with OP(member, type_id) defined.
 * Each entry supplies both name (member) and type (type_id); default practice for schema.
 * Kept next to the struct so fields/types stay correlated; .cpp uses this to build SchemaInfo.
 * (Four trailing fields have dynamic type_id and are added in .cpp after this list.)
 */
#define PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS(OP)                                      \
    /* Identification and Versioning */                                                      \
    OP(magic_number, "u32")                                                                   \
    OP(version_major, "u16")                                                                  \
    OP(version_minor, "u16")                                                                  \
    OP(total_block_size, "u64")                                                               \
    /* Security and Schema (Phase 4: Dual schema) */                                         \
    OP(shared_secret, "u8[64]")                                                               \
    OP(flexzone_schema_hash, "u8[32]")                                                        \
    OP(datablock_schema_hash, "u8[32]")                                                       \
    OP(schema_version, "u32")                                                                 \
    /* Ring Buffer Configuration */                                                           \
    OP(policy, "u32")                                                                         \
    OP(consumer_sync_policy, "u32")                                                          \
    OP(physical_page_size, "u32")                                                             \
    OP(logical_unit_size, "u32")                                                               \
    OP(ring_buffer_capacity, "u32")                                                           \
    OP(flexible_zone_size, "u32")                                                             \
    OP(checksum_type, "u8")                                                                     \
    OP(checksum_policy, "u32")                                                                \
    /* Ring Buffer State (Hot Path) */                                                         \
    OP(write_index, "u64")                                                                    \
    OP(commit_index, "u64")                                                                   \
    OP(read_index, "u64")                                                                     \
    OP(active_consumer_count, "u32")                                                           \
    /* Metrics: Slot Coordination */                                                          \
    OP(writer_timeout_count, "u64")                                                           \
    OP(writer_lock_timeout_count, "u64")                                                      \
    OP(writer_reader_timeout_count, "u64")                                                    \
    OP(writer_blocked_total_ns, "u64")                                                         \
    OP(write_lock_contention, "u64")                                                          \
    OP(write_generation_wraps, "u64")                                                          \
    OP(reader_not_ready_count, "u64")                                                          \
    OP(reader_race_detected, "u64")                                                           \
    OP(reader_validation_failed, "u64")                                                        \
    OP(reader_peak_count, "u64")                                                               \
    OP(reader_timeout_count, "u64")                                                           \
    /* Metrics: Error Tracking */                                                             \
    OP(last_error_timestamp_ns, "u64")                                                        \
    OP(last_error_code, "u32")                                                                 \
    OP(error_sequence, "u32")                                                                  \
    OP(slot_acquire_errors, "u64")                                                             \
    OP(slot_commit_errors, "u64")                                                             \
    OP(checksum_failures, "u64")                                                               \
    OP(zmq_send_failures, "u64")                                                               \
    OP(zmq_recv_failures, "u64")                                                               \
    OP(zmq_timeout_count, "u64")                                                              \
    OP(recovery_actions_count, "u64")                                                          \
    OP(schema_mismatch_count, "u64")                                                          \
    OP(reserved_errors, "u64[2]")                                                             \
    /* Metrics: Heartbeat */                                                                   \
    OP(heartbeat_sent_count, "u64")                                                            \
    OP(heartbeat_failed_count, "u64")                                                          \
    OP(last_heartbeat_ns, "u64")                                                               \
    OP(reserved_hb, "u64")                                                                     \
    /* Metrics: Performance */                                                                \
    OP(total_slots_written, "u64")                                                            \
    OP(total_slots_read, "u64")                                                                \
    OP(total_bytes_written, "u64")                                                             \
    OP(total_bytes_read, "u64")                                                                \
    OP(uptime_seconds, "u64")                                                                  \
    OP(creation_timestamp_ns, "u64")                                                          \
    OP(reserved_perf, "u64[2]")                                                                \
    /* (consumer_heartbeats, spinlock_states, flexible_zone_checksums, reserved_header: dynamic type_id in .cpp) */

namespace detail
{
/** Effective logical slot size from header (bytes per slot). Legacy: 0 in header means use physical_page_size. */
inline uint32_t get_slot_stride_bytes(const SharedMemoryHeader *h) noexcept
{
    return h && h->logical_unit_size != 0 ? h->logical_unit_size : (h ? h->physical_page_size : 0u);
}
/** Effective slot count from header (single place for "capacity 0 means 1" convention). */
inline uint32_t get_slot_count(const SharedMemoryHeader *h) noexcept
{
    return h && h->ring_buffer_capacity > 0 ? h->ring_buffer_capacity : 1u;
}
} // namespace detail

// Forward declarations for slot handles (primitive data transfer API)
struct SlotWriteHandleImpl;
struct SlotConsumeHandleImpl;
struct DataBlockSlotIteratorImpl;

/**
 * @note Phase 2 refactoring (2026-02-15): FlexibleZoneInfo struct removed.
 *       Single flexible zone design no longer requires this structure.
 *       Use DataBlockProducer::flexible_zone_span() for single zone access.
 */

/**
 * @class SlotWriteHandle
 * @brief Primitive write handle for a single data slot (producer).
 *
 * @par Lifetime contract
 * The handle holds pointers into the DataBlock's shared memory. You must release or destroy
 * all SlotWriteHandle instances (via release_write_slot or handle destruction) before
 * destroying the DataBlockProducer. Destroying the producer while handles exist causes
 * use-after-free.
 */
class PYLABHUB_UTILS_EXPORT SlotWriteHandle
{
  public:
    SlotWriteHandle();
    ~SlotWriteHandle() noexcept;
    SlotWriteHandle(SlotWriteHandle &&other) noexcept;
    SlotWriteHandle &operator=(SlotWriteHandle &&other) noexcept;
    SlotWriteHandle(const SlotWriteHandle &) = delete;
    SlotWriteHandle &operator=(const SlotWriteHandle &) = delete;

    /** @brief Slot index within the ring buffer. */
    size_t slot_index() const noexcept;
    /** @brief Monotonic slot id (write_index value). */
    uint64_t slot_id() const noexcept;

    /** @brief Mutable view of the slot buffer. */
    std::span<std::byte> buffer_span() noexcept;
    /** 
     * @brief Mutable view of the flexible zone.
     * @return Empty span if zone is not configured (size == 0).
     * 
     * @note Phase 2: Single flex zone design. Old multi-zone index parameter removed.
     */
    std::span<std::byte> flexible_zone_span() noexcept;

    /** @brief Copy into slot buffer with bounds check. */
    [[nodiscard]] bool write(const void *src, size_t len, size_t offset = 0) noexcept;
    /** @brief Commit written data; makes it visible to consumers. */
    [[nodiscard]] bool commit(size_t bytes_written) noexcept;

    /** @brief Update checksum for this slot (if enabled). */
    [[nodiscard]] bool update_checksum_slot() noexcept;
    /** 
     * @brief Update checksum for flexible zone (if enabled).
     * @note Phase 2: Single flex zone design. Old index parameter removed.
     */
    [[nodiscard]] bool update_checksum_flexible_zone() noexcept;

  private:
    friend class DataBlockProducer;
    explicit SlotWriteHandle(std::unique_ptr<SlotWriteHandleImpl> impl);
    std::unique_ptr<SlotWriteHandleImpl> pImpl;
};

/**
 * @class SlotConsumeHandle
 * @brief Primitive read handle for a single data slot (consumer).
 *
 * @par Lifetime contract
 * The handle holds pointers into the DataBlock's shared memory. You must release or destroy
 * all SlotConsumeHandle instances (via release_consume_slot or handle destruction) before
 * destroying the DataBlockConsumer or DataBlockProducer. Destroying the consumer or producer
 * while handles exist causes use-after-free.
 */
class PYLABHUB_UTILS_EXPORT SlotConsumeHandle
{
  public:
    SlotConsumeHandle();
    ~SlotConsumeHandle() noexcept;
    SlotConsumeHandle(SlotConsumeHandle &&other) noexcept;
    SlotConsumeHandle &operator=(SlotConsumeHandle &&other) noexcept;
    SlotConsumeHandle(const SlotConsumeHandle &) = delete;
    SlotConsumeHandle &operator=(const SlotConsumeHandle &) = delete;

    /** @brief Slot index within the ring buffer. */
    size_t slot_index() const noexcept;
    /** @brief Monotonic slot id (commit_index value). */
    uint64_t slot_id() const noexcept;

    /** @brief Read-only view of the slot buffer. */
    std::span<const std::byte> buffer_span() const noexcept;
    /** 
     * @brief Read-only view of the flexible zone.
     * @return Empty span if zone is not configured (size == 0).
     * 
     * @note Phase 2: Single flex zone design. Old multi-zone index parameter removed.
     */
    std::span<const std::byte> flexible_zone_span() const noexcept;

    /** @brief Copy out of slot buffer with bounds check. */
    [[nodiscard]] bool read(void *dst, size_t len, size_t offset = 0) const noexcept;

    /** @brief Verify checksum for this slot (if enabled). */
    [[nodiscard]] bool verify_checksum_slot() const noexcept;
    /** 
     * @brief Verify checksum for flexible zone (if enabled).
     * @note Phase 2: Single flex zone design. Old index parameter removed.
     */
    [[nodiscard]] bool verify_checksum_flexible_zone() const noexcept;
    /** @brief Returns true if the slot is still valid (generation not overwritten). */
    [[nodiscard]] bool validate_read() const noexcept;

  private:
    friend class DataBlockConsumer;
    friend class DataBlockSlotIterator;
    explicit SlotConsumeHandle(std::unique_ptr<SlotConsumeHandleImpl> impl);
    std::unique_ptr<SlotConsumeHandleImpl> pImpl;
};

/**
 * @class DataBlockSlotIterator
 * @brief Iterator for ring-buffer slots (consumer view).
 *
 * Provides a higher-level API that hides commit_index/ring-buffer mechanics.
 */
class PYLABHUB_UTILS_EXPORT DataBlockSlotIterator
{
  public:
    struct NextResult;

    DataBlockSlotIterator();
    ~DataBlockSlotIterator() noexcept;
    DataBlockSlotIterator(DataBlockSlotIterator &&other) noexcept;
    DataBlockSlotIterator &operator=(DataBlockSlotIterator &&other) noexcept;
    DataBlockSlotIterator(const DataBlockSlotIterator &) = delete;
    DataBlockSlotIterator &operator=(const DataBlockSlotIterator &) = delete;

    /** @brief Advance to next available slot; returns ok=false on timeout. */
    [[nodiscard]] NextResult try_next(int timeout_ms = -1) noexcept;
    /** @brief Advance to next available slot; throws on timeout. */
    SlotConsumeHandle next(int timeout_ms = -1);

    /** @brief Set cursor to latest committed slot (no consumption). */
    void seek_latest() noexcept;
    /** @brief Set cursor to a specific slot id (next() returns newer). */
    void seek_to(uint64_t slot_id) noexcept;

    uint64_t last_slot_id() const noexcept;
    [[nodiscard]] bool is_valid() const noexcept;

  private:
    friend class DataBlockConsumer;
    explicit DataBlockSlotIterator(std::unique_ptr<DataBlockSlotIteratorImpl> impl);
    std::unique_ptr<DataBlockSlotIteratorImpl> pImpl;
};

/** Result of DataBlockSlotIterator::try_next() */
struct DataBlockSlotIterator::NextResult
{
    SlotConsumeHandle next;
    bool ok = false;
    int error_code = 0;
};

/**
 * @class DataBlockProducer
 * @brief Producer handle for a DataBlock. ABI-stable via pImpl.
 *
 * @par Thread Safety
 * DataBlockProducer is **thread-safe**: slot acquire/release, update_heartbeat,
 * check_consumer_health, and register_with_broker are protected by an internal mutex.
 * Multiple threads may share one producer; only one context (e.g. one write slot) is
 * active at a time per producer.
 */
class PYLABHUB_UTILS_EXPORT DataBlockProducer
{
  public:
    DataBlockProducer();
    ~DataBlockProducer() noexcept;
    DataBlockProducer(DataBlockProducer &&other) noexcept;
    DataBlockProducer &operator=(DataBlockProducer &&other) noexcept;
    DataBlockProducer(const DataBlockProducer &) = delete;
    DataBlockProducer &operator=(const DataBlockProducer &) = delete;

    // ─── Shared Spinlock API ───
    /** Acquire spinlock by index; returns owning guard. Throws if index invalid. */
    [[nodiscard]] std::unique_ptr<SharedSpinLockGuardOwning> acquire_spinlock(size_t index,
                                                                const std::string &debug_name = "");
    /** Get SharedSpinLock for direct use by index. */
    SharedSpinLock get_spinlock(size_t index);
    /** Total number of spinlocks (MAX_SHARED_SPINLOCKS). */
    uint32_t spinlock_count() const noexcept;

    // ─── Flexible Zone Access ───
    // Phase 2 refactoring: Single flex zone (no index parameter)
    template <typename T> T &flexible_zone()
    {
        std::span<std::byte> span = flexible_zone_span();
        if (span.size() < sizeof(T))
        {
            throw std::runtime_error("Flexible zone too small for type T");
        }
        return *reinterpret_cast<T *>(span.data());
    }
    /** 
     * @brief Direct view of flexible zone memory.
     * @return Empty span if zone is not configured (size == 0).
     * 
     * @note Phase 2: Single flex zone design. Old multi-zone index parameter removed.
     */
    std::span<std::byte> flexible_zone_span() noexcept;

    // ─── Checksum API (BLAKE2b via libsodium; stored in control zone) ───
    /** 
     * @brief Compute BLAKE2b of flexible zone, store in header.
     * @return false if zone not configured (size == 0).
     * 
     * @note Phase 2: Single flex zone design. Old index parameter removed.
     */
    [[nodiscard]] bool update_checksum_flexible_zone() noexcept;
    /** Compute BLAKE2b of data slot at index, store in header. Slot layout:
     * structured_buffer_size/ring_capacity. */
    [[nodiscard]] bool update_checksum_slot(size_t slot_index) noexcept;

    // ─── Primitive Data Transfer API ───
    /** Acquire a slot for writing; returns nullptr on timeout.
     * @note Release or destroy the handle before destroying this producer (see SlotWriteHandle). */
    [[nodiscard]] std::unique_ptr<SlotWriteHandle> acquire_write_slot(int timeout_ms = -1) noexcept;
    /**
     * @brief Release a previously acquired slot.
     * @return true on success. Returns false if:
     *   - handle is invalid (default-constructed or moved-from)
     *   - checksum update failed (when ChecksumPolicy::Enforced);
     *     slot was committed but BLAKE2b update failed)
     * Idempotent: calling again on an already-released handle returns true.
     */
    [[nodiscard]] bool release_write_slot(SlotWriteHandle &handle) noexcept;

    // ─── Broker and Health Management ───
    /** @brief Registers the producer with the broker.
     * If passing this producer's name as @p channel_name, use logical_name(name()) so the broker uses the logical channel name. */
    [[nodiscard]] bool register_with_broker(MessageHub &hub, const std::string &channel_name);
    /** @brief Checks the health of registered consumers and cleans up dead ones. */
    void check_consumer_health() noexcept;

    /** @brief Updates producer heartbeat (PID and monotonic timestamp). Call explicitly when idle, or
     * rely on automatic update on slot commit. Used for liveness: is_process_alive is only checked
     * when heartbeat is missing or stale. */
    void update_heartbeat() noexcept;

    /** @brief Last committed slot id (commit_index). Returns 0 if producer is invalid. */
    [[nodiscard]] uint64_t last_slot_id() const noexcept;

    /** 
     * @brief Retrieve comprehensive metrics and state snapshot from the DataBlock.
     * 
     * Fills the provided DataBlockMetrics structure with current metrics including:
     * - State snapshot: commit_index, slot_count
     * - Writer metrics: timeout counts, lock contention, blocked time
     * - Reader metrics: race detection, validation failures, peak reader count
     * - Error tracking: last error timestamp, error codes, error sequence
     * - Checksum metrics: checksum failures
     * - Performance: total slots written/read, total bytes written/read, uptime
     * 
     * This is a lightweight operation using relaxed memory ordering suitable for
     * monitoring and diagnostics. All metrics are atomic snapshots.
     * 
     * @param out_metrics Reference to DataBlockMetrics struct to fill
     * @return 0 on success, -1 if producer is invalid or error occurred
     * 
     * @note Thread-safe. Can be called concurrently with other operations.
     * @note Uses slot_rw_get_metrics() C API internally
     * 
     * @par Example
     * @code
     * DataBlockMetrics metrics;
     * if (producer->get_metrics(metrics) == 0) {
     *     std::cout << "Total commits: " << metrics.total_slots_written << "\n";
     *     std::cout << "Writer timeouts: " << metrics.writer_timeout_count << "\n";
     * }
     * @endcode
     */
    [[nodiscard]] int get_metrics(DataBlockMetrics &out_metrics) const noexcept;
    
    /**
     * @brief Reset all metrics counters to zero.
     * 
     * Resets metric counters while preserving state information (commit_index, slot_count).
     * Useful for measuring metrics over specific time intervals.
     * 
     * @return 0 on success, -1 if producer is invalid or error occurred
     * 
     * @note Thread-safe but should be used carefully in production to avoid
     *       losing diagnostic information during incidents.
     * @note Uses slot_rw_reset_metrics() C API internally
     */
    [[nodiscard]] int reset_metrics() noexcept;

    // ─── Structure Re-Mapping API (Placeholder - Future Feature) ───
    /**
     * @brief Request structure remapping (requires broker coordination).
     * @param new_flexzone_schema New flex zone structure info (optional)
     * @param new_datablock_schema New ring buffer slot structure info (optional)
     * @return RequestId for broker coordination
     * @throws std::runtime_error("Remapping requires broker - not yet implemented")
     * 
     * @note **PLACEHOLDER API.** Implementation deferred until broker is ready.
     *       This API ensures our design doesn't block future remapping capability.
     *       
     * **Future Remapping Protocol:**
     * 1. Producer calls `request_structure_remap()` → broker validates
     * 2. Broker signals all consumers to call `release_for_remap()`
     * 3. Producer calls `commit_structure_remap()` → updates schema_hash
     * 4. Broker signals consumers to call `reattach_after_remap()`
     * 
     * See `CHECKSUM_ARCHITECTURE.md` §7.1 for full protocol details.
     */
    [[nodiscard]] uint64_t request_structure_remap(
        const std::optional<schema::SchemaInfo> &new_flexzone_schema,
        const std::optional<schema::SchemaInfo> &new_datablock_schema
    );
    
    /**
     * @brief Commit structure remapping (after broker approval).
     * @param request_id From request_structure_remap()
     * @param new_flexzone_schema New flex zone structure (if remapping flex zone)
     * @param new_datablock_schema New slot structure (if remapping slots)
     * @throws std::runtime_error if broker hasn't approved or remap not in progress
     * 
     * @note **PLACEHOLDER API.** Throws "not implemented" until broker is ready.
     * 
     * Updates `schema_hash`, `schema_version`, and recomputes checksums.
     * Must be called with all consumers detached (broker-coordinated).
     */
    void commit_structure_remap(
        uint64_t request_id,
        const std::optional<schema::SchemaInfo> &new_flexzone_schema,
        const std::optional<schema::SchemaInfo> &new_datablock_schema
    );

    // ====================================================================
    // Phase 3: C++ RAII Layer - Type-Safe Transaction API
    // ====================================================================

    /**
     * @brief Execute a type-safe transaction with schema validation
     * @tparam FlexZoneT Type of flexible zone data (or void for no flexzone)
     * @tparam DataBlockT Type of datablock slot data
     * @tparam Func Lambda/callable type (must accept WriteTransactionContext<F,D>&)
     * @param timeout Default timeout for slot operations
     * @param func Lambda receiving transaction context
     * @return Result of lambda invocation
     * @throws std::invalid_argument if parameters invalid
     * @throws std::runtime_error if entry validation fails (schema, layout, checksums)
     * 
     * **Type-Safe Transaction API** - The primary interface for producer operations.
     * 
     * Entry Validation:
     * - Schema validation (if registered): sizeof(FlexZoneT) and sizeof(DataBlockT)
     * - Layout validation: slot count, stride
     * - Checksum policy enforcement
     * 
     * Context Lifetime:
     * - Context valid for entire lambda scope
     * - RAII ensures cleanup on exception
     * - Current slot auto-released on scope exit
     * 
     * Usage:
     * @code
     * struct MetaData { int status; };
     * struct Payload { double value; };
     * 
     * producer.with_transaction<MetaData, Payload>(100ms, [](auto& ctx) {
     *     // Access flexible zone
     *     ctx.flexzone().get().status = 1;
     *     
     *     // Iterate over slots (non-terminating)
     *     for (auto result : ctx.slots(100ms)) {
     *         if (!result.is_ok()) {
     *             if (result.error() == SlotAcquireError::Timeout) {
     *                 process_events();
     *             }
     *             if (ctx.flexzone().get().shutdown_requested) break;
     *             continue;
     *         }
     *         
     *         auto& slot = result.value();
     *         slot.get().value = compute();
     *         ctx.publish();
     *     }
     * });
     * @endcode
     * 
     * @note Requires: FlexZoneT and DataBlockT must be trivially copyable.
     * @note Thread-safe: Producer has internal mutex, contexts are per-thread.
     * 
     * @see WriteTransactionContext, SlotIterator, Result
     */
    template <typename FlexZoneT, typename DataBlockT, typename Func>
        requires std::invocable<Func, WriteTransactionContext<FlexZoneT, DataBlockT> &>
    [[nodiscard]] auto with_transaction(std::chrono::milliseconds timeout, Func &&func)
        -> std::invoke_result_t<Func, WriteTransactionContext<FlexZoneT, DataBlockT> &>;

    /** @brief Display name (for diagnostics and logging). Not hot path: computed once per instance and cached.
     * Returns "(null)" if no pImpl. Otherwise returns the user name plus suffix " | pid:&lt;pid&gt;-&lt;idx&gt;",
     * or a generated id "producer-&lt;pid&gt;-&lt;idx&gt;" if no name was provided. For comparison use logical_name(name()). */
    [[nodiscard]] const std::string &name() const noexcept;

    /** @brief Returns the checksum policy configured for this DataBlock.
     * Used by the RAII layer (with_transaction) to decide whether to auto-update the
     * flexzone checksum on transaction exit. Returns ChecksumPolicy::None if pImpl is null. */
    [[nodiscard]] ChecksumPolicy checksum_policy() const noexcept;

    /** Construct from implementation (for factory use; Impl is opaque to users). */
    explicit DataBlockProducer(std::unique_ptr<DataBlockProducerImpl> impl);

  private:
    std::unique_ptr<DataBlockProducerImpl> pImpl;
};

/**
 * @class DataBlockConsumer
 * @brief Consumer handle for a DataBlock. ABI-stable via pImpl.
 *
 * @par Thread Safety
 * DataBlockConsumer is **thread-safe**: slot acquire/release, slot_iterator(),
 * register_heartbeat, update_heartbeat, and unregister_heartbeat are protected by
 * an internal recursive mutex. Multiple threads may share one consumer; only one
 * context (e.g. one consume slot or iterator advance) is active at a time per consumer.
 */
class PYLABHUB_UTILS_EXPORT DataBlockConsumer
{
  public:
    DataBlockConsumer();
    ~DataBlockConsumer() noexcept;
    DataBlockConsumer(DataBlockConsumer &&other) noexcept;
    DataBlockConsumer &operator=(DataBlockConsumer &&other) noexcept;
    DataBlockConsumer(const DataBlockConsumer &) = delete;
    DataBlockConsumer &operator=(const DataBlockConsumer &) = delete;

    // ─── Shared Spinlock API ───
    SharedSpinLock get_spinlock(size_t index);
    uint32_t spinlock_count() const noexcept;

    // ─── Flexible Zone Access ───
    // Phase 2 refactoring: Single flex zone (no index parameter)
    template <typename T> const T &flexible_zone() const
    {
        std::span<const std::byte> span = flexible_zone_span();
        if (span.size() < sizeof(T))
        {
            throw std::runtime_error("Flexible zone too small for type T");
        }
        return *reinterpret_cast<const T *>(span.data());
    }
    /**
     * @brief Read-only view of flexible zone memory.
     * @return Empty span if zone is not configured (size == 0).
     * 
     * @note Phase 2: Single flex zone design. Old multi-zone index parameter removed.
     */
    std::span<const std::byte> flexible_zone_span() const noexcept;

    // ─── Checksum API (BLAKE2b; verify stored checksum matches computed) ───
    /** 
     * @brief Returns true if stored checksum matches computed BLAKE2b.
     * @return false if zone not configured (size == 0).
     * 
     * @note Phase 2: Single flex zone design. Old index parameter removed.
     */
    [[nodiscard]] bool verify_checksum_flexible_zone() const noexcept;
    /** Returns true if stored checksum matches computed BLAKE2b of data slot. */
    [[nodiscard]] bool verify_checksum_slot(size_t slot_index) const noexcept;

    // --- Heartbeat Management ---
    // Heartbeat registration and deregistration are managed automatically:
    //   - register_heartbeat() is called by find_datablock_consumer<>() at construction.
    //   - unregister_heartbeat() is called by the DataBlockConsumer destructor.
    // These methods are public for advanced use (e.g. attaching a consumer without the factory),
    // but normal callers do not need to call them directly.

    /** @brief Registers the consumer in the heartbeat table. Returns the slot index or -1 on
     * failure (pool exhausted). Normally called automatically by the factory function. */
    [[nodiscard]] int register_heartbeat();

    /** @brief Updates the heartbeat for the given slot index. */
    void update_heartbeat(int slot);

    /** @brief Updates the heartbeat for the currently registered slot.
     * No-op if no heartbeat slot is registered.
     * Call during long idle periods inside a transaction loop to signal liveness. */
    void update_heartbeat() noexcept;

    /** @brief Unregisters the consumer from the heartbeat table.
     * Normally called automatically by the destructor. */
    void unregister_heartbeat(int slot);

    // ─── Primitive Data Transfer API ───
    /** Acquire the next slot for reading; returns nullptr on timeout.
     * @note Release or destroy the handle before destroying this consumer (see SlotConsumeHandle).
     * @note Single-threaded: do not call from multiple threads on the same consumer instance. */
    [[nodiscard]] std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(int timeout_ms = -1) noexcept;
    /** Acquire a specific slot by ID for reading; returns nullptr on timeout or if slot not
     * available. */
    [[nodiscard]] std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(uint64_t slot_id,
                                                                          int timeout_ms) noexcept;
    /** Release a previously acquired slot; returns false if checksum verification failed. */
    [[nodiscard]] bool release_consume_slot(SlotConsumeHandle &handle) noexcept;

    /** Iterator for ring-buffer slots (consumer view). */
    DataBlockSlotIterator slot_iterator();

    // ─── Broker Discovery ───
    /** @brief Discovers a producer via the broker and attaches as a consumer. */
    [[nodiscard]] static std::unique_ptr<DataBlockConsumer> discover(MessageHub &hub,
                                                       const std::string &channel_name,
                                                       uint64_t shared_secret,
                                                       const DataBlockConfig &expected_config);

    /** @brief Display name (for diagnostics and logging). Not hot path: computed once per instance and cached.
     * Returns "(null)" if no pImpl. Otherwise returns the user name plus suffix " | pid:&lt;pid&gt;-&lt;idx&gt;",
     * or a generated id "consumer-&lt;pid&gt;-&lt;idx&gt;" if no name was provided. For comparison use logical_name(name()). */
    [[nodiscard]] const std::string &name() const noexcept;

    /** 
     * @brief Retrieve comprehensive metrics and state snapshot from the DataBlock.
     * 
     * Fills the provided DataBlockMetrics structure with current metrics including:
     * - State snapshot: commit_index, slot_count
     * - Writer metrics: timeout counts, lock contention, blocked time
     * - Reader metrics: race detection, validation failures, peak reader count
     * - Error tracking: last error timestamp, error codes, error sequence
     * - Checksum metrics: checksum failures
     * - Performance: total slots written/read, total bytes written/read, uptime
     * 
     * This is a lightweight operation using relaxed memory ordering suitable for
     * monitoring and diagnostics. All metrics are atomic snapshots.
     * 
     * @param out_metrics Reference to DataBlockMetrics struct to fill
     * @return 0 on success, -1 if consumer is invalid or error occurred
     * 
     * @note Thread-safe. Can be called concurrently with other operations.
     * @note Uses slot_rw_get_metrics() C API internally
     * 
     * @par Example
     * @code
     * DataBlockMetrics metrics;
     * if (consumer->get_metrics(metrics) == 0) {
     *     std::cout << "Total reads: " << metrics.total_slots_read << "\n";
     *     std::cout << "Reader races detected: " << metrics.reader_race_detected << "\n";
     *     std::cout << "Peak concurrent readers: " << metrics.reader_peak_count << "\n";
     * }
     * @endcode
     */
    [[nodiscard]] int get_metrics(DataBlockMetrics &out_metrics) const noexcept;
    
    /**
     * @brief Reset all metrics counters to zero.
     * 
     * Resets metric counters while preserving state information (commit_index, slot_count).
     * Useful for measuring metrics over specific time intervals.
     * 
     * @return 0 on success, -1 if consumer is invalid or error occurred
     * 
     * @note Thread-safe but should be used carefully in production to avoid
     *       losing diagnostic information during incidents.
     * @note Uses slot_rw_reset_metrics() C API internally
     */
    [[nodiscard]] int reset_metrics() noexcept;

    // ─── Structure Re-Mapping API (Placeholder - Future Feature) ───
    /**
     * @brief Release context for structure remapping.
     * Called in response to broker signal when remapping is requested.
     * Consumer waits for broker approval before reattaching.
     * 
     * @throws std::runtime_error("Remapping requires broker - not yet implemented")
     * 
     * @note **PLACEHOLDER API.** Implementation deferred until broker is ready.
     * 
     * **Future Protocol:**
     * 1. Broker signals consumer → consumer calls `release_for_remap()`
     * 2. Consumer detaches, waits for broker "remap complete" signal
     * 3. Consumer calls `reattach_after_remap()` with new schema
     * 
     * See `CHECKSUM_ARCHITECTURE.md` §7.1 for full protocol details.
     */
    void release_for_remap();
    
    /**
     * @brief Reattach after structure remapping.
     * @param new_flexzone_schema Expected flex zone structure
     * @param new_datablock_schema Expected slot structure
     * @throws SchemaMismatchException if reattach with wrong schema
     * @throws std::runtime_error("Remapping requires broker - not yet implemented")
     * 
     * @note **PLACEHOLDER API.** Implementation deferred until broker is ready.
     * 
     * Revalidates schema against producer's updated `schema_hash`.
     * If schema matches, consumer resumes normal operations.
     */
    void reattach_after_remap(
        const std::optional<schema::SchemaInfo> &new_flexzone_schema,
        const std::optional<schema::SchemaInfo> &new_datablock_schema
    );

    // ====================================================================
    // Phase 3: C++ RAII Layer - Type-Safe Transaction API
    // ====================================================================

    /**
     * @brief Execute a type-safe transaction with schema validation
     * @tparam FlexZoneT Type of flexible zone data (or void for no flexzone)
     * @tparam DataBlockT Type of datablock slot data
     * @tparam Func Lambda/callable type (must accept ReadTransactionContext<F,D>&)
     * @param timeout Default timeout for slot operations
     * @param func Lambda receiving transaction context
     * @return Result of lambda invocation
     * @throws std::invalid_argument if parameters invalid
     * @throws std::runtime_error if entry validation fails (schema, layout, checksums)
     * 
     * **Type-Safe Transaction API** - The primary interface for consumer operations.
     * 
     * Entry Validation:
     * - Schema validation (if registered): sizeof(FlexZoneT) and sizeof(DataBlockT)
     * - Layout validation: slot count, stride
     * - Checksum policy enforcement
     * 
     * Context Lifetime:
     * - Context valid for entire lambda scope
     * - RAII ensures cleanup on exception
     * - Current slot auto-released on scope exit
     * 
     * Usage:
     * @code
     * struct MetaData { int status; };
     * struct Payload { double value; };
     * 
     * consumer.with_transaction<MetaData, Payload>(100ms, [](auto& ctx) {
     *     // Iterate over slots (non-terminating)
     *     for (auto result : ctx.slots(100ms)) {
     *         if (!result.is_ok()) {
     *             process_events();
     *             continue;
     *         }
     *         
     *         if (!ctx.validate_read()) continue;
     *         
     *         if (ctx.flexzone().get().end_of_stream) break;
     *         
     *         auto& slot = result.value();
     *         process(slot.get().value);
     *     }
     * });
     * @endcode
     * 
     * @note Requires: FlexZoneT and DataBlockT must be trivially copyable.
     * @note Thread-safe: Consumer has internal mutex, contexts are per-thread.
     * 
     * @see ReadTransactionContext, SlotIterator, Result
     */
    template <typename FlexZoneT, typename DataBlockT, typename Func>
        requires std::invocable<Func, ReadTransactionContext<FlexZoneT, DataBlockT> &>
    [[nodiscard]] auto with_transaction(std::chrono::milliseconds timeout, Func &&func)
        -> std::invoke_result_t<Func, ReadTransactionContext<FlexZoneT, DataBlockT> &>;

    /** Construct from implementation (for factory use; Impl is opaque to users). */
    explicit DataBlockConsumer(std::unique_ptr<DataBlockConsumerImpl> impl);

  private:
    std::unique_ptr<DataBlockConsumerImpl> pImpl;
};

} // namespace pylabhub::hub

// ============================================================================
// Phase 3: C++ RAII Layer - Headers (outside namespace to avoid nesting)
// ============================================================================

#include "utils/result.hpp"
#include "utils/slot_ref.hpp"
#include "utils/zone_ref.hpp"
#include "utils/transaction_context.hpp"
#include "utils/slot_iterator.hpp"

namespace pylabhub::hub
{

// ============================================================================
// Phase 3: C++ RAII Layer - Template Implementations
// ============================================================================

// with_transaction() implementations
template <typename FlexZoneT, typename DataBlockT, typename Func>
    requires std::invocable<Func, WriteTransactionContext<FlexZoneT, DataBlockT> &>
auto DataBlockProducer::with_transaction(std::chrono::milliseconds timeout, Func &&func)
    -> std::invoke_result_t<Func, WriteTransactionContext<FlexZoneT, DataBlockT> &>
{
    WriteTransactionContext<FlexZoneT, DataBlockT> ctx(this, timeout);

    using ReturnType = std::invoke_result_t<Func, WriteTransactionContext<FlexZoneT, DataBlockT> &>;

    if constexpr (std::is_void_v<ReturnType>)
    {
        std::forward<Func>(func)(ctx);
        // Conservative: only auto-update flexzone checksum on normal exit (no exception).
        // On exception the stack unwinds past this point, so the update is skipped —
        // leaving the stored checksum inconsistent with any partial flexzone writes,
        // which signals to consumers that the flexzone state is unreliable.
        if constexpr (!std::is_void_v<FlexZoneT>)
        {
            if (!ctx.is_flexzone_checksum_suppressed() &&
                checksum_policy() != ChecksumPolicy::None)
            {
                (void)update_checksum_flexible_zone();
            }
        }
    }
    else
    {
        auto &&result = std::forward<Func>(func)(ctx);
        if constexpr (!std::is_void_v<FlexZoneT>)
        {
            if (!ctx.is_flexzone_checksum_suppressed() &&
                checksum_policy() != ChecksumPolicy::None)
            {
                (void)update_checksum_flexible_zone();
            }
        }
        return std::forward<decltype(result)>(result);
    }
}

template <typename FlexZoneT, typename DataBlockT, typename Func>
    requires std::invocable<Func, ReadTransactionContext<FlexZoneT, DataBlockT> &>
auto DataBlockConsumer::with_transaction(std::chrono::milliseconds timeout, Func &&func)
    -> std::invoke_result_t<Func, ReadTransactionContext<FlexZoneT, DataBlockT> &>
{
    // Create transaction context with entry validation
    ReadTransactionContext<FlexZoneT, DataBlockT> ctx(this, timeout);

    // Invoke user lambda with context reference
    // Exception safety: ctx destructor ensures cleanup
    return std::forward<Func>(func)(ctx);
}

/**
 * @brief Executes a function on the next available slot from a `DataBlockSlotIterator`.
 *
 * This function attempts to get the next available slot from the iterator and,
 * if successful, executes the provided lambda with the slot handle.
 *
 * @tparam Func A callable that takes a `const SlotConsumeHandle&`.
 * @param iterator The `DataBlockSlotIterator` to use.
 * @param timeout_ms The timeout in milliseconds to wait for the next slot.
 * @param lambda The function to execute.
 * @return An `std::optional` containing the return value of the lambda, or `std::nullopt` on
 * timeout.
 */
template <typename Func>
auto with_next_slot(DataBlockSlotIterator &iterator, int timeout_ms, Func &&lambda)
    -> std::optional<
        std::conditional_t<std::is_void_v<std::invoke_result_t<Func, const SlotConsumeHandle &>>,
                           std::monostate, std::invoke_result_t<Func, const SlotConsumeHandle &>>>
{
    using LambdaReturnType = std::invoke_result_t<Func, const SlotConsumeHandle &>;
    using OptionalWrappedType =
        std::conditional_t<std::is_void_v<LambdaReturnType>, std::monostate, LambdaReturnType>;
    using ReturnOptionalType = std::optional<OptionalWrappedType>;

    auto result = iterator.try_next(timeout_ms);

    if (!result.ok)
    {
        return ReturnOptionalType();
    }

    try
    {
        if constexpr (std::is_void_v<LambdaReturnType>)
        {
            std::invoke(std::forward<Func>(lambda), result.next);
            return ReturnOptionalType(std::monostate());
        }
        else
        {
            return ReturnOptionalType(std::invoke(std::forward<Func>(lambda), result.next));
        }
    }
    catch (...)
    {
        throw;
    }
}

// ─── Diagnostic attach (for recovery / tooling; read-only) ───
/**
 * @brief Opaque handle for attaching to a DataBlock by name for diagnostics only.
 * @see open_datablock_for_diagnostic
 */
class PYLABHUB_UTILS_EXPORT DataBlockDiagnosticHandle
{
  public:
    ~DataBlockDiagnosticHandle() noexcept;
    DataBlockDiagnosticHandle(DataBlockDiagnosticHandle &&) noexcept;
    DataBlockDiagnosticHandle &operator=(DataBlockDiagnosticHandle &&) noexcept;
    DataBlockDiagnosticHandle(const DataBlockDiagnosticHandle &) = delete;
    DataBlockDiagnosticHandle &operator=(const DataBlockDiagnosticHandle &) = delete;

    SharedMemoryHeader *header() const;
    SlotRWState *slot_rw_state(uint32_t index) const;

  private:
    explicit DataBlockDiagnosticHandle(std::unique_ptr<DataBlockDiagnosticHandleImpl> impl);
    std::unique_ptr<DataBlockDiagnosticHandleImpl> pImpl;
    friend PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockDiagnosticHandle>
    open_datablock_for_diagnostic(const std::string &name);
};

/** Opens an existing DataBlock by name for read-only diagnostics. Returns nullptr on failure. */
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockDiagnosticHandle>
open_datablock_for_diagnostic(const std::string &name);

/** @brief Returns the logical name (part before any runtime suffix) for comparison/lookup.
 * Producer/Consumer name() may append a suffix " | pid:<pid>-<idx>"; for channel or broker
 * comparison use this. See docs/NAME_CONVENTIONS.md. */
inline std::string_view logical_name(std::string_view full_name) noexcept
{
    constexpr std::string_view suffix_marker(" | pid:");
    const auto pos = full_name.find(suffix_marker);
    if (pos == std::string_view::npos)
        return full_name;
    return full_name.substr(0, pos);
}

// ─── Factory Functions (require LifecycleGuard with GetLifecycleModule() in main()) ───

// Internal implementation (exported for test and recovery tool use)
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockProducer>
create_datablock_producer_impl(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                               const DataBlockConfig &config,
                               const pylabhub::schema::SchemaInfo *flexzone_schema,
                               const pylabhub::schema::SchemaInfo *datablock_schema);

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer_impl(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                             const DataBlockConfig *expected_config,
                             const pylabhub::schema::SchemaInfo *flexzone_schema,
                             const pylabhub::schema::SchemaInfo *datablock_schema);

// Public C++ API: Template-based dual-schema factory functions.
// Schema derived from template parameters, stored/validated in shared memory.

// ============================================================================
// Memory model: layout and validation API (single control surface)
// ============================================================================
// All layout/segment validation entry points live here. Access to layout
// information (slot stride, offsets) is internal and goes through DataBlockLayout;
// these functions are used at creation, attach, and integrity validation.
// ============================================================================

/**
 * @brief Returns schema info for SharedMemoryHeader including layout (offset/size per member).
 * @details Used for protocol checking: producer stores the layout hash in the header,
 *          consumer validates that its header layout matches (same ABI).
 *          The schema is the canonical source for header field names: every header
 *          member is listed there; struct and docs should use the same names.
 * @return SchemaInfo with BLDS encoding member names, types, offsets and sizes.
 */
PYLABHUB_UTILS_EXPORT pylabhub::schema::SchemaInfo get_shared_memory_header_schema_info();

/**
 * @brief Validates that the header's stored layout hash matches this build's SharedMemoryHeader
 * layout.
 * @param header Mapped SharedMemoryHeader (must be fully initialized by producer).
 * @throws pylabhub::schema::SchemaValidationException if layout hash mismatch (ABI
 * incompatibility).
 */
PYLABHUB_UTILS_EXPORT void validate_header_layout_hash(const SharedMemoryHeader *header);

/** Store layout checksum in header (call at segment creation after header is written). */
PYLABHUB_UTILS_EXPORT void store_layout_checksum(SharedMemoryHeader *header);
/** Validate layout checksum; returns true if stored checksum matches recomputed from header. */
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool validate_layout_checksum(const SharedMemoryHeader *header);

// ============================================================================
// Template Factory Function Implementations (must be in header for templates)
// ============================================================================

// ============================================================================
// Phase 4: Dual-Schema Template Implementations
// ============================================================================

/**
 * @brief Creates producer with dual-schema storage (FlexZone + DataBlock).
 * Schema is derived from the template parameters (FlexZoneT, DataBlockT); no schema argument.
 * @tparam FlexZoneT Type of flexible zone data (schema generated from this type)
 * @tparam DataBlockT Type of datablock slot data (schema generated from this type)
 */
template <typename FlexZoneT, typename DataBlockT>
[[nodiscard]] std::unique_ptr<DataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                          const DataBlockConfig &config)
{
    // Compile-time validation
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");
    
    // Generate BOTH schemas at compile-time
    auto flexzone_schema = pylabhub::schema::generate_schema_info<FlexZoneT>(
        "FlexZone", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    auto datablock_schema = pylabhub::schema::generate_schema_info<DataBlockT>(
        "DataBlock", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    // Validate sizes
    if constexpr (!std::is_void_v<FlexZoneT>)
    {
        if (config.flex_zone_size < sizeof(FlexZoneT))
        {
            throw std::invalid_argument(
                "config.flex_zone_size (" + std::to_string(config.flex_zone_size) +
                ") too small for FlexZoneT (" + std::to_string(sizeof(FlexZoneT)) + ")");
        }
    }
    
    size_t slot_size = config.effective_logical_unit_size();
    if (slot_size < sizeof(DataBlockT))
    {
        throw std::invalid_argument(
            "slot size (" + std::to_string(slot_size) +
            ") too small for DataBlockT (" + std::to_string(sizeof(DataBlockT)) + ")");
    }
    
    // Call internal implementation with BOTH schemas
    return create_datablock_producer_impl(hub, name, policy, config,
                                          &flexzone_schema, &datablock_schema);
}

/**
 * @brief Discovers consumer with dual-schema validation (FlexZone + DataBlock).
 * Schema is derived from the template parameters (FlexZoneT, DataBlockT); no schema argument.
 * @tparam FlexZoneT Expected type of flexible zone (must match producer)
 * @tparam DataBlockT Expected type of datablock slot (must match producer)
 * @param expected_config Config to validate against producer's config (REQUIRED for type-safe API)
 * @return Consumer handle, or nullptr if schema hashes don't match, producer did not store
 *         schemas, or config/sizes incompatible (see DESIGN_VERIFICATION_CHECKLIST.md).
 * @throws std::invalid_argument if sizes/alignment incompatible (during config validation).
 */
template <typename FlexZoneT, typename DataBlockT>
[[nodiscard]] std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                        const DataBlockConfig &expected_config)
{
    // Compile-time validation
    static_assert(std::is_void_v<FlexZoneT> || std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");
    
    // Generate BOTH expected schemas at compile-time
    auto expected_flexzone = pylabhub::schema::generate_schema_info<FlexZoneT>(
        "FlexZone", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    auto expected_datablock = pylabhub::schema::generate_schema_info<DataBlockT>(
        "DataBlock", pylabhub::schema::SchemaVersion{1, 0, 0});
    
    // Call internal implementation with BOTH schemas for validation
    return find_datablock_consumer_impl(hub, name, shared_secret, &expected_config,
                                        &expected_flexzone, &expected_datablock);
}

// ============================================================================
// Phase 3: Single-Schema Template Implementations (Deprecated)
// ============================================================================


} // namespace pylabhub::hub

#if defined(_MSC_VER)
#pragma warning(pop)
#endif