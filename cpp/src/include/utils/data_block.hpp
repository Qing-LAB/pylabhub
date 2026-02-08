#pragma once
/**
 * @file data_block.hpp
 * @brief Shared memory data block with producer/consumer coordination.
 *
 * Design: Single shared-memory block, counters/flags, slot iterator.
 * All public classes use pImpl for ABI stability. 
 * See docs/hep/hep-core-0002-data-hub.md for complete design specification.
 */
#include "pylabhub_utils_export.h"
#include "data_header_sync_primitives.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

/** Format of the flexible data zone (single block). */
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
 * @enum ChecksumPolicy
 * @brief Defines checksum enforcement behavior when checksums are enabled.
 */
enum class ChecksumPolicy
{
    Explicit,          // User explicitly calls update/verify primitives
    EnforceOnRelease   // Enforce update/verify at slot release
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
    ChecksumPolicy checksum_policy = ChecksumPolicy::EnforceOnRelease;

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
 * Single-block design. Expansion is handled by creating a new larger block and
 * handing over to it (old block remains valid until all consumers detach).
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
    // Section 2: Consumer Management & Data Indices (40 bytes)
    // ──────────────────────────────────────────────────
    std::atomic<uint32_t> active_consumer_count;
    std::atomic<uint64_t> write_index;
    std::atomic<uint64_t> commit_index;
    std::atomic<uint64_t> read_index;
    std::atomic<uint64_t> current_slot_id;

    // ──────────────────────────────────────────────────
    // Section 3: Internal Management Mutex (64 bytes)
    // POSIX: pthread_mutex_t storage; Windows: reserved (uses named kernel mutex)
    // ──────────────────────────────────────────────────
    char management_mutex_storage[64];

    // ──────────────────────────────────────────────────
    // Section 4: User Spinlocks (128 bytes)
    // ──────────────────────────────────────────────────
    static constexpr size_t MAX_SHARED_SPINLOCKS = 8;
    SharedSpinLockState shared_spinlocks[MAX_SHARED_SPINLOCKS];
    std::atomic_flag spinlock_allocated[MAX_SHARED_SPINLOCKS];

    // ──────────────────────────────────────────────────
    // Section 5: 64-bit Counters/Flags (64 bytes)
    // Atomic slots for timestamps, counts, bit flags.
    // User coordinates access via spinlocks when needed.
    // ──────────────────────────────────────────────────
    static constexpr size_t NUM_COUNTERS_64 = 8;
    std::atomic<uint64_t> counters_64[NUM_COUNTERS_64];

    // ──────────────────────────────────────────────────
    // Section 6: Flexible Zone & Buffer Metadata (28 bytes)
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
    // Section 7: Integrity Checksums (BLAKE2b via libsodium)
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

// Forward declarations for slot handles (primitive data transfer API)
struct SlotWriteHandleImpl;
struct SlotConsumeHandleImpl;
struct DataBlockSlotIteratorImpl;

/**
 * @class SlotWriteHandle
 * @brief Primitive write handle for a single data slot (producer).
 */
class PYLABHUB_UTILS_EXPORT SlotWriteHandle
{
  public:
    SlotWriteHandle();
    ~SlotWriteHandle();
    SlotWriteHandle(SlotWriteHandle &&other) noexcept;
    SlotWriteHandle &operator=(SlotWriteHandle &&other) noexcept;
    SlotWriteHandle(const SlotWriteHandle &) = delete;
    SlotWriteHandle &operator=(const SlotWriteHandle &) = delete;

    /** @brief Slot index within the ring buffer. */
    size_t slot_index() const;
    /** @brief Monotonic slot id (write_index value). */
    uint64_t slot_id() const;

    /** @brief Mutable view of the slot buffer. */
    std::span<std::byte> buffer_span();
    /** @brief Mutable view of the flexible zone. */
    std::span<std::byte> flexible_zone_span();

    /** @brief Copy into slot buffer with bounds check. */
    bool write(const void *src, size_t len, size_t offset = 0);
    /** @brief Commit written data; makes it visible to consumers. */
    bool commit(size_t bytes_written);

    /** @brief Update checksum for this slot (if enabled). */
    bool update_checksum_slot();
    /** @brief Update checksum for flexible zone (if enabled). */
    bool update_checksum_flexible_zone();

  private:
    friend class DataBlockProducer;
    explicit SlotWriteHandle(std::unique_ptr<SlotWriteHandleImpl> impl);
    std::unique_ptr<SlotWriteHandleImpl> pImpl;
};

/**
 * @class SlotConsumeHandle
 * @brief Primitive read handle for a single data slot (consumer).
 */
class PYLABHUB_UTILS_EXPORT SlotConsumeHandle
{
  public:
    SlotConsumeHandle();
    ~SlotConsumeHandle();
    SlotConsumeHandle(SlotConsumeHandle &&other) noexcept;
    SlotConsumeHandle &operator=(SlotConsumeHandle &&other) noexcept;
    SlotConsumeHandle(const SlotConsumeHandle &) = delete;
    SlotConsumeHandle &operator=(const SlotConsumeHandle &) = delete;

    /** @brief Slot index within the ring buffer. */
    size_t slot_index() const;
    /** @brief Monotonic slot id (commit_index value). */
    uint64_t slot_id() const;

    /** @brief Read-only view of the slot buffer. */
    std::span<const std::byte> buffer_span() const;
    /** @brief Read-only view of the flexible zone. */
    std::span<const std::byte> flexible_zone_span() const;

    /** @brief Copy out of slot buffer with bounds check. */
    bool read(void *dst, size_t len, size_t offset = 0) const;

    /** @brief Verify checksum for this slot (if enabled). */
    bool verify_checksum_slot() const;
    /** @brief Verify checksum for flexible zone (if enabled). */
    bool verify_checksum_flexible_zone() const;

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
    ~DataBlockSlotIterator();
    DataBlockSlotIterator(DataBlockSlotIterator &&other) noexcept;
    DataBlockSlotIterator &operator=(DataBlockSlotIterator &&other) noexcept;
    DataBlockSlotIterator(const DataBlockSlotIterator &) = delete;
    DataBlockSlotIterator &operator=(const DataBlockSlotIterator &) = delete;

    /** @brief Advance to next available slot; returns ok=false on timeout. */
    NextResult try_next(int timeout_ms = 0);
    /** @brief Advance to next available slot; throws on timeout. */
    SlotConsumeHandle next(int timeout_ms = 0);

    /** @brief Set cursor to latest committed slot (no consumption). */
    void seek_latest();
    /** @brief Set cursor to a specific slot id (next() returns newer). */
    void seek_to(uint64_t slot_id);

    uint64_t last_slot_id() const;
    bool is_valid() const;

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

    // ─── Spinlock API (index 0..MAX_SHARED_SPINLOCKS-1) ───
    /** Acquire spinlock by index; returns owning guard. Throws if index invalid. */
    std::unique_ptr<SharedSpinLockGuardOwning> acquire_spinlock(size_t index,
                                                               const std::string &debug_name = "");

    /** Release spinlock allocation (producer only; makes slot available for re-acquisition). */
    void release_spinlock(size_t index);

    /** Get SharedSpinLock for direct use by index. */
    SharedSpinLock get_spinlock(size_t index);

    /** Total number of spinlocks (MAX_SHARED_SPINLOCKS). */
    uint32_t spinlock_count() const;

    /** Acquire next free spinlock slot (legacy API). */
    std::unique_ptr<SharedSpinLockGuardOwning> acquire_user_spinlock(const std::string &debug_name);

    /** Release by index (legacy API). */
    void release_user_spinlock(size_t index);

    // ─── Counter/Flags API (64-bit only) ───
    /** 64-bit counters: index 0..NUM_COUNTERS_64-1 */
    void set_counter_64(size_t index, uint64_t value);
    uint64_t get_counter_64(size_t index) const;

    /** Total counter slots (NUM_COUNTERS_64). */
    uint32_t counter_count() const;

    // ─── Checksum API (BLAKE2b via libsodium; stored in control zone) ───
    /** Compute BLAKE2b of flexible zone, store in header. Returns true on success. */
    bool update_checksum_flexible_zone();
    /** Compute BLAKE2b of data slot at index, store in header. Slot layout: structured_buffer_size/ring_capacity. */
    bool update_checksum_slot(size_t slot_index);

    // ─── Primitive Data Transfer API ───
    /** Acquire a slot for writing; returns nullptr on timeout. */
    std::unique_ptr<SlotWriteHandle> acquire_write_slot(int timeout_ms = 0);
    /** Release a previously acquired slot; returns false if checksum verification failed. */
    bool release_write_slot(SlotWriteHandle &handle);

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

    // ─── Primitive Data Transfer API ───
    /** Acquire a slot for reading; returns nullptr on timeout. */
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(int timeout_ms = 0);
    /** Release a previously acquired slot; returns false if checksum verification failed. */
    bool release_consume_slot(SlotConsumeHandle &handle);

    /** Iterator for ring-buffer slots (consumer view). */
    DataBlockSlotIterator slot_iterator();

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
