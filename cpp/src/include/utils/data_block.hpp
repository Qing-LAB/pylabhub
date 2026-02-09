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
#include "data_block_spinlock.hpp" // Will contain SharedSpinLockState and SharedSpinLock
#include "schema_blds.hpp"         // For SchemaInfo and generate_schema_info

#include <functional>
#include <stdexcept>
#include <optional>
#include <variant>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include "slot_rw_coordinator.h" // Include C interface header
#include "slot_rw_access.hpp"    // Include Layer 1.75 template wrappers

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

// 48 bytes per slot, cache-aligned
struct PYLABHUB_UTILS_EXPORT alignas(64) SlotRWState {
    // === Writer Coordination ===
    std::atomic<uint64_t> write_lock;  // PID-based exclusive lock (0 = free)

    // === Reader Coordination ===
    std::atomic<uint32_t> reader_count;  // Active readers (multi-reader)

    // === State Machine ===
    enum class SlotState : uint8_t {
        FREE       = 0,  // Available for writing
        WRITING    = 1,  // Producer is writing
        COMMITTED  = 2,  // Data ready for reading
        DRAINING   = 3   // Waiting for readers to finish (wrap-around)
    };
    std::atomic<SlotState> slot_state;

    // === Backpressure and Coordination ===
    std::atomic<uint8_t> writer_waiting;  // Producer blocked on readers

    // === TOCTTOU Detection ===
    std::atomic<uint64_t> write_generation;  // Incremented on each commit

    // === Padding ===
    uint8_t padding[24];  // Pad to 48 bytes
};

constexpr size_t raw_size_SlotRWState = offsetof(SlotRWState, padding) + sizeof(SlotRWState::padding);
static_assert(raw_size_SlotRWState == 48, "SlotRWState must be 48 bytes");
static_assert(alignof(SlotRWState) >= 64, "SlotRWState should be cache-line aligned");


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

 * @struct FlexibleZoneConfig

 * @brief Configuration for a single flexible zone within the DataBlock.

 */

struct FlexibleZoneConfig {

    std::string name;

    size_t size;

    int spinlock_index = -1; // -1 means no dedicated spinlock

};



/**

 * @struct DataBlockConfig

 * @brief Configuration for creating a new DataBlock.

 */

struct DataBlockConfig

{

    std::string name;

    uint64_t shared_secret; // This will be generated internally, but needed for discovery

    DataBlockUnitSize unit_block_size = DataBlockUnitSize::Size4K;

    uint32_t ring_buffer_capacity;    // Slot count: 1=Single, 2=Double, N=RingBuffer

    DataBlockPolicy policy;

    bool enable_checksum = false;     // BLAKE2b checksums in control zone (flexible zone + slots)

    ChecksumPolicy checksum_policy = ChecksumPolicy::EnforceOnRelease;

    std::vector<FlexibleZoneConfig> flexible_zone_configs;



    /** Computed: Total flexible zone size. */

    size_t total_flexible_zone_size() const {

        size_t total = 0;

        for (const auto& config : flexible_zone_configs) {

            total += config.size;

        }

        return total;

    }



    /** Computed: slot_count * unit_block_size. slot_count = max(1, ring_buffer_capacity). */

    size_t structured_buffer_size() const

    {

        uint32_t slots = (ring_buffer_capacity > 0) ? ring_buffer_capacity : 1u;

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
struct alignas(4096) SharedMemoryHeader {
    // === Identification and Versioning ===
    uint32_t magic_number;          // 0x504C4842 ('PLHB')
    uint16_t version_major;         // ABI compatibility
    uint16_t version_minor;
    uint64_t total_block_size;      // Total shared memory size

    // === Security ===
    uint8_t shared_secret[64];      // Access capability token
    uint8_t schema_hash[32];        // BLAKE2b hash of data schema
    uint32_t schema_version;        // Schema version number
    uint8_t padding_sec[28];        // Align to cache line

    // === Ring Buffer Configuration ===
    DataBlockPolicy policy;         // Single/DoubleBuffer/RingBuffer
    uint32_t unit_block_size;       // Bytes per slot (power of 2)
    uint32_t ring_buffer_capacity;  // Number of slots
    size_t flexible_zone_size;      // Total TABLE 1 size
    bool enable_checksum;           // BLAKE2b checksums enabled
    ChecksumPolicy checksum_policy; // Manual or Enforced

    // === Ring Buffer State (Hot Path) ===
    std::atomic<uint64_t> write_index;   // Next slot to write (producer)
    std::atomic<uint64_t> commit_index;  // Last committed slot (producer)
    std::atomic<uint64_t> read_index;    // Oldest unread slot (system)
    std::atomic<uint32_t> active_consumer_count;

    // === Metrics Section (256 bytes) ===
    // Slot Coordination (64 bytes)
    std::atomic<uint64_t> writer_timeout_count;
    std::atomic<uint64_t> writer_blocked_total_ns;
    std::atomic<uint64_t> write_lock_contention;
    std::atomic<uint64_t> write_generation_wraps;
    std::atomic<uint64_t> reader_not_ready_count;
    std::atomic<uint64_t> reader_race_detected;
    std::atomic<uint64_t> reader_validation_failed;
    std::atomic<uint64_t> reader_peak_count;

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
    struct ConsumerHeartbeat {
        std::atomic<uint64_t> consumer_id;        // PID or UUID
        std::atomic<uint64_t> last_heartbeat_ns;  // Monotonic timestamp
        uint8_t padding[48];                       // Cache line (64 bytes total)
    } consumer_heartbeats[8];  // Max 8 consumers

    // === SharedSpinLock States (256 bytes) ===
    SharedSpinLockState spinlock_states[8];  // Fixed pool for flexible zones

    // === Padding to 4096 bytes ===
    uint8_t reserved_header[2686]; // Calculated: 4096 - 16 - 128 - 25 - 28 - 256 - 512 - 256 = 2875. No, sum needs to be re-done carefully.
                                    // 16 (ID) + 128 (Security) + 25 (Config) + 28 (State) + 256 (Metrics total) + 512 (Heartbeats) + 256 (Spinlocks) = 1221
                                    // 4096 - 1221 = 2875. This seems correct. Let's recalculate the variable parts.
                                    // ID = 16
                                    // Security = 128
                                    // Config = 4 (policy) + 4 (unit_block_size) + 4 (ring_buffer_capacity) + 8 (flexible_zone_size) + 1 (enable_checksum) + 4 (checksum_policy) = 25 bytes.
                                    // State = 8 (write_index) + 8 (commit_index) + 8 (read_index) + 4 (active_consumer_count) = 28 bytes
                                    // Metrics: SlotCoord (64) + ErrorTracking (104) + HeartbeatStats (32) + PerfCount (64) = 264 bytes.
                                    // Consumer Heartbeats: 512 bytes
                                    // SharedSpinLock States: 256 bytes
                                    // Total: 16 + 128 + 25 + 28 + 264 + 512 + 256 = 1229 bytes.
                                    // 4096 - 1229 = 2867 bytes.
                                    // Let's go with 2867.
};
constexpr size_t raw_size_SharedMemoryHeader = offsetof(SharedMemoryHeader, reserved_header) + sizeof(SharedMemoryHeader::reserved_header);
static_assert(raw_size_SharedMemoryHeader == 4096, "Header must be exactly 4KB");
static_assert(alignof(SharedMemoryHeader) >= 4096, "SharedMemoryHeader should be page-border aligned");

// Forward declarations for slot handles (primitive data transfer API)
struct SlotWriteHandleImpl;
struct SlotConsumeHandleImpl;
struct DataBlockSlotIteratorImpl;

/**
 * @struct FlexibleZoneInfo
 * @brief Runtime information for a flexible zone.
 */
struct FlexibleZoneInfo {
    size_t offset;
    size_t size;
    int spinlock_index;
};


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
    std::span<std::byte> flexible_zone_span(size_t flexible_zone_idx = 0);

    /** @brief Copy into slot buffer with bounds check. */
    bool write(const void *src, size_t len, size_t offset = 0);
    /** @brief Commit written data; makes it visible to consumers. */
    bool commit(size_t bytes_written);

    /** @brief Update checksum for this slot (if enabled). */
    bool update_checksum_slot();
    /** @brief Update checksum for flexible zone (if enabled). */
    bool update_checksum_flexible_zone(size_t flexible_zone_idx = 0);

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
    std::span<const std::byte> flexible_zone_span(size_t flexible_zone_idx = 0) const;

    /** @brief Copy out of slot buffer with bounds check. */
    bool read(void *dst, size_t len, size_t offset = 0) const;

    /** @brief Verify checksum for this slot (if enabled). */
    bool verify_checksum_slot() const;
    /** @brief Verify checksum for flexible zone (if enabled). */
    bool verify_checksum_flexible_zone(size_t flexible_zone_idx = 0) const;

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

    // ─── Shared Spinlock API ───
    /** Acquire spinlock by index; returns owning guard. Throws if index invalid. */
    std::unique_ptr<SharedSpinLockGuardOwning> acquire_spinlock(size_t index,
                                                               const std::string &debug_name = "");
    /** Get SharedSpinLock for direct use by index. */
    SharedSpinLock get_spinlock(size_t index);
    /** Total number of spinlocks (MAX_SHARED_SPINLOCKS). */
    uint32_t spinlock_count() const;

    // ─── Flexible Zone Access ───
    template <typename T>
    T& flexible_zone(size_t index) {
        // Implementation will check index and type size against FlexibleZoneInfo
        // For now, this is a placeholder. Real implementation in .cpp
        // For now, just reinterpret_cast from flexible_zone_span(index).data()
        std::span<std::byte> span = flexible_zone_span(index);
        if (span.size() < sizeof(T)) {
            throw std::runtime_error("Flexible zone too small for type T");
        }
        return *reinterpret_cast<T*>(span.data());
    }
    std::span<std::byte> flexible_zone_span(size_t index = 0);

    // ─── Checksum API (BLAKE2b via libsodium; stored in control zone) ───
    /** Compute BLAKE2b of flexible zone, store in header. Returns true on success. */
    bool update_checksum_flexible_zone(size_t flexible_zone_idx = 0);
    /** Compute BLAKE2b of data slot at index, store in header. Slot layout: structured_buffer_size/ring_capacity. */
    bool update_checksum_slot(size_t slot_index);

    // ─── Primitive Data Transfer API ───
    /** Acquire a slot for writing; returns nullptr on timeout. */
    std::unique_ptr<SlotWriteHandle> acquire_write_slot(int timeout_ms = 0);
    /** Release a previously acquired slot; returns false if checksum verification failed. */
    bool release_write_slot(SlotWriteHandle &handle);

    // ─── Broker and Health Management ───
    /** @brief Registers the producer with the broker. */
    bool register_with_broker(MessageHub &hub, const std::string &channel_name);
    /** @brief Checks the health of registered consumers and cleans up dead ones. */
    void check_consumer_health();

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

    // ─── Shared Spinlock API ───
    SharedSpinLock get_spinlock(size_t index);
    uint32_t spinlock_count() const;

    // ─── Flexible Zone Access ───
    template <typename T>
    const T& flexible_zone(size_t index) const {
        // Implementation will check index and type size against FlexibleZoneInfo
        // For now, this is a placeholder. Real implementation in .cpp
        std::span<const std::byte> span = flexible_zone_span(index);
        if (span.size() < sizeof(T)) {
            throw std::runtime_error("Flexible zone too small for type T");
        }
        return *reinterpret_cast<const T*>(span.data());
    }
    std::span<const std::byte> flexible_zone_span(size_t index = 0) const;

    // ─── Checksum API (BLAKE2b; verify stored checksum matches computed) ───
    /** Returns true if stored checksum matches computed BLAKE2b of flexible zone. */
    bool verify_checksum_flexible_zone(size_t flexible_zone_idx = 0) const;
    /** Returns true if stored checksum matches computed BLAKE2b of data slot. */
    bool verify_checksum_slot(size_t slot_index) const;

    // --- Heartbeat Management ---
    /** @brief Registers the consumer in the heartbeat table. Returns the slot index or -1 on failure. */
    int register_heartbeat();
    /** @brief Updates the heartbeat for the given slot. */
    void update_heartbeat(int slot);
    /** @brief Unregisters the consumer from the heartbeat table. */
    void unregister_heartbeat(int slot);

    // ─── Primitive Data Transfer API ───
    /** Acquire a slot for reading; returns nullptr on timeout. */
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(int timeout_ms = 0);
    /** Release a previously acquired slot; returns false if checksum verification failed. */
    bool release_consume_slot(SlotConsumeHandle &handle);

    /** Iterator for ring-buffer slots (consumer view). */
    DataBlockSlotIterator slot_iterator();

    // ─── Broker Discovery ───
    /** @brief Discovers a producer via the broker and attaches as a consumer. */
    static std::unique_ptr<DataBlockConsumer> discover(MessageHub& hub, const std::string& channel_name, uint64_t shared_secret, const DataBlockConfig& expected_config);

    /** Construct from implementation (for factory use; Impl is opaque to users). */
    explicit DataBlockConsumer(std::unique_ptr<DataBlockConsumerImpl> impl);

  private:
    std::unique_ptr<DataBlockConsumerImpl> pImpl;
};

// Forward declarations of transaction guards
class WriteTransactionGuard;
class ReadTransactionGuard;

/**
 * @brief Executes a write transaction on a DataBlock producer.
 *
 * This function acquires a write slot, executes the provided function with the
 * slot handle, and automatically releases the slot upon completion, even if
 * an exception is thrown.
 *
 * @tparam Func A callable that takes a `SlotWriteHandle&`.
 * @param producer The `DataBlockProducer` to operate on.
 * @param timeout_ms The timeout in milliseconds to acquire a write slot.
 * @param func The function to execute with the acquired slot.
 * @return The return value of the provided function.
 */
template <typename Func>
auto with_write_transaction(DataBlockProducer& producer, 
                            int timeout_ms, 
                            Func&& func) 
    -> std::invoke_result_t<Func, SlotWriteHandle&> 
{
    WriteTransactionGuard guard(producer, timeout_ms);
    if (!guard) {
        throw std::runtime_error("Failed to acquire write slot in transaction");
    }
    return std::invoke(std::forward<Func>(func), guard.slot());
}

/**
 * @brief Executes a read transaction on a DataBlock consumer for a specific slot ID.
 *
 * This function acquires a specific slot for reading, executes the provided
 * function with the slot handle, and automatically releases the slot upon
 * completion.
 *
 * @tparam Func A callable that takes a `const SlotConsumeHandle&`.
 * @param consumer The `DataBlockConsumer` to operate on.
 * @param slot_id The ID of the slot to read.
 * @param timeout_ms The timeout in milliseconds to acquire the slot.
 * @param func The function to execute with the acquired slot.
 * @return The return value of the provided function.
 */
template <typename Func>
auto with_read_transaction(
    DataBlockConsumer& consumer,
    uint64_t slot_id,
    int timeout_ms,
    Func&& func
) -> std::invoke_result_t<Func, const SlotConsumeHandle&>
{
    ReadTransactionGuard guard(consumer, slot_id, timeout_ms);
    if (!guard) {
        throw std::runtime_error("Failed to acquire consume slot in transaction");
    }
    return std::invoke(std::forward<Func>(func), guard.slot());
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
 * @return An `std::optional` containing the return value of the lambda, or `std::nullopt` on timeout.
 */
template<typename Func>
auto with_next_slot(
    DataBlockSlotIterator& iterator,
    int timeout_ms,
    Func&& lambda
) -> std::optional<std::conditional_t<std::is_void_v<std::invoke_result_t<Func, const SlotConsumeHandle&>>,
                                       std::monostate,
                                       std::invoke_result_t<Func, const SlotConsumeHandle&>>>
{
    using LambdaReturnType = std::invoke_result_t<Func, const SlotConsumeHandle&>;
    using OptionalWrappedType = std::conditional_t<std::is_void_v<LambdaReturnType>,
                                                   std::monostate,
                                                   LambdaReturnType>;
    using ReturnOptionalType = std::optional<OptionalWrappedType>;

    auto result = iterator.try_next(timeout_ms);
    
    if (!result.ok) {
        return ReturnOptionalType();
    }
    
    try {
        if constexpr (std::is_void_v<LambdaReturnType>) {
            std::invoke(std::forward<Func>(lambda), result.next);
            return ReturnOptionalType(std::monostate());
        } else {
            return ReturnOptionalType(std::invoke(std::forward<Func>(lambda), result.next));
        }
    } catch (...) {
        throw;
    }
}

/**
 * @class WriteTransactionGuard
 * @brief RAII guard for managing a DataBlockProducer write slot.
 *
 * This class acquires a write slot on construction and guarantees its release
 * on destruction, simplifying resource management and ensuring correctness
 * even in the presence of exceptions.
 */
class PYLABHUB_UTILS_EXPORT WriteTransactionGuard {
public:
    /**
     * @brief Constructs a `WriteTransactionGuard` and acquires a write slot.
     * @param producer The producer to acquire the slot from.
     * @param timeout_ms The maximum time to wait for a slot.
     */
    explicit WriteTransactionGuard(
        DataBlockProducer& producer,
        int timeout_ms
    );
    
    // Movable, not copyable
    WriteTransactionGuard(WriteTransactionGuard&&) noexcept;
    WriteTransactionGuard& operator=(WriteTransactionGuard&&) noexcept;
    
    WriteTransactionGuard(const WriteTransactionGuard&) = delete;
    WriteTransactionGuard& operator=(const WriteTransactionGuard&) = delete;
    
    /**
     * @brief Destructor that releases the acquired slot if it's still held.
     */
    ~WriteTransactionGuard() noexcept;
    
    /**
     * @brief Checks if the guard successfully acquired a slot.
     * @return `true` if a slot is held, `false` otherwise.
     */
    explicit operator bool() const noexcept;

    /**
     * @brief Provides access to the underlying `SlotWriteHandle`.
     * @return A reference to the held slot handle.
     * @throws `std::runtime_error` if no slot is held.
     */
    SlotWriteHandle& slot() noexcept;
    
    /**
     * @brief Marks the transaction as committed. The user must still call `slot().commit()`.
     */
    void commit();

    /**
     * @brief Marks the transaction as aborted, preventing release on destruction.
     */
    void abort() noexcept;

private:
    DataBlockProducer* producer_;
    std::unique_ptr<SlotWriteHandle> slot_;
    bool acquired_;
    bool committed_;
    bool aborted_;
};

/**
 * @class ReadTransactionGuard
 * @brief RAII guard for managing a DataBlockConsumer read slot.
 *
 * This class acquires a read slot on construction and guarantees its release
 * on destruction.
 */
class PYLABHUB_UTILS_EXPORT ReadTransactionGuard {
public:
    /**
     * @brief Constructs a `ReadTransactionGuard` and acquires a read slot.
     * @param consumer The consumer to acquire the slot from.
     * @param slot_id The ID of the slot to acquire.
     * @param timeout_ms The maximum time to wait for the slot.
     */
    explicit ReadTransactionGuard(
        DataBlockConsumer& consumer,
        uint64_t slot_id,
        int timeout_ms
    );
    
    // Movable, not copyable
    ReadTransactionGuard(ReadTransactionGuard&&) noexcept;
    ReadTransactionGuard& operator=(ReadTransactionGuard&&) noexcept;
    
    ReadTransactionGuard(const ReadTransactionGuard&) = delete;
    ReadTransactionGuard& operator=(const ReadTransactionGuard&) = delete;
    
    /**
     * @brief Destructor that releases the acquired slot.
     */
    ~ReadTransactionGuard() noexcept;
    
    /**
     * @brief Checks if the guard successfully acquired a slot.
     * @return `true` if a slot is held, `false` otherwise.
     */
    explicit operator bool() const noexcept;

    /**
     * @brief Provides access to the underlying `SlotConsumeHandle`.
     * @return A const reference to the held slot handle.
     * @throws `std::runtime_error` if no slot is held.
     */
    const SlotConsumeHandle& slot() const noexcept;

private:
    DataBlockConsumer* consumer_;
    std::unique_ptr<SlotConsumeHandle> slot_;
    bool acquired_;
};

// ─── Factory Functions ───
template <typename Schema>
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                          const DataBlockConfig &config, const Schema& schema_instance);

PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockProducer>
create_datablock_producer(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                          const DataBlockConfig &config);

template <typename Schema>
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                        const DataBlockConfig &expected_config, const Schema& schema_instance);

PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                        const DataBlockConfig &expected_config);


PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret);

/** Overload: validate version and config on attach. Returns nullptr if inconsistent. */
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                        const DataBlockConfig &expected_config);

// ─── Internal Implementation Functions (not for public use) ───
// These are used by template functions to pass schema information

/**
 * @brief Internal: Creates producer with optional schema storage.
 * @param schema_info If non-null, stores schema hash and version in SharedMemoryHeader.
 */
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockProducer>
create_datablock_producer_impl(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                               const DataBlockConfig &config,
                               const pylabhub::schema::SchemaInfo* schema_info);

/**
 * @brief Internal: Finds consumer with optional config/schema validation.
 * @param expected_config If non-null, validates against stored config.
 * @param schema_info If non-null, validates against stored schema hash.
 */
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockConsumer>
find_datablock_consumer_impl(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                             const DataBlockConfig* expected_config,
                             const pylabhub::schema::SchemaInfo* schema_info);

/** @deprecated Use DataBlockProducer. Kept for compatibility. */
using IDataBlockProducer = DataBlockProducer;

/** @deprecated Use DataBlockConsumer. Kept for compatibility. */
using IDataBlockConsumer = DataBlockConsumer;

// ============================================================================
// Template Factory Function Implementations (must be in header for templates)
// ============================================================================

/**
 * @brief Creates a DataBlock producer with schema validation (template version).
 * @details This template version generates schema information at compile-time
 *          from the Schema type and stores it in SharedMemoryHeader for
 *          consumer validation.
 *
 * @tparam Schema The C++ struct type used for data slots.
 * @param hub MessageHub for broker registration.
 * @param name Channel name for discovery.
 * @param policy Buffer policy (Single, DoubleBuffer, RingBuffer).
 * @param config DataBlock configuration.
 * @param schema_instance Dummy parameter for template type deduction (unused).
 * @return Unique pointer to DataBlockProducer with schema validation enabled.
 *
 * @note Requires PYLABHUB_SCHEMA_BEGIN/END macros to be defined for Schema type.
 *
 * @example
 * PYLABHUB_SCHEMA_BEGIN(SensorData)
 *     PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
 *     PYLABHUB_SCHEMA_MEMBER(temperature)
 * PYLABHUB_SCHEMA_END(SensorData)
 *
 * auto producer = create_datablock_producer<SensorData>(
 *     hub, "sensor_temp", DataBlockPolicy::RingBuffer, config, SensorData{}
 * );
 */
template <typename Schema>
std::unique_ptr<DataBlockProducer> create_datablock_producer(
    MessageHub& hub,
    const std::string& name,
    DataBlockPolicy policy,
    const DataBlockConfig& config,
    const Schema& schema_instance)
{
    (void)schema_instance; // Unused, just for template deduction

    // Generate schema info at compile-time
    auto schema_info = pylabhub::schema::generate_schema_info<Schema>(
        name,
        pylabhub::schema::SchemaVersion{1, 0, 0}
    );

    // Call internal implementation with schema info
    return create_datablock_producer_impl(hub, name, policy, config, &schema_info);
}

/**
 * @brief Discovers and attaches to a DataBlock consumer with schema validation (template version).
 * @details This template version generates expected schema at compile-time
 *          and validates it against the producer's schema stored in SharedMemoryHeader.
 *
 * @tparam Schema The expected C++ struct type for data slots.
 * @param hub MessageHub for broker discovery.
 * @param name Channel name to discover.
 * @param shared_secret Access capability token.
 * @param expected_config Expected DataBlock configuration.
 * @param schema_instance Dummy parameter for template type deduction (unused).
 * @return Unique pointer to DataBlockConsumer.
 * @throws pylabhub::schema::SchemaValidationException if schema doesn't match producer.
 *
 * @note Requires PYLABHUB_SCHEMA_BEGIN/END macros to be defined for Schema type.
 *
 * @example
 * auto consumer = find_datablock_consumer<SensorData>(
 *     hub, "sensor_temp", secret, config, SensorData{}
 * );
 */
template <typename Schema>
std::unique_ptr<DataBlockConsumer> find_datablock_consumer(
    MessageHub& hub,
    const std::string& name,
    uint64_t shared_secret,
    const DataBlockConfig& expected_config,
    const Schema& schema_instance)
{
    (void)schema_instance; // Unused, just for template deduction

    // Generate expected schema at compile-time
    auto expected_schema = pylabhub::schema::generate_schema_info<Schema>(
        name,
        pylabhub::schema::SchemaVersion{1, 0, 0}
    );

    // Call internal implementation with expected schema for validation
    return find_datablock_consumer_impl(hub, name, shared_secret, &expected_config, &expected_schema);
}

} // namespace pylabhub::hub

#if defined(_MSC_VER)
#pragma warning(pop)
#endif