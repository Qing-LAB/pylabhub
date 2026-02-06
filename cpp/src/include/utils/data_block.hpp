#pragma once
/**
 * @file data_block.hpp
 * @brief Shared memory data block with producer/consumer coordination.
 *
 * Design: Expandable chain of blocks, counters/flags section, iterator interface.
 * All public classes use pImpl for ABI stability. See docs/hep/hep-core-0002-data-exchange-hub-framework.md.
 */
#include "pylabhub_utils_export.h"
#include "data_header_sync_primitives.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace pylabhub::hub
{

// Forward declarations
class MessageHub;
struct DataBlockProducerImpl;
struct DataBlockConsumerImpl;
struct DataBlockIteratorImpl;

/** Chain position flags for SharedMemoryHeader */
constexpr uint32_t CHAIN_HEAD = 1u << 0;
constexpr uint32_t CHAIN_TAIL = 1u << 1;

/** Format of the flexible data zone (per block; can differ along the chain) */
enum class FlexibleZoneFormat : uint8_t
{
    Raw = 0,
    MessagePack = 1,
    Json = 2
};

/** Unit block size for structured data buffer. Simplifies bookkeeping; may waste memory. */
enum class DataBlockUnitSize : uint32_t
{
    Size4K = 4096u,
    Size4M = 4194304u,
    Size16M = 16777216u
};

/** Return byte size for DataBlockUnitSize */
inline size_t to_bytes(DataBlockUnitSize u)
{
    return static_cast<size_t>(u);
}

/**
 * @enum DataBlockPolicy
 * @brief Defines the buffer management strategy for a DataBlock.
 */
enum class DataBlockPolicy
{
    Single,
    DoubleBuffer,
    RingBuffer
};

/**
 * @struct DataBlockConfig
 * @brief Configuration for creating a new DataBlock.
 */
struct DataBlockConfig
{
    uint64_t shared_secret;
    size_t flexible_zone_size;
    DataBlockUnitSize unit_block_size = DataBlockUnitSize::Size4K;
    int ring_buffer_capacity; // Slot count: 1=Single, 2=Double, N=RingBuffer
    FlexibleZoneFormat flexible_zone_format = FlexibleZoneFormat::Raw;
    bool enable_checksum = false; // BLAKE2b checksums in control zone (flexible zone + slots)

    /** Computed: slot_count * unit_block_size. slot_count = max(1, ring_buffer_capacity). */
    size_t structured_buffer_size() const
    {
        size_t slots = (ring_buffer_capacity > 0) ? static_cast<size_t>(ring_buffer_capacity) : 1u;
        return slots * to_bytes(unit_block_size);
    }
};

/**
 * @struct SharedMemoryHeader
 * @brief The header structure for every DataBlock shared memory segment.
 *
 * Supports expandable bidirectional chain. Chain never shrinks; expansion and
 * shutdown are handled at higher level.
 */
struct SharedMemoryHeader
{
    // ──────────────────────────────────────────────────
    // Section 1: Identity & Validation (32 bytes)
    // ──────────────────────────────────────────────────
    uint64_t magic_number;  // Magic constant (SET LAST during init)
    uint64_t shared_secret; // Key to prevent unauthorized access
    uint32_t version;       // Version of the header layout
    uint32_t header_size;   // sizeof(SharedMemoryHeader)

    std::atomic<uint32_t> init_state; // 0=uninit, 1=mutex ready, 2=fully init
    uint32_t _padding1;

    // ──────────────────────────────────────────────────
    // Section 2: Chain Navigation (24 bytes)
    // ──────────────────────────────────────────────────
    uint64_t prev_block_offset; // Offset to previous block header (0 = head)
    uint64_t next_block_offset; // Offset to next block header (0 = tail)
    uint32_t chain_flags;       // CHAIN_HEAD | CHAIN_TAIL
    uint32_t chain_index;       // 0-based position in chain (head=0)

    // Chain-wide totals (valid only when CHAIN_HEAD; updated on expansion)
    std::atomic<uint32_t> total_spinlock_count;
    std::atomic<uint32_t> total_counter_count;

    // ──────────────────────────────────────────────────
    // Section 3: Consumer Management & Data Indices (40 bytes)
    // ──────────────────────────────────────────────────
    std::atomic<uint32_t> active_consumer_count;
    std::atomic<uint64_t> write_index;
    std::atomic<uint64_t> commit_index;
    std::atomic<uint64_t> read_index;
    std::atomic<uint64_t> current_slot_id;

    // ──────────────────────────────────────────────────
    // Section 4: Internal Management Mutex (64 bytes)
    // POSIX: pthread_mutex_t storage; Windows: reserved (uses named kernel mutex)
    // ──────────────────────────────────────────────────
    char management_mutex_storage[64];

    // ──────────────────────────────────────────────────
    // Section 5: User Spinlocks (128 bytes)
    // ──────────────────────────────────────────────────
    static constexpr size_t MAX_SHARED_SPINLOCKS = 8;
    SharedSpinLockState shared_spinlocks[MAX_SHARED_SPINLOCKS];
    std::atomic_flag spinlock_allocated[MAX_SHARED_SPINLOCKS];

    // ──────────────────────────────────────────────────
    // Section 6: 64-bit Counters/Flags (64 bytes)
    // Atomic slots for timestamps, counts, bit flags.
    // User coordinates access via spinlocks when needed.
    // ──────────────────────────────────────────────────
    static constexpr size_t NUM_COUNTERS_64 = 8;
    std::atomic<uint64_t> counters_64[NUM_COUNTERS_64];

    // ──────────────────────────────────────────────────
    // Section 7: Flexible Zone & Buffer Metadata (28 bytes)
    // ──────────────────────────────────────────────────
    FlexibleZoneFormat flexible_zone_format; // Raw, MessagePack, or Json
    uint8_t _reserved_format[3];
    uint32_t flexible_zone_size;      // Size in bytes (for consumer discovery)
    uint32_t ring_buffer_capacity;    // Slot count (1=Single, 2=Double, N=Ring)
    uint32_t structured_buffer_size;  // Total structured buffer = slot_count * unit_block_size
    uint32_t unit_block_size;         // Bytes per slot: 4096, 4194304, or 16777216
    uint8_t checksum_enabled;         // 1 if BLAKE2b checksums are in use
    uint8_t _reserved_buffer[3];

    // ──────────────────────────────────────────────────
    // Section 8: Integrity Checksums (BLAKE2b via libsodium)
    // Flexible zone checksum in fixed header; slot checksums in variable region.
    // ──────────────────────────────────────────────────
    static constexpr size_t CHECKSUM_BYTES = 32; // crypto_generichash_BYTES (BLAKE2b-256)
    uint8_t flexible_zone_checksum[CHECKSUM_BYTES];
    std::atomic<uint8_t> flexible_zone_checksum_valid; // 0=not set, 1=valid
    uint8_t _checksum_pad[7];

    /** Bytes per slot in checksum region: CHECKSUM_BYTES + 1 (valid flag). */
    static constexpr size_t SLOT_CHECKSUM_ENTRY_SIZE = CHECKSUM_BYTES + 1;
    /** Size of variable slot checksum region when enabled. */
    size_t slot_checksum_region_size() const
    {
        return (checksum_enabled && ring_buffer_capacity > 0)
                   ? (static_cast<size_t>(ring_buffer_capacity) * SLOT_CHECKSUM_ENTRY_SIZE)
                   : 0;
    }
};

/**
 * @class DataBlockIterator
 * @brief Iterator for traversing blocks in the shared memory chain.
 *
 * Used for ring-buffer and similar patterns. Supports advancing to next block
 * with explicit deny/error when the next block is not ready.
 */
class PYLABHUB_UTILS_EXPORT DataBlockIterator
{
  public:
    struct NextResult;

    DataBlockIterator();
    ~DataBlockIterator();
    DataBlockIterator(DataBlockIterator &&other) noexcept;
    DataBlockIterator &operator=(DataBlockIterator &&other) noexcept;
    DataBlockIterator(const DataBlockIterator &) = delete;
    DataBlockIterator &operator=(const DataBlockIterator &) = delete;

    /** @brief Advance to next block. Returns ok=false if next not ready. */
    NextResult try_next();

    /** @brief Advance to next block; throws if next not available. */
    DataBlockIterator next();

    /** @brief Base address of current block's header. */
    void *block_base() const;

    /** @brief Position in chain (0 = head). */
    size_t block_index() const;

    /** @brief Pointer to flexible data zone for current block. */
    void *flexible_zone_base() const;

    /** @brief Size of flexible zone in bytes. */
    size_t flexible_zone_size() const;

    /** @brief Format of flexible zone (Raw, MessagePack, Json). */
    FlexibleZoneFormat flexible_zone_format() const;

    bool is_head() const;
    bool is_tail() const;
    bool is_valid() const;

  private:
    friend struct DataBlockProducerImpl;
    friend struct DataBlockConsumerImpl;
    friend class DataBlockProducer;
    friend class DataBlockConsumer;
    explicit DataBlockIterator(std::unique_ptr<DataBlockIteratorImpl> impl);
    std::unique_ptr<DataBlockIteratorImpl> pImpl;
};

/** Result of DataBlockIterator::try_next() */
struct DataBlockIterator::NextResult
{
    DataBlockIterator next;
    bool ok = false;
    int error_code = 0;
};

/**
 * @class DataBlockProducer
 * @brief Producer handle for a DataBlock. ABI-stable via pImpl.
 */
class PYLABHUB_UTILS_EXPORT DataBlockProducer
{
  public:
    DataBlockProducer();
    ~DataBlockProducer();
    DataBlockProducer(DataBlockProducer &&other) noexcept;
    DataBlockProducer &operator=(DataBlockProducer &&other) noexcept;
    DataBlockProducer(const DataBlockProducer &) = delete;
    DataBlockProducer &operator=(const DataBlockProducer &) = delete;

    // ─── Spinlock API (index is global across chain) ───
    /** Acquire spinlock by index; returns owning guard. Throws if index invalid. */
    std::unique_ptr<SharedSpinLockGuardOwning> acquire_spinlock(size_t index,
                                                               const std::string &debug_name = "");

    /** Release spinlock allocation (producer only; makes slot available for re-acquisition). */
    void release_spinlock(size_t index);

    /** Get SharedSpinLock for direct use by index. */
    SharedSpinLock get_spinlock(size_t index);

    /** Total number of spinlocks in the chain. */
    uint32_t spinlock_count() const;

    /** Acquire next free spinlock slot (legacy API). */
    std::unique_ptr<SharedSpinLockGuardOwning> acquire_user_spinlock(const std::string &debug_name);

    /** Release by index (legacy API). */
    void release_user_spinlock(size_t index);

    // ─── Counter/Flags API (64-bit only) ───
    /** 64-bit counters: index 0..NUM_COUNTERS_64-1 */
    void set_counter_64(size_t index, uint64_t value);
    uint64_t get_counter_64(size_t index) const;

    /** Total counter slots in the chain. */
    uint32_t counter_count() const;

    // ─── Checksum API (BLAKE2b via libsodium; stored in control zone) ───
    /** Compute BLAKE2b of flexible zone, store in header. Returns true on success. */
    bool update_checksum_flexible_zone();
    /** Compute BLAKE2b of data slot at index, store in header. Slot layout: structured_buffer_size/ring_capacity. */
    bool update_checksum_slot(size_t slot_index);

    // ─── Iterator API ───
    DataBlockIterator begin();
    DataBlockIterator end();

    /** Construct from implementation (for factory use; Impl is opaque to users). */
    explicit DataBlockProducer(std::unique_ptr<DataBlockProducerImpl> impl);

  private:
    std::unique_ptr<DataBlockProducerImpl> pImpl;
};

/**
 * @class DataBlockConsumer
 * @brief Consumer handle for a DataBlock. ABI-stable via pImpl.
 */
class PYLABHUB_UTILS_EXPORT DataBlockConsumer
{
  public:
    DataBlockConsumer();
    ~DataBlockConsumer();
    DataBlockConsumer(DataBlockConsumer &&other) noexcept;
    DataBlockConsumer &operator=(DataBlockConsumer &&other) noexcept;
    DataBlockConsumer(const DataBlockConsumer &) = delete;
    DataBlockConsumer &operator=(const DataBlockConsumer &) = delete;

    // ─── Spinlock API ───
    SharedSpinLock get_spinlock(size_t index);
    uint32_t spinlock_count() const;

    /** Legacy API. */
    SharedSpinLock get_user_spinlock(size_t index);

    // ─── Counter/Flags API (64-bit only) ───
    uint64_t get_counter_64(size_t index) const;
    void set_counter_64(size_t index, uint64_t value);
    uint32_t counter_count() const;

    // ─── Checksum API (BLAKE2b; verify stored checksum matches computed) ───
    /** Returns true if stored checksum matches computed BLAKE2b of flexible zone. */
    bool verify_checksum_flexible_zone() const;
    /** Returns true if stored checksum matches computed BLAKE2b of data slot. */
    bool verify_checksum_slot(size_t slot_index) const;

    // ─── Iterator API ───
    DataBlockIterator begin();
    DataBlockIterator end();

    /** Construct from implementation (for factory use; Impl is opaque to users). */
    explicit DataBlockConsumer(std::unique_ptr<DataBlockConsumerImpl> impl);

  private:
    std::unique_ptr<DataBlockConsumerImpl> pImpl;
};

// ─── Factory Functions ───
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                          const DataBlockConfig &config);

PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret);

/** Overload: validate version and config on attach. Returns nullptr if inconsistent. */
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                        const DataBlockConfig &expected_config);

/** @deprecated Use DataBlockProducer. Kept for compatibility. */
using IDataBlockProducer = DataBlockProducer;

/** @deprecated Use DataBlockConsumer. Kept for compatibility. */
using IDataBlockConsumer = DataBlockConsumer;

} // namespace pylabhub::hub

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
