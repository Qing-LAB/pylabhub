#include "plh_service.hpp" // Includes crypto_utils, logger, lifecycle
#include "plh_platform.hpp"
#include "utils/data_block.hpp"
#include "utils/deterministic_checksum.hpp"
#include "utils/message_hub.hpp"
#include "utils/data_block_mutex.hpp"
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pylabhub::hub
{
// Forward declaration: DataBlock is defined later in this TU (internal helper class).
class DataBlock;

namespace detail
{
/// Producer heartbeat: at PRODUCER_HEARTBEAT_OFFSET, [0]=producer_id, [1]=producer_last_heartbeat_ns.
inline std::atomic<uint64_t> *producer_heartbeat_id_ptr(SharedMemoryHeader *header)
{
    return reinterpret_cast<std::atomic<uint64_t> *>(header->reserved_header +
                                                    PRODUCER_HEARTBEAT_OFFSET);
}
inline std::atomic<uint64_t> *producer_heartbeat_ns_ptr(SharedMemoryHeader *header)
{
    return reinterpret_cast<std::atomic<uint64_t> *>(header->reserved_header +
                                                    PRODUCER_HEARTBEAT_OFFSET) +
           1;
}

/// Update producer heartbeat. Call on slot commit and on explicit update_heartbeat().
inline void update_producer_heartbeat_impl(SharedMemoryHeader *header, uint64_t pid)
{
    if (header == nullptr) {
        return;
    }
    uint64_t now = pylabhub::platform::monotonic_time_ns();
    producer_heartbeat_id_ptr(header)->store(pid, std::memory_order_release);
    producer_heartbeat_ns_ptr(header)->store(now, std::memory_order_release);
}

/// Returns true if producer heartbeat exists for this pid and is fresh (within threshold).
inline bool is_producer_heartbeat_fresh(const SharedMemoryHeader *header, uint64_t pid)
{
    if (header == nullptr || pid == 0) {
        return false;
    }
    uint64_t stored_id =
        reinterpret_cast<const std::atomic<uint64_t> *>(
            header->reserved_header + PRODUCER_HEARTBEAT_OFFSET)
            ->load(std::memory_order_acquire);
    if (stored_id != pid) {
        return false;
    }
    constexpr size_t kProducerHeartbeatNsOffset = sizeof(uint64_t);
    uint64_t stored_ns =
        reinterpret_cast<const std::atomic<uint64_t> *>(
            header->reserved_header + PRODUCER_HEARTBEAT_OFFSET + kProducerHeartbeatNsOffset)
            ->load(std::memory_order_acquire);
    uint64_t now = pylabhub::platform::monotonic_time_ns();
    return (now - stored_ns) <= PRODUCER_HEARTBEAT_STALE_THRESHOLD_NS;
}

/// Returns true if writer (pid) is alive. Uses producer heartbeat if fresh; otherwise is_process_alive.
inline bool is_writer_alive_impl(const SharedMemoryHeader *header, uint64_t pid)
{
    if (pid == 0) {
        return false;
    }
    if (header != nullptr && is_producer_heartbeat_fresh(header, pid)) {
        return true;
    }
    return pylabhub::platform::is_process_alive(pid);
}

} // namespace detail

// ============================================================================
// Constants
// ============================================================================
namespace
{
// Use backoff from utils module (provided by backoff_strategy.hpp)
using pylabhub::utils::backoff;

// Import version constants from header
using pylabhub::hub::detail::HEADER_VERSION_MAJOR;
using pylabhub::hub::detail::HEADER_VERSION_MINOR;
using pylabhub::hub::detail::MAX_CONSUMER_HEARTBEATS;
using pylabhub::hub::detail::MAX_SHARED_SPINLOCKS;
using pylabhub::hub::detail::CONSUMER_READ_POSITIONS_OFFSET;
using pylabhub::hub::detail::PRODUCER_HEARTBEAT_OFFSET;
using pylabhub::hub::detail::PRODUCER_HEARTBEAT_STALE_THRESHOLD_NS;

// Use detail::DATABLOCK_MAGIC_NUMBER and detail::is_header_magic_valid from data_block.hpp
constexpr uint16_t DATABLOCK_VERSION_MAJOR = HEADER_VERSION_MAJOR;
constexpr uint16_t DATABLOCK_VERSION_MINOR = HEADER_VERSION_MINOR;
constexpr uint64_t INVALID_SLOT_ID = std::numeric_limits<uint64_t>::max();

/// Sync_reader: pointer to the slot_index-th consumer's next-read slot id in reserved_header.
/// Offset into reserved_header for Sync_reader; must match CONSUMER_READ_POSITIONS_OFFSET layout (8 * uint64_t).
inline std::atomic<uint64_t> *consumer_next_read_slot_ptr(SharedMemoryHeader *header,
                                                          size_t slot_index)
{
    return reinterpret_cast<std::atomic<uint64_t> *>(header->reserved_header +
                                                    CONSUMER_READ_POSITIONS_OFFSET) +
           slot_index;
}

constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000ULL;

/// Returns true if elapsed time since start_time_ns has exceeded timeout_ms.
/// Use in spin loops: timeout_ms 0 means no timeout (always returns false).
inline bool spin_elapsed_ms_exceeded(uint64_t start_time_ns, int timeout_ms)
{
    return timeout_ms > 0 &&
           pylabhub::platform::elapsed_time_ns(start_time_ns) / kNanosecondsPerMillisecond >=
               static_cast<uint64_t>(timeout_ms);
}

/// Slot buffer pointer: base + slot_index * slot_stride_bytes (logical stride). Single place for ring-buffer slot addressing.
inline char *slot_buffer_ptr(char *base, size_t slot_index, size_t slot_stride_bytes)
{
    return base + slot_index * slot_stride_bytes;
}
inline const char *slot_buffer_ptr(const char *base, size_t slot_index, size_t slot_stride_bytes)
{
    return base + slot_index * slot_stride_bytes;
}

/// Returns (header, slot_count) for acquisition validation; (nullptr, 0) if invalid. Defined after DataBlock.
inline std::pair<SharedMemoryHeader *, uint32_t> get_header_and_slot_count(DataBlock *dataBlock);

/**
 * Policy-based next slot to read. Single place for Latest_only / Single_reader / Sync_reader.
 * Used by DataBlockConsumer::acquire_consume_slot and DataBlockSlotIterator::try_next.
 * @return Next slot_id to try, or INVALID_SLOT_ID if none available yet.
 */
inline uint64_t get_next_slot_to_read(const SharedMemoryHeader *header, // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- (header, last_seen_or_consumed_slot_id, heartbeat_slot) order is fixed by API
                                      uint64_t last_seen_or_consumed_slot_id,
                                      int heartbeat_slot)
{
    const ConsumerSyncPolicy policy = header->consumer_sync_policy;
    if (policy == ConsumerSyncPolicy::Latest_only)
    {
        uint64_t next = header->commit_index.load(std::memory_order_acquire);
        if (next == INVALID_SLOT_ID || next == last_seen_or_consumed_slot_id)
        {
            return INVALID_SLOT_ID;
        }
        return next;
    }
    if (policy == ConsumerSyncPolicy::Single_reader)
    {
        uint64_t const commit = header->commit_index.load(std::memory_order_acquire);
        if (commit == INVALID_SLOT_ID)
        {
            return INVALID_SLOT_ID; // No slot committed yet
        }
        uint64_t next = header->read_index.load(std::memory_order_acquire);
        if (commit < next)
        {
            return INVALID_SLOT_ID;
        }
        return next;
    }
    // Sync_reader
    if (heartbeat_slot < 0 || heartbeat_slot >= static_cast<int>(MAX_CONSUMER_HEARTBEATS))
    {
        return INVALID_SLOT_ID;
    }
    uint64_t next =
        consumer_next_read_slot_ptr(const_cast<SharedMemoryHeader *>(header),
                                    static_cast<size_t>(heartbeat_slot))
            ->load(std::memory_order_acquire);
    if (header->commit_index.load(std::memory_order_acquire) < next)
    {
        return INVALID_SLOT_ID;
    }
    return next;
}

// === SlotRWState Coordination Logic ===
// Note: backoff() is now provided by pylabhub::utils::backoff() from backoff_strategy.hpp
// TOCTTOU: Reader path (acquire_read) uses double-check (reader_count then state re-check). Do not reorder without reviewing HEP and tests.

// 4.2.1 Writer Acquisition Flow
SlotAcquireResult acquire_write(SlotRWState *slot_rw_state, SharedMemoryHeader *header,
                                int timeout_ms)
{
    auto start_time = pylabhub::platform::monotonic_time_ns();
    uint64_t my_pid = pylabhub::platform::get_pid();
    int iteration = 0;

    while (true)
    {
        uint64_t expected_lock = 0;
        if (slot_rw_state->write_lock.compare_exchange_strong(
                expected_lock, my_pid, std::memory_order_acquire, std::memory_order_relaxed))
        {
            // Lock acquired
            break;
        }
        // Lock held by another process. Use heartbeat-first: only check pid if heartbeat missing/stale.
        if (detail::is_writer_alive_impl(header, expected_lock))
        {
            // Valid contention, continue waiting or timeout
        }
        else
        {
            // HIGH-RISK: Zombie lock — is_process_alive was false; we force reclaim. Do not change order: must only reclaim after confirming process is dead. See recovery docs (release_zombie_writer).
            LOGGER_WARN("SlotRWState: Detected zombie write lock by PID {}. Force reclaiming.",
                        expected_lock);
            slot_rw_state->write_lock.store(my_pid, std::memory_order_release); // Reclaim
            if (header != nullptr)
            {
                header->write_lock_contention.fetch_add(1, std::memory_order_relaxed);
            }
            break; // Acquired
        }

        // Check for timeout if lock was not acquired or was valid contention
        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            if (header != nullptr)
            {
                header->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
                header->writer_lock_timeout_count.fetch_add(1, std::memory_order_relaxed);
                LOGGER_ERROR(
                    "DataBlock acquire_write: timeout while waiting for write_lock. "
                    "pid={}, current_owner_pid={}",
                    my_pid, expected_lock);
            }
            return SLOT_ACQUIRE_TIMEOUT;
        }
        backoff(iteration++);
    }

    // Now we hold the write_lock
    slot_rw_state->writer_waiting.store(1, std::memory_order_relaxed); // Signal readers to drain

    iteration = 0;
    while (true)
    {
        std::atomic_thread_fence(std::memory_order_seq_cst); // Force visibility

        uint32_t readers = slot_rw_state->reader_count.load(std::memory_order_acquire);
        if (readers == 0)
        {
            break; // All readers finished
        }

        // Check timeout
        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            slot_rw_state->writer_waiting.store(0, std::memory_order_relaxed);
            slot_rw_state->write_lock.store(
                0, std::memory_order_release); // Release the lock before returning timeout
            if (header != nullptr)
            {
                header->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
                header->writer_reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
                LOGGER_ERROR(
                    "DataBlock acquire_write: timeout while waiting for readers to drain. "
                    "pid={}, reader_count={} (possible zombie reader).",
                    my_pid, readers);
            }
            return SLOT_ACQUIRE_TIMEOUT;
        }

        backoff(iteration++);
    }
    slot_rw_state->writer_waiting.store(0, std::memory_order_relaxed); // All readers drained

    // Transition to WRITING state
    slot_rw_state->slot_state.store(SlotRWState::SlotState::WRITING, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst); // Ensure state change is visible

    return SLOT_ACQUIRE_OK;
}

// 4.2.2 Writer Commit Flow
void commit_write(SlotRWState *slot_rw_state, SharedMemoryHeader *header)
{
    slot_rw_state->write_generation.fetch_add(
        1, std::memory_order_release); // Step 1: Increment generation counter
    slot_rw_state->slot_state.store(SlotRWState::SlotState::COMMITTED,
                                    std::memory_order_release); // Step 2: Transition to COMMITTED state
    if (header != nullptr)
    {
        header->commit_index.fetch_add(
            1, std::memory_order_release); // Step 3: Increment global commit index (makes visible
                                           // to consumers)
        header->total_slots_written.fetch_add(1, std::memory_order_release); // Metric: commit count
    }
    // Memory ordering: All writes before this release are visible to
    // any consumer that performs acquire on commit_index or slot_state
}

// 4.2.2b Writer Release (without commit) - for C API and abort paths
void release_write(SlotRWState *slot_rw_state, SharedMemoryHeader * /*header*/)
{
    slot_rw_state->write_lock.store(0, std::memory_order_release);
    slot_rw_state->slot_state.store(SlotRWState::SlotState::FREE, std::memory_order_release);
}

// 4.2.3 Reader Acquisition Flow (TOCTTOU-Safe)
SlotAcquireResult acquire_read(SlotRWState *slot_rw_state, SharedMemoryHeader *header,
                               uint64_t *out_generation)
{
    // Step 1: Check slot state (first check)
    SlotRWState::SlotState state = slot_rw_state->slot_state.load(std::memory_order_acquire);
    if (state != SlotRWState::SlotState::COMMITTED)
    {
        return SLOT_ACQUIRE_NOT_READY;
    }

    // Step 2: Register as reader (minimize race window)
    slot_rw_state->reader_count.fetch_add(1, std::memory_order_acq_rel);

    // Step 3: Memory fence (force writer visibility)
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Step 4: Double-check slot state (TOCTTOU mitigation — do not reorder with Step 2)
    state = slot_rw_state->slot_state.load(std::memory_order_acquire);
    if (state != SlotRWState::SlotState::COMMITTED)
    {
        // Race detected: writer changed state after our first check but before we registered.
        // Safely abort and decrement reader_count so writer can progress.
        slot_rw_state->reader_count.fetch_sub(1, std::memory_order_release);
        if (header != nullptr)
        {
            header->reader_race_detected.fetch_add(1, std::memory_order_relaxed);
        }
        return SLOT_ACQUIRE_NOT_READY;
    }

    // Step 5: Capture generation for optimistic validation
    *out_generation = slot_rw_state->write_generation.load(std::memory_order_acquire);

    return SLOT_ACQUIRE_OK;
}

// 4.2.4 Reader Validation (Wrap-Around Detection)
/// Returns false if generation changed (wrap-around or slot overwritten); in-flight read is then invalid.
bool validate_read_impl(SlotRWState *slot_rw_state, SharedMemoryHeader *header,
                        uint64_t captured_gen)
{
    // Check if slot was overwritten during read
    uint64_t current_gen = slot_rw_state->write_generation.load(std::memory_order_acquire);

    if (current_gen != captured_gen)
    {
        // Slot was reused (ring buffer wrapped around)
        if (header != nullptr)
        {
            header->reader_validation_failed.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    return true;
}

// 4.2.5 Reader Release Flow
void release_read(SlotRWState *slot_rw_state, SharedMemoryHeader *header)
{
    // Decrement reader count
    uint32_t prev_count = slot_rw_state->reader_count.fetch_sub(1, std::memory_order_release);

    // Track peak reader count (optional; header may be null for C API)
    if (header != nullptr)
    {
        uint64_t peak = header->reader_peak_count.load(std::memory_order_relaxed);
        if (prev_count > peak)
        {
            header->reader_peak_count.store(prev_count, std::memory_order_relaxed);
        }
    }
    // If last reader and writer is waiting, writer will proceed
    // (writer polls reader_count with acquire ordering)
}

} // namespace

// Exported for recovery and diagnostics.
bool is_writer_alive(const SharedMemoryHeader *header, uint64_t pid) noexcept
{
    return detail::is_writer_alive_impl(header, pid);
}

// ============================================================================
// DataBlockLayout – single control surface for memory model
// ============================================================================
// All layout, sizes, and derived access (slot stride, offsets) come from this struct.
// Populated once at init from config (creator) or from header (attacher). All access
// (slot buffer pointer/size, checksum region, flexible zone) uses layout only.
// Validation entry points: validate_header_layout_hash (ABI); validate_attach_layout_and_config
// (layout checksum + optional config match); used by find_datablock_consumer_impl and recovery.
// ============================================================================
struct DataBlockLayout
{
    size_t slot_rw_state_offset = 0;
    size_t slot_rw_state_size = 0;
    size_t slot_checksum_offset = 0;
    size_t slot_checksum_size = 0;
    size_t flexible_zone_offset = 0;
    size_t flexible_zone_size = 0;
    size_t structured_buffer_offset = 0;
    size_t structured_buffer_size = 0;
    size_t total_size = 0;
    /** Slot stride (bytes per slot). Single source for slot buffer pointer arithmetic. */
    size_t slot_stride_bytes_ = 0;
    /** Physical page size (bytes). Allocation granularity. */
    size_t physical_page_size_bytes = 0;
    /** Effective slot count (single source; 0 capacity treated as 1). */
    uint32_t slot_count = 0;

    static DataBlockLayout from_config(const DataBlockConfig &config)
    {
        DataBlockLayout layout{};
        layout.slot_count =
            (config.ring_buffer_capacity > 0) ? config.ring_buffer_capacity : 1U;
        layout.slot_rw_state_offset = sizeof(SharedMemoryHeader);
        layout.slot_rw_state_size = layout.slot_count * sizeof(SlotRWState);
        layout.slot_checksum_size =
            (config.checksum_type != ChecksumType::Unset)
                ? (layout.slot_count * detail::SLOT_CHECKSUM_ENTRY_SIZE)
                : 0;
        layout.slot_checksum_offset = layout.slot_rw_state_offset + layout.slot_rw_state_size;
        layout.flexible_zone_size = config.total_flexible_zone_size();
        layout.flexible_zone_offset = layout.slot_checksum_offset + layout.slot_checksum_size;
        layout.slot_stride_bytes_ = config.effective_logical_unit_size();
        layout.physical_page_size_bytes = to_bytes(config.physical_page_size);
        layout.structured_buffer_size = config.structured_buffer_size();
        // Align structured data start so slot buffers meet alignof(T) for with_typed_write/read.
        // Physical page size governs slot stride; the offset of the structured region was packed (no
        // padding) and thus misaligned. Single constant, no branches; raw and typed access both valid.
        constexpr size_t kStructuredBufferAlignment = 8;
        const size_t after_flexible = layout.flexible_zone_offset + layout.flexible_zone_size;
        layout.structured_buffer_offset =
            (after_flexible + kStructuredBufferAlignment - 1) & ~(kStructuredBufferAlignment - 1);
        layout.total_size = layout.structured_buffer_offset + layout.structured_buffer_size;
        return layout;
    }

    static DataBlockLayout from_header(const SharedMemoryHeader *header)
    {
        DataBlockLayout layout{};
        if (header == nullptr)
        {
            return layout;
        }
        layout.slot_count = detail::get_slot_count(header);
        layout.slot_rw_state_offset = sizeof(SharedMemoryHeader);
        layout.slot_rw_state_size = layout.slot_count * sizeof(SlotRWState);
        layout.slot_checksum_size =
            (static_cast<ChecksumType>(header->checksum_type) != ChecksumType::Unset)
                ? (layout.slot_count * detail::SLOT_CHECKSUM_ENTRY_SIZE)
                : 0;
        layout.slot_checksum_offset = layout.slot_rw_state_offset + layout.slot_rw_state_size;
        layout.flexible_zone_size = header->flexible_zone_size;
        layout.flexible_zone_offset = layout.slot_checksum_offset + layout.slot_checksum_size;
        layout.slot_stride_bytes_ = static_cast<size_t>(detail::get_slot_stride_bytes(header));
        layout.physical_page_size_bytes = static_cast<size_t>(header->physical_page_size);
        layout.structured_buffer_size = layout.slot_count * layout.slot_stride_bytes_;
        const size_t after_flexible = layout.flexible_zone_offset + layout.flexible_zone_size;
        constexpr size_t kStructuredBufferAlignment = 8; // Same as from_config; no legacy branch.
        layout.structured_buffer_offset =
            (after_flexible + kStructuredBufferAlignment - 1) & ~(kStructuredBufferAlignment - 1);
        layout.total_size = layout.structured_buffer_offset + layout.structured_buffer_size;
        return layout;
    }

    /** Slot buffer stride (bytes per slot). Use for all slot buffer pointer arithmetic. */
    [[nodiscard]] size_t slot_stride_bytes() const { return slot_stride_bytes_; }
    /** Physical page size (bytes). Allocation granularity. */
    [[nodiscard]] size_t physical_page_size() const { return physical_page_size_bytes; }
    /** Effective slot count. Use this for all slot index bounds; do not read header directly. */
    [[nodiscard]] uint32_t slot_count_value() const { return slot_count; }

    char *slot_checksum_base(char *segment_base) const
    {
        return segment_base + slot_checksum_offset;
    }
    const char *slot_checksum_base(const char *segment_base) const
    {
        return segment_base + slot_checksum_offset;
    }

#if !defined(NDEBUG)
    [[nodiscard]] bool validate() const
    {
        if (slot_rw_state_offset != sizeof(SharedMemoryHeader))
        {
            return false;
        }
        if (slot_checksum_offset != slot_rw_state_offset + slot_rw_state_size)
        {
            return false;
        }
        if (flexible_zone_offset != slot_checksum_offset + slot_checksum_size)
        {
            return false;
        }
        const size_t after_flexible = flexible_zone_offset + flexible_zone_size;
        if (structured_buffer_offset < after_flexible)
        {
            return false;
        }
        if (total_size != structured_buffer_offset + structured_buffer_size)
        {
            return false;
        }
        return true;
    }
#endif
};

// ============================================================================
// Flexible zone info – single source of truth
// ============================================================================
// Rules:
// 1. Layout comes from FlexibleZoneConfig list only; offset = sum of previous sizes.
// 2. DataBlock (creator): populated in ctor from config via build_flexible_zone_info.
// 3. DataBlock (attacher): populated only via set_flexible_zone_info_for_attach(configs)
//    when the factory has expected_config (consumer must agree on zone layout).
// 4. ProducerImpl/ConsumerImpl flexible_zones_info: built in factory from same config
//    that created or validated the block. Use build_flexible_zone_info() for consistency.
namespace
{
std::vector<FlexibleZoneInfo> build_flexible_zone_info(const std::vector<FlexibleZoneConfig> &configs)
{
    std::vector<FlexibleZoneInfo> out;
    out.reserve(configs.size());
    size_t offset = 0;
    for (const auto &config : configs)
    {
        FlexibleZoneInfo info{};
        info.offset = offset;
        info.size = config.size;
        info.spinlock_index = config.spinlock_index;
        out.push_back(info);
        offset += config.size;
    }
    return out;
}
} // namespace

// ============================================================================
// DataBlock - Internal helper
// ============================================================================
class DataBlock
{
  public:
    // Single point of config validation and memory creation; do not add alternate creation paths without updating this.
    DataBlock(const std::string &name, const DataBlockConfig &config)
        : m_name(name), m_is_creator(true)
    {
        if (config.policy == DataBlockPolicy::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.policy must be set explicitly (Single, DoubleBuffer, or RingBuffer).", name);
            throw std::invalid_argument("DataBlockConfig::policy must be set explicitly");
        }
        if (config.consumer_sync_policy == ConsumerSyncPolicy::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.consumer_sync_policy must be set explicitly (Latest_only, Single_reader, or Sync_reader).", name);
            throw std::invalid_argument("DataBlockConfig::consumer_sync_policy must be set explicitly");
        }
        if (config.physical_page_size == DataBlockPageSize::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.physical_page_size must be set explicitly (Size4K, Size4M, or Size16M).", name);
            throw std::invalid_argument("DataBlockConfig::physical_page_size must be set explicitly");
        }
        if (config.ring_buffer_capacity == 0)
        {
            LOGGER_ERROR("DataBlock '{}': config.ring_buffer_capacity must be set explicitly (>= 1).", name);
            throw std::invalid_argument("DataBlockConfig::ring_buffer_capacity must be set (1 or more)");
        }
        if (config.checksum_type == ChecksumType::Unset)
        {
            LOGGER_ERROR("DataBlock '{}': config.checksum_type must be set (e.g. BLAKE2b). Checksum is mandatory.", name);
            throw std::invalid_argument("DataBlockConfig::checksum_type must be set");
        }
        m_layout = DataBlockLayout::from_config(config);
        m_size = m_layout.total_size;
#if !defined(NDEBUG)
        assert(m_layout.validate() && "DataBlockLayout invariant violated");
#endif

#if defined(PYLABHUB_IS_POSIX)
        pylabhub::platform::shm_unlink(m_name.c_str()); // Ensure it's not already existing
#endif
        m_shm = pylabhub::platform::shm_create(m_name.c_str(), m_size);
        if (m_shm.base == nullptr)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            throw std::runtime_error(
                fmt::format("Failed to create file mapping for '{}'. Error: {}", m_name, GetLastError()));
#else
            throw std::runtime_error(
                fmt::format("shm_create failed for '{}'. Error: {}", m_name, errno));
#endif
        }
        // Placement new for SharedMemoryHeader (value-initialize for deterministic layout; zero padding)
        m_header = new (m_shm.base) SharedMemoryHeader{};

        // 2. Initialize SharedMemoryHeader fields
        m_header->version_major = DATABLOCK_VERSION_MAJOR;
        m_header->version_minor = DATABLOCK_VERSION_MINOR;
        m_header->total_block_size = m_size;

        pylabhub::crypto::generate_random_bytes(
            m_header->shared_secret, sizeof(m_header->shared_secret)); // Generate random secret
        if (config.shared_secret != 0)
        {
            std::memcpy(m_header->shared_secret, &config.shared_secret,
                        sizeof(config.shared_secret)); // Store capability for discovery
        }

        m_header->schema_version = 0; // Will be set by factory function if schema is used
        std::memset(m_header->schema_hash, 0, sizeof(m_header->schema_hash));

        m_header->policy = config.policy;
        m_header->consumer_sync_policy = config.consumer_sync_policy;
        m_header->physical_page_size = static_cast<uint32_t>(to_bytes(config.physical_page_size));
        {
            const size_t physical = to_bytes(config.physical_page_size);
            const size_t logical = config.effective_logical_unit_size();
            if (config.logical_unit_size != 0 && config.logical_unit_size < physical)
            {
                LOGGER_ERROR("DataBlock '{}': logical_unit_size ({}) must be >= physical_page_size ({}); "
                             "there is no case where logical < physical.",
                             m_name, config.logical_unit_size, physical);
                throw std::invalid_argument("logical_unit_size must be >= physical_page_size");
            }
            if (config.logical_unit_size != 0 && (config.logical_unit_size % physical != 0))
            {
                LOGGER_ERROR("DataBlock '{}': logical_unit_size ({}) must be a multiple of physical_page_size ({})",
                             m_name, config.logical_unit_size, physical);
                throw std::invalid_argument("logical_unit_size must be a multiple of physical_page_size");
            }
            if (logical > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
            {
                throw std::invalid_argument("logical_unit_size exceeds maximum storable in header");
            }
            // Always store resolved bytes; never 0 (0 at config input means use physical).
            m_header->logical_unit_size = static_cast<uint32_t>(logical);
        }
        m_header->ring_buffer_capacity = config.ring_buffer_capacity;
        m_header->flexible_zone_size = m_layout.flexible_zone_size;
        m_header->checksum_type = static_cast<uint8_t>(config.checksum_type);
        m_header->checksum_policy = config.checksum_policy;

        // Initialize hot path indices
        m_header->write_index.store(0, std::memory_order_release);
        m_header->commit_index.store(INVALID_SLOT_ID, std::memory_order_release);
        m_header->read_index.store(0, std::memory_order_release);
        m_header->active_consumer_count.store(0, std::memory_order_release);

        // Initialize metrics section to zero
        m_header->writer_timeout_count.store(0, std::memory_order_release);
        m_header->writer_blocked_total_ns.store(0, std::memory_order_release);
        m_header->write_lock_contention.store(0, std::memory_order_release);
        m_header->write_generation_wraps.store(0, std::memory_order_release);
        m_header->reader_not_ready_count.store(0, std::memory_order_release);
        m_header->reader_race_detected.store(0, std::memory_order_release);
        m_header->reader_validation_failed.store(0, std::memory_order_release);
        m_header->reader_peak_count.store(0, std::memory_order_release);
        m_header->reader_timeout_count.store(0, std::memory_order_release);

        m_header->last_error_timestamp_ns.store(0, std::memory_order_release);
        m_header->last_error_code.store(0, std::memory_order_release);
        m_header->error_sequence.store(0, std::memory_order_release);
        m_header->slot_acquire_errors.store(0, std::memory_order_release);
        m_header->slot_commit_errors.store(0, std::memory_order_release);
        m_header->checksum_failures.store(0, std::memory_order_release);
        m_header->zmq_send_failures.store(0, std::memory_order_release);
        m_header->zmq_recv_failures.store(0, std::memory_order_release);
        m_header->zmq_timeout_count.store(0, std::memory_order_release);
        m_header->recovery_actions_count.store(0, std::memory_order_release);
        m_header->schema_mismatch_count.store(0, std::memory_order_release);
        for (auto &reserved_error : m_header->reserved_errors)
        {
            reserved_error.store(0, std::memory_order_release);
        }

        m_header->heartbeat_sent_count.store(0, std::memory_order_release);
        m_header->heartbeat_failed_count.store(0, std::memory_order_release);
        m_header->last_heartbeat_ns.store(0, std::memory_order_release);
        m_header->reserved_hb.store(0, std::memory_order_release);

        m_header->total_slots_written.store(0, std::memory_order_release);
        m_header->total_slots_read.store(0, std::memory_order_release);
        m_header->total_bytes_written.store(0, std::memory_order_release);
        m_header->total_bytes_read.store(0, std::memory_order_release);
        m_header->uptime_seconds.store(0, std::memory_order_release);
        m_header->creation_timestamp_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch())
                .count(),
            std::memory_order_release);
        for (auto &reserved_perf_elem : m_header->reserved_perf)
        {
            reserved_perf_elem.store(0, std::memory_order_release);
        }

        // Initialize Consumer Heartbeats
        for (auto &consumer_heartbeat : m_header->consumer_heartbeats)
        {
            consumer_heartbeat.consumer_id.store(0, std::memory_order_release);
            consumer_heartbeat.last_heartbeat_ns.store(0, std::memory_order_release);
        }

        // Initialize SharedSpinLock states (same factory logic as in-process spinlock)
        for (auto &spinlock_state : m_header->spinlock_states)
        {
            init_spinlock_state(&spinlock_state);
        }

        // 3. Initialize SlotRWState array (using layout)
        m_slot_rw_states_array = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(m_shm.base) + m_layout.slot_rw_state_offset);

        for (uint32_t i = 0; i < m_layout.slot_count_value(); ++i)
        {
            m_slot_rw_states_array[i].write_lock.store(0, std::memory_order_release);
            m_slot_rw_states_array[i].reader_count.store(0, std::memory_order_release);
            m_slot_rw_states_array[i].slot_state.store(SlotRWState::SlotState::FREE,
                                                       std::memory_order_release);
            m_slot_rw_states_array[i].writer_waiting.store(0, std::memory_order_release);
            m_slot_rw_states_array[i].write_generation.store(0, std::memory_order_release);
        }

        // 4. Set pointers from layout
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_shm.base) + m_layout.flexible_zone_offset;
        m_structured_data_buffer =
            reinterpret_cast<char *>(m_shm.base) + m_layout.structured_buffer_offset;

        // Populate flexible zone info map (single source: build_flexible_zone_info)
        const auto &configs = config.flexible_zone_configs;
        auto vec = build_flexible_zone_info(configs);
        for (size_t i = 0; i < configs.size(); ++i)
        {
            m_flexible_zone_info[configs[i].name] = vec[i];
        }

        // Sync_reader: initialize per-consumer read positions in reserved_header
        for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i)
        {
            consumer_next_read_slot_ptr(m_header, i)->store(0, std::memory_order_release);
        }

        std::atomic_thread_fence(std::memory_order_release);
        m_header->magic_number.store(
            detail::DATABLOCK_MAGIC_NUMBER,
            std::memory_order_release); // Set magic number last for atomicity

        // Store header layout hash for protocol check (consumer validates same ABI)
        pylabhub::schema::SchemaInfo header_schema = get_shared_memory_header_schema_info();
        std::memcpy(m_header->reserved_header + detail::HEADER_LAYOUT_HASH_OFFSET,
                    header_schema.hash.data(), detail::HEADER_LAYOUT_HASH_SIZE);
        // Store layout checksum (segment layout-defining values; validated on attach and integrity)
        store_layout_checksum(m_header);

        // Initialize producer heartbeat (creator is initial producer)
        detail::update_producer_heartbeat_impl(m_header, pylabhub::platform::get_pid());

        LOGGER_INFO("DataBlock '{}' created with total size {} bytes.", m_name, m_size);
    }

    DataBlock(std::string name) : m_name(std::move(name)), m_is_creator(false)
    {
        m_shm = pylabhub::platform::shm_attach(m_name.c_str());
        if (m_shm.base == nullptr)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            throw std::runtime_error(
                fmt::format("Failed to open file mapping for consumer '{}'. Error: {}", m_name, GetLastError()));
#else
            throw std::runtime_error(
                fmt::format("shm_attach failed for consumer '{}'. Error: {}", m_name, errno));
#endif
        }
        m_size = m_shm.size;

        m_header = reinterpret_cast<SharedMemoryHeader *>(m_shm.base);

        // Wait for producer to fully initialize the header.
        // The magic_number being set indicates initialization is complete.
        // Timeout for robustness against crashed producers.
        const int max_wait_ms = 5000;
        const int poll_interval_ms = 10;
        int total_wait_ms = 0;
        while (!detail::is_header_magic_valid(&m_header->magic_number, detail::DATABLOCK_MAGIC_NUMBER) &&
               total_wait_ms < max_wait_ms)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
            total_wait_ms += poll_interval_ms;
        }

        if (!detail::is_header_magic_valid(&m_header->magic_number, detail::DATABLOCK_MAGIC_NUMBER))
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' initialization timeout - producer may have crashed or not fully initialized.", m_name));
        }

        // Validate version compatibility
        if (m_header->version_major != DATABLOCK_VERSION_MAJOR ||
            m_header->version_minor >
                DATABLOCK_VERSION_MINOR) // Consumer can read older minor versions
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' version mismatch. Producer: {}.{}, Consumer: {}.{}",
                            m_name, m_header->version_major, m_header->version_minor,
                            DATABLOCK_VERSION_MAJOR, DATABLOCK_VERSION_MINOR));
        }

        // Validate total size
        if (m_size != m_header->total_block_size)
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error(
                fmt::format("DataBlock '{}' size mismatch. Expected {}, got {}",
                            m_name, m_header->total_block_size, m_size));
        }

        // Calculate pointers from layout (single source of truth)
        m_layout = DataBlockLayout::from_header(m_header);
#if !defined(NDEBUG)
        assert(m_layout.validate() && "DataBlockLayout invariant violated");
#endif
        m_slot_rw_states_array = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(m_shm.base) + m_layout.slot_rw_state_offset);
        m_flexible_data_zone =
            reinterpret_cast<char *>(m_shm.base) + m_layout.flexible_zone_offset;
        m_structured_data_buffer =
            reinterpret_cast<char *>(m_shm.base) + m_layout.structured_buffer_offset;

        // Populate flexible zone info map (this is tricky for consumer as it doesn't have config)
        // This will be handled in a factory function with expected_config.
        // For now, this constructor doesn't know about individual flexible zones.

        LOGGER_INFO("DataBlock '{}' opened by consumer. Total size {} bytes.", m_name, m_size);
    }

    ~DataBlock()
    {
        pylabhub::platform::shm_close(&m_shm);
        if (m_is_creator)
        {
            pylabhub::platform::shm_unlink(m_name.c_str());
            LOGGER_INFO("DataBlock '{}' shared memory removed.", m_name);
        }
    }

    SharedMemoryHeader *header() const { return m_header; }
    char *flexible_data_zone() const { return m_flexible_data_zone; }
    char *structured_data_buffer() const { return m_structured_data_buffer; }
    void *segment() const { return m_shm.base; }
    size_t size() const { return m_size; }

    size_t acquire_shared_spinlock(const std::string &debug_name)
    {
        // Simple allocation: find first spinlock with owner_pid == 0
        for (size_t i = 0; i < MAX_SHARED_SPINLOCKS; ++i)
        {
            uint64_t expected_pid = 0;
            if (m_header->spinlock_states[i].owner_pid.compare_exchange_strong(
                    expected_pid, 1, // Use 1 as "allocated but not locked" marker
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                // Successfully claimed this spinlock slot; reset to free state (same as factory)
                init_spinlock_state(&m_header->spinlock_states[i]);
                LOGGER_INFO("DataBlock '{}': Acquired spinlock slot {} for '{}'.", m_name, i,
                            debug_name);
                return i;
            }
        }
        throw std::runtime_error(
            fmt::format("DataBlock '{}': No free spinlock slots.", m_name));
    }

    void release_shared_spinlock(size_t index)
    {
        if (index >= detail::MAX_SHARED_SPINLOCKS)
        {
            throw std::out_of_range("Spinlock index out of range.");
        }
        if (m_header->spinlock_states[index].owner_pid.load(std::memory_order_acquire) != 0)
        {
            LOGGER_WARN("DataBlock '{}': Releasing spinlock {} still held. Force releasing.",
                        m_name, index);
        }
        init_spinlock_state(&m_header->spinlock_states[index]);
        LOGGER_INFO("DataBlock '{}': Released spinlock slot {}.", m_name, index);
    }

    SharedSpinLockState *get_shared_spinlock_state(size_t index)
    {
        if (index >= detail::MAX_SHARED_SPINLOCKS)
        {
            throw std::out_of_range("Spinlock index out of range.");
        }
        return &m_header->spinlock_states[index];
    }

    SlotRWState *slot_rw_state(size_t index) const
    {
        if (m_header == nullptr || index >= m_layout.slot_count_value())
        {
            throw std::out_of_range(
                fmt::format("SlotRWState index {} out of range or header invalid.", index));
        }
        return &m_slot_rw_states_array[index];
    }
    const std::unordered_map<std::string, FlexibleZoneInfo> &flexible_zone_info() const
    {
        return m_flexible_zone_info;
    }
    /** Called by consumer factory when attaching with expected_config; populates zone map so
     * verify_checksum_flexible_zone etc. work. Uses same build_flexible_zone_info as creator. */
    void set_flexible_zone_info_for_attach(const std::vector<FlexibleZoneConfig> &configs)
    {
        m_flexible_zone_info.clear();
        auto vec = build_flexible_zone_info(configs);
        for (size_t i = 0; i < configs.size(); ++i)
        {
            m_flexible_zone_info[configs[i].name] = vec[i];
        }
    }
    const DataBlockLayout &layout() const { return m_layout; }

  private:
    std::string m_name;
    bool m_is_creator;
    pylabhub::platform::ShmHandle m_shm{};
    size_t m_size = 0; // Cached from m_shm.size for convenience
    DataBlockLayout m_layout{};

    SharedMemoryHeader *m_header = nullptr;
    SlotRWState *m_slot_rw_states_array = nullptr; // New member
    char *m_flexible_data_zone = nullptr;
    char *m_structured_data_buffer = nullptr;
    // Removed m_management_mutex as it's no longer managed by DataBlock directly
    std::unordered_map<std::string, FlexibleZoneInfo> m_flexible_zone_info; // New member
};

// ============================================================================
// DataBlockDiagnosticHandle (opaque attach for recovery/tooling)
// ============================================================================
struct DataBlockDiagnosticHandleImpl
{
    SharedMemoryHeader *header_ptr = nullptr;
    SlotRWState *slot_rw_states = nullptr;
    uint32_t ring_buffer_capacity = 0;
    pylabhub::platform::ShmHandle m_shm{};
};

namespace
{
/// Definition (DataBlock is complete here).
inline std::pair<SharedMemoryHeader *, uint32_t> get_header_and_slot_count(DataBlock *dataBlock)
{
    if (dataBlock == nullptr)
    {
        return {nullptr, 0};
    }
    SharedMemoryHeader *hdr = dataBlock->header();
    if (hdr == nullptr)
    {
        return {nullptr, 0};
    }
    return {hdr, dataBlock->layout().slot_count_value()};
}

inline bool update_checksum_flexible_zone_impl(DataBlock *block, size_t flexible_zone_idx)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    auto *hdr = block->header();

    // Get the FlexibleZoneInfo from the block's flexible_zone_info map
    if (flexible_zone_idx >= detail::MAX_FLEXIBLE_ZONE_CHECKSUMS ||
        flexible_zone_idx >= block->flexible_zone_info().size())
    {
        return false;
    }
    auto zone_iter = block->flexible_zone_info().begin();
    std::advance(zone_iter, flexible_zone_idx);
    const FlexibleZoneInfo &zone_info = zone_iter->second;

    char *flex = block->flexible_data_zone();
    size_t len = zone_info.size;              // Use size from zone_info
    char *zone_ptr = flex + zone_info.offset; // Calculate pointer to specific zone

    if (zone_ptr == nullptr || len == 0)
    {
        return false;
    }

    // Checksum data is now per flexible zone, stored in SharedMemoryHeader::flexible_zone_checksums
    if (!pylabhub::crypto::compute_blake2b(
            hdr->flexible_zone_checksums[flexible_zone_idx].checksum_bytes, zone_ptr, len))
    {
        return false;
    }

    hdr->flexible_zone_checksums[flexible_zone_idx].valid.store(
        1, std::memory_order_release); // Use valid
    return true;
}

inline bool update_checksum_slot_impl(DataBlock *block, size_t slot_index)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    if (slot_index >= block->layout().slot_count_value())
    {
        return false;
    }
    // Slot data pointer: step size = logical (slot_stride_bytes), so ring iteration uses logical size.
    const size_t slot_size = block->layout().slot_stride_bytes();
    if (slot_size == 0)
    {
        return false;
    }
    char *buf = block->structured_data_buffer();
    if (buf == nullptr)
    {
        return false;
    }
    char *base = reinterpret_cast<char *>(block->segment());
    char *slot_checksum_base_ptr = block->layout().slot_checksum_base(base);
    auto *slot_checksum = reinterpret_cast<uint8_t *>(
        slot_checksum_base_ptr + slot_index * detail::SLOT_CHECKSUM_ENTRY_SIZE);
    auto *slot_valid =
        reinterpret_cast<std::atomic<uint8_t> *>(slot_checksum + detail::CHECKSUM_BYTES);
    const void *slot_data = buf + slot_index * slot_size;
    if (!pylabhub::crypto::compute_blake2b(slot_checksum, slot_data, slot_size))
    {
        return false;
    }
    slot_valid->store(1, std::memory_order_release);
    return true;
}

inline bool verify_checksum_flexible_zone_impl(const DataBlock *block, size_t flexible_zone_idx)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    auto *hdr = block->header();

    // Get the FlexibleZoneInfo from the block's flexible_zone_info map
    if (flexible_zone_idx >= detail::MAX_FLEXIBLE_ZONE_CHECKSUMS ||
        flexible_zone_idx >= block->flexible_zone_info().size())
    {
        return false;
    }
    auto zone_iter = block->flexible_zone_info().begin();
    std::advance(zone_iter, flexible_zone_idx);
    const FlexibleZoneInfo &zone_info = zone_iter->second;

    if (hdr->flexible_zone_checksums[flexible_zone_idx].valid.load(std::memory_order_acquire) != 1)
    {
        return false;
    }

    const char *flex = block->flexible_data_zone();
    size_t len = zone_info.size;                    // Use size from zone_info
    const char *zone_ptr = flex + zone_info.offset; // Calculate pointer to specific zone

    if (zone_ptr == nullptr || len == 0)
    {
        return false;
    }

    // Checksum data is now per flexible zone, stored in SharedMemoryHeader::flexible_zone_checksums
    return pylabhub::crypto::verify_blake2b(
        hdr->flexible_zone_checksums[flexible_zone_idx].checksum_bytes, zone_ptr, len);
}

inline bool verify_checksum_slot_impl(const DataBlock *block, size_t slot_index)
{
    if (block == nullptr || block->header() == nullptr)
    {
        return false;
    }
    if (block->layout().slot_checksum_size == 0)
    {
        return false;
    }
    if (slot_index >= block->layout().slot_count_value())
    {
        return false;
    }
    const char *base = reinterpret_cast<const char *>(block->segment());
    const char *slot_checksum_base_ptr = block->layout().slot_checksum_base(base);
    const auto *slot_checksum = reinterpret_cast<const uint8_t *>(
        slot_checksum_base_ptr + slot_index * detail::SLOT_CHECKSUM_ENTRY_SIZE);
    const auto *slot_valid =
        reinterpret_cast<const std::atomic<uint8_t> *>(slot_checksum + detail::CHECKSUM_BYTES);
    if (slot_valid->load(std::memory_order_acquire) != 1)
    {
        return false;
    }
    // Step size for slot data = logical (slot_stride_bytes).
    const size_t slot_size = block->layout().slot_stride_bytes();
    if (slot_size == 0)
    {
        return false;
    }
    const char *buf = block->structured_data_buffer();
    if (buf == nullptr)
    {
        return false;
    }
    const void *slot_data = buf + slot_index * slot_size;
    return pylabhub::crypto::verify_blake2b(slot_checksum, slot_data, slot_size);
}
} // namespace

// ============================================================================
// Slot Handles (Primitive Data Transfer API)
// ============================================================================
struct DataBlockProducerImpl;
struct DataBlockConsumerImpl;

struct SlotWriteHandleImpl
{
    DataBlockProducerImpl *owner = nullptr;
    DataBlock *dataBlock = nullptr;
    SharedMemoryHeader *header = nullptr;
    size_t slot_index = 0;
    uint64_t slot_id = 0;
    char *buffer_ptr = nullptr;
    size_t buffer_size = 0;
    size_t bytes_written = 0;
    bool committed = false;
    bool released = false;
    SlotRWState *rw_state = nullptr; // New: Pointer to the SlotRWState for this slot
};

struct SlotConsumeHandleImpl
{
    DataBlockConsumerImpl *owner = nullptr;
    DataBlock *dataBlock = nullptr;
    SharedMemoryHeader *header = nullptr;
    size_t slot_index = 0;
    uint64_t slot_id = 0;
    const char *buffer_ptr = nullptr;
    size_t buffer_size = 0;
    bool released = false;
    SlotRWState *rw_state = nullptr;  // New: Pointer to the SlotRWState for this slot
    uint64_t captured_generation = 0; // New: Captured generation for validation
    int consumer_heartbeat_slot = -1; // For Sync_reader: which consumer slot to update on release
};

namespace
{
bool release_write_handle(SlotWriteHandleImpl &impl);
bool release_consume_handle(SlotConsumeHandleImpl &impl);

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- slot_id/slot_index and captured_generation/consumer_heartbeat_slot are semantically distinct; internal helper with 2 call sites
std::unique_ptr<SlotConsumeHandleImpl> make_slot_consume_handle_impl(
    DataBlockConsumerImpl *owner,
    DataBlock *dataBlock,
    SharedMemoryHeader *header,
    uint64_t slot_id,     // NOLINT(bugprone-easily-swappable-parameters)
    size_t slot_index,
    const char *buf,
    size_t slot_stride_bytes,
    SlotRWState *rw_state,
    uint64_t captured_generation,  // NOLINT(bugprone-easily-swappable-parameters)
    int consumer_heartbeat_slot)
{
    auto impl = std::make_unique<SlotConsumeHandleImpl>();
    impl->owner = owner;
    impl->dataBlock = dataBlock;
    impl->header = header;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = slot_buffer_ptr(buf, slot_index, slot_stride_bytes);
    impl->buffer_size = slot_stride_bytes;
    impl->rw_state = rw_state;
    impl->captured_generation = captured_generation;
    impl->consumer_heartbeat_slot = consumer_heartbeat_slot;
    return impl;
}
} // namespace

// ============================================================================
// DataBlockProducerImpl
// ============================================================================
struct DataBlockProducerImpl
{
    std::mutex mutex; // Protects slot acquire/release and context; makes Producer thread-safe
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;
    std::vector<FlexibleZoneInfo> flexible_zones_info; // New member
    /// Display name (with optional suffix). Set once via call_once; not hot path.
    mutable std::once_flag name_fallback_once;
    mutable std::string name_fallback;
};

// ============================================================================
// DataBlockProducer
// ============================================================================
DataBlockProducer::DataBlockProducer() : pImpl(nullptr) {}

DataBlockProducer::DataBlockProducer(std::unique_ptr<DataBlockProducerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockProducer::~DataBlockProducer() noexcept = default;

DataBlockProducer::DataBlockProducer(DataBlockProducer &&other) noexcept = default;

DataBlockProducer &DataBlockProducer::operator=(DataBlockProducer &&other) noexcept = default;

// ============================================================================
// DataBlockConsumerImpl
// ============================================================================
struct DataBlockConsumerImpl
{
    std::recursive_mutex mutex; // Protects slot acquire/release, iterator, heartbeat; recursive (register_heartbeat called from acquire_consume_slot)
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;
    uint64_t last_consumed_slot_id = INVALID_SLOT_ID;  // This field is still needed
    std::vector<FlexibleZoneInfo> flexible_zones_info; // New member
    int heartbeat_slot = -1; // For Sync_reader: index into consumer_heartbeats / read positions
    /// Display name (with optional suffix). Set once via call_once; not hot path.
    mutable std::once_flag name_fallback_once;
    mutable std::string name_fallback;

    ~DataBlockConsumerImpl()
    {
        std::string label;
        if (!name_fallback.empty())
        {
            label = name_fallback;
        }
        else if (!name.empty())
        {
            label = name;
        }
        else
        {
            label = "(unnamed)";
        }
        LOGGER_INFO("DataBlockConsumerImpl: Shutting down for '{}'.", label);
    }
};

namespace
{
/// Returned by name() when pImpl is null (default-constructed or moved-from). Neutral state, not an error.
const std::string kNullProducerOrConsumerName("(null)");

/// Prefix of the runtime suffix appended to names for context. See docs/NAME_CONVENTIONS.md.
constexpr std::string_view kNameSuffixPrefix(" | pid:");
/// Single counter for both named (suffix) and unnamed (full id) so each instance has a unique index.
std::atomic<uint64_t> g_name_instance_counter{0};

/// Not hot path: called at most once per instance via call_once; result stored in name_fallback.
void ensure_producer_display_name(DataBlockProducerImpl *impl)
{
    if (impl == nullptr)
    {
        return;
    }
    uint64_t pid = pylabhub::platform::get_pid();
    uint64_t idx = g_name_instance_counter.fetch_add(1, std::memory_order_relaxed);
    if (impl->name.empty())
    {
        impl->name_fallback = fmt::format("producer-{}-{}", pid, idx);
    }
    else
    {
        impl->name_fallback = fmt::format("{}{}{}-{}", impl->name, kNameSuffixPrefix, pid, idx);
    }
}

/// Not hot path: called at most once per instance via call_once; result stored in name_fallback.
void ensure_consumer_display_name(DataBlockConsumerImpl *impl)
{
    if (impl == nullptr)
    {
        return;
    }
    uint64_t pid = pylabhub::platform::get_pid();
    uint64_t idx = g_name_instance_counter.fetch_add(1, std::memory_order_relaxed);
    if (impl->name.empty())
    {
        impl->name_fallback = fmt::format("consumer-{}-{}", pid, idx);
    }
    else
    {
        impl->name_fallback = fmt::format("{}{}{}-{}", impl->name, kNameSuffixPrefix, pid, idx);
    }
}
}

const std::string &DataBlockProducer::name() const noexcept
{
    if (pImpl == nullptr)
    {
        return kNullProducerOrConsumerName;
    }
    std::call_once(pImpl->name_fallback_once, [this] { ensure_producer_display_name(pImpl.get()); });
    return pImpl->name_fallback;
}

const std::string &DataBlockConsumer::name() const noexcept
{
    if (pImpl == nullptr)
    {
        return kNullProducerOrConsumerName;
    }
    std::call_once(pImpl->name_fallback_once, [this] { ensure_consumer_display_name(pImpl.get()); });
    return pImpl->name_fallback;
}

// ============================================================================
// DataBlockDiagnosticHandle implementation
// ============================================================================
DataBlockDiagnosticHandle::DataBlockDiagnosticHandle(
    std::unique_ptr<DataBlockDiagnosticHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockDiagnosticHandle::~DataBlockDiagnosticHandle() noexcept
{
    if (pImpl != nullptr)
    {
        pylabhub::platform::shm_close(&pImpl->m_shm);
    }
}

DataBlockDiagnosticHandle::DataBlockDiagnosticHandle(DataBlockDiagnosticHandle &&) noexcept =
    default;

DataBlockDiagnosticHandle &
DataBlockDiagnosticHandle::operator=(DataBlockDiagnosticHandle &&) noexcept = default;

SharedMemoryHeader *DataBlockDiagnosticHandle::header() const
{
    return pImpl ? pImpl->header_ptr : nullptr;
}

SlotRWState *DataBlockDiagnosticHandle::slot_rw_state(uint32_t index) const
{
    if (pImpl == nullptr || pImpl->slot_rw_states == nullptr || index >= pImpl->ring_buffer_capacity)
    {
        return nullptr;
    }
    return &pImpl->slot_rw_states[index];
}

std::unique_ptr<DataBlockDiagnosticHandle> open_datablock_for_diagnostic(const std::string &name)
{
    auto impl = std::make_unique<DataBlockDiagnosticHandleImpl>();
    try
    {
        impl->m_shm = pylabhub::platform::shm_attach(name.c_str());
        if (impl->m_shm.base == nullptr)
        {
            return nullptr;
        }
        impl->header_ptr = reinterpret_cast<SharedMemoryHeader *>(impl->m_shm.base);
        if (!detail::is_header_magic_valid(&impl->header_ptr->magic_number,
                                            detail::DATABLOCK_MAGIC_NUMBER))
        {
            return nullptr;
        }
        DataBlockLayout layout = DataBlockLayout::from_header(impl->header_ptr);
        impl->ring_buffer_capacity = layout.slot_count_value();
        impl->slot_rw_states = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(impl->m_shm.base) + layout.slot_rw_state_offset);
        return std::unique_ptr<DataBlockDiagnosticHandle>(
            new DataBlockDiagnosticHandle(std::move(impl)));
    }
    catch (...)
    {
        return nullptr;
    }
}

uint64_t DataBlockProducer::last_slot_id() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return 0;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    if (header == nullptr)
    {
        return 0;
    }
    return header->commit_index.load(std::memory_order_acquire);
}

int DataBlockProducer::get_metrics(DataBlockMetrics &out_metrics) const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_get_metrics(header, &out_metrics) : -1;
}

int DataBlockProducer::reset_metrics() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_reset_metrics(header) : -1;
}

bool DataBlockProducer::update_checksum_flexible_zone(size_t flexible_zone_idx) noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? update_checksum_flexible_zone_impl(pImpl->dataBlock.get(), flexible_zone_idx)
               : false;
}

SharedSpinLock DataBlockProducer::get_spinlock(size_t index)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        throw std::runtime_error("DataBlockProducer::get_spinlock: producer is invalid.");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(index);
    return {state, fmt::format("{}:spinlock:{}", name(), index)};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency
uint32_t DataBlockProducer::spinlock_count() const noexcept
{
    return static_cast<uint32_t>(detail::MAX_SHARED_SPINLOCKS);
}

std::span<std::byte> DataBlockProducer::flexible_zone_span(size_t index) noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr || index >= pImpl->flexible_zones_info.size())
    {
        return {};
    }
    const auto &zone_info = pImpl->flexible_zones_info[index];
    char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();
    if (flexible_zone_base == nullptr || zone_info.size == 0)
    {
        return {};
    }
    return {reinterpret_cast<std::byte *>(flexible_zone_base + zone_info.offset), zone_info.size};
}

bool DataBlockProducer::update_checksum_slot(size_t slot_index) noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? update_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
               : false;
}

// NOLINTNEXTLINE(bugprone-exception-escape) -- slot_rw_state may throw; callers expect noexcept
std::unique_ptr<SlotWriteHandle> DataBlockProducer::acquire_write_slot(int timeout_ms) noexcept
{
    if (pImpl == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto [header, slot_count] = get_header_and_slot_count(pImpl->dataBlock.get());
    if (header == nullptr || slot_count == 0)
    {
        return nullptr;
    }

    const ConsumerSyncPolicy policy = header->consumer_sync_policy;
    if (policy == ConsumerSyncPolicy::Single_reader || policy == ConsumerSyncPolicy::Sync_reader)
    {
        auto start_time = pylabhub::platform::monotonic_time_ns();
        int iteration = 0;
        while (true)
        {
            uint64_t write_idx = header->write_index.load(std::memory_order_acquire);
            uint64_t read_idx = header->read_index.load(std::memory_order_acquire);
            if (write_idx - read_idx < static_cast<uint64_t>(slot_count))
            {
                break;
            }
            if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
            {
                header->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            backoff(iteration++);
        }
    }

    // Acquire a new slot ID (monotonically increasing)
    uint64_t slot_id = header->write_index.fetch_add(1, std::memory_order_acq_rel);
    auto slot_index = static_cast<size_t>(slot_id % slot_count);

    // Get the SlotRWState for this slot
    SlotRWState *rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
    if (rw_state == nullptr)
    {
        return nullptr; // Should not happen, slot_rw_state throws on error
    }

    // Acquire write lock for this slot
    SlotAcquireResult acquire_res = acquire_write(rw_state, header, timeout_ms);
    if (acquire_res != SLOT_ACQUIRE_OK)
    {
        // Error already logged in acquire_write. Just return nullptr.
        return nullptr;
    }

    // Ring-buffer iteration step is logical slot size (slot_stride_bytes), not physical.
    const size_t slot_stride_bytes = pImpl->dataBlock->layout().slot_stride_bytes();
    char *buf = pImpl->dataBlock->structured_data_buffer();
    if (buf == nullptr || slot_stride_bytes == 0)
    {
        // Release write lock if buffer is invalid before returning
        rw_state->write_lock.store(0, std::memory_order_release);
        return nullptr;
    }

    auto impl = std::make_unique<SlotWriteHandleImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->header = header;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = slot_buffer_ptr(buf, slot_index, slot_stride_bytes);
    impl->buffer_size = slot_stride_bytes;
    impl->rw_state = rw_state;
    // flexible_ptr and flexible_size are no longer directly used in SlotWriteHandleImpl
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) -- OOM would terminate; noexcept design choice
    return std::unique_ptr<SlotWriteHandle>(new SlotWriteHandle(std::move(impl)));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency with class
bool DataBlockProducer::release_write_slot(SlotWriteHandle &handle) noexcept
{
    if (handle.pImpl == nullptr)
    {
        return false;
    }
    DataBlockProducerImpl *owner = handle.pImpl->owner;
    if (owner == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(owner->mutex);
    return release_write_handle(*handle.pImpl);
}

void DataBlockProducer::update_heartbeat() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    if (header != nullptr)
    {
        detail::update_producer_heartbeat_impl(header, pylabhub::platform::get_pid());
    }
}

void DataBlockProducer::check_consumer_health() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr || pImpl->dataBlock->header() == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();

    for (size_t i = 0; i < detail::MAX_CONSUMER_HEARTBEATS; ++i)
    {
        uint64_t consumer_pid =
            header->consumer_heartbeats[i].consumer_id.load(std::memory_order_acquire);
        if (consumer_pid != 0)
        {
            if (!pylabhub::platform::is_process_alive(consumer_pid))
            {
                LOGGER_WARN(
                    "DataBlock '{}': Detected dead consumer PID {}. Clearing heartbeat slot {}.",
                    pImpl->name, consumer_pid, i);
                uint64_t expected_pid = consumer_pid;
                if (header->consumer_heartbeats[i].consumer_id.compare_exchange_strong(
                        expected_pid, 0, std::memory_order_acq_rel))
                {
                    header->active_consumer_count.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        }
    }
}

bool DataBlockProducer::register_with_broker(MessageHub &hub, const std::string &channel_name)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr ||
        pImpl->dataBlock->header() == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    ProducerInfo info{};
    info.shm_name = pImpl->name;
    info.producer_pid = pylabhub::platform::get_pid();
    info.schema_hash.assign(
        reinterpret_cast<const char *>(pImpl->dataBlock->header()->schema_hash),
        detail::CHECKSUM_BYTES);
    info.schema_version = pImpl->dataBlock->header()->schema_version;
    return hub.register_producer(channel_name, info);
}

// ============================================================================
// DataBlockSlotIterator (ring-buffer)
// ============================================================================
struct DataBlockSlotIteratorImpl
{
    DataBlockConsumerImpl *owner = nullptr;
    DataBlock *dataBlock = nullptr;
    uint64_t last_seen_slot_id = INVALID_SLOT_ID;
};

DataBlockSlotIterator::DataBlockSlotIterator() : pImpl(nullptr) {}

DataBlockSlotIterator::DataBlockSlotIterator(std::unique_ptr<DataBlockSlotIteratorImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockSlotIterator::~DataBlockSlotIterator() noexcept = default;

DataBlockSlotIterator::DataBlockSlotIterator(DataBlockSlotIterator &&other) noexcept = default;

DataBlockSlotIterator &
DataBlockSlotIterator::operator=(DataBlockSlotIterator &&other) noexcept = default;

// NOLINTNEXTLINE(bugprone-exception-escape) -- slot_rw_state may throw; callers expect noexcept
DataBlockSlotIterator::NextResult DataBlockSlotIterator::try_next(int timeout_ms) noexcept
{
    NextResult result{};
    result.ok = false;
    result.error_code = 0;
    if (pImpl == nullptr)
    {
        result.error_code = 1;
        return result;
    }
    DataBlockConsumerImpl *owner = pImpl->owner;
    if (owner == nullptr)
    {
        result.error_code = 1;
        return result;
    }
    std::lock_guard<std::recursive_mutex> lock(owner->mutex);
    auto [header, slot_count] = get_header_and_slot_count(pImpl->dataBlock);
    if (header == nullptr || slot_count == 0)
    {
        result.error_code = 1;
        return result;
    }

    const ConsumerSyncPolicy policy = header->consumer_sync_policy;
    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;
    uint64_t slot_id = INVALID_SLOT_ID;
    size_t slot_index = 0;
    SlotRWState *rw_state = nullptr;
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res{};

    const int heartbeat_slot =
        (pImpl->owner != nullptr) ? pImpl->owner->heartbeat_slot : -1;

    while (true)
    {
        const uint64_t next_to_read =
            get_next_slot_to_read(header, pImpl->last_seen_slot_id, heartbeat_slot);

        if (next_to_read != INVALID_SLOT_ID)
        {
            slot_index = static_cast<size_t>(next_to_read % slot_count);
            rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
            if (rw_state == nullptr)
            {
                result.error_code = 3;
                return result;
            }
            acquire_res = acquire_read(rw_state, header, &captured_generation);
            if (acquire_res == SLOT_ACQUIRE_OK)
            {
                slot_id = next_to_read;
                break;
            }
            if (acquire_res != SLOT_ACQUIRE_NOT_READY)
            {
                result.error_code = 3;
                return result;
            }
        }

        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            header->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            result.error_code = 2;
            return result;
        }
        backoff(iteration++);
    }

    const size_t slot_stride_bytes = pImpl->dataBlock->layout().slot_stride_bytes();
    auto impl = make_slot_consume_handle_impl(
        pImpl->owner, pImpl->dataBlock, header, slot_id, slot_index,
        pImpl->dataBlock->structured_data_buffer(), slot_stride_bytes,
        rw_state, captured_generation,
        (policy == ConsumerSyncPolicy::Sync_reader && pImpl->owner != nullptr)
            ? pImpl->owner->heartbeat_slot
            : -1);

    result.next = SlotConsumeHandle(std::move(impl));
    result.ok = true;
    pImpl->last_seen_slot_id = slot_id;
    return result;
}

SlotConsumeHandle DataBlockSlotIterator::next(int timeout_ms)
{
    auto res = try_next(timeout_ms);
    if (!res.ok)
    {
        throw std::runtime_error(
            fmt::format("DataBlockSlotIterator::next: slot not available (error {})", res.error_code));
    }
    return std::move(res.next);
}

void DataBlockSlotIterator::seek_latest() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr || pImpl->owner == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->owner->mutex);
    auto *header = pImpl->dataBlock->header();
    if (header == nullptr)
    {
        return;
    }
    pImpl->last_seen_slot_id = header->commit_index.load(std::memory_order_acquire);
}

void DataBlockSlotIterator::seek_to(uint64_t slot_id) noexcept
{
    if (pImpl == nullptr || pImpl->owner == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->owner->mutex);
    pImpl->last_seen_slot_id = slot_id;
}

uint64_t DataBlockSlotIterator::last_slot_id() const noexcept
{
    return pImpl ? pImpl->last_seen_slot_id : INVALID_SLOT_ID;
}

bool DataBlockSlotIterator::is_valid() const noexcept
{
    return pImpl != nullptr && pImpl->dataBlock != nullptr;
}

// ============================================================================
// Slot Handles (Primitive Data Transfer API) - Implementations
// ============================================================================
// Lifetime contract: SlotWriteHandle and SlotConsumeHandle hold pointers into the
// DataBlock's shared memory. Callers must release or destroy all handles before
// destroying the DataBlockProducer or DataBlockConsumer. Otherwise the handle
// destructor will access freed memory (use-after-free).
namespace
{
bool release_write_handle(SlotWriteHandleImpl &impl)
{
    if (impl.released)
    {
        return true;
    }
    bool success = true;

    // Perform checksum updates if policy requires and committed. On checksum failure the slot is already committed (visible to readers); we log and return false.
    if (impl.committed && impl.owner != nullptr && impl.owner->checksum_policy != ChecksumPolicy::None &&
        impl.header != nullptr &&
        static_cast<ChecksumType>(impl.header->checksum_type) != ChecksumType::Unset)
    {
        if (!update_checksum_slot_impl(impl.dataBlock, impl.slot_index))
        {
            LOGGER_WARN("DataBlock '{}': release_write_slot failed — checksum update failed for slot_index={} slot_id={}.",
                        impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index, impl.slot_id);
            success = false;
        }

        // Update flexible zone checksums
        for (size_t i = 0; i < impl.dataBlock->flexible_zone_info().size(); ++i)
        {
            if (!update_checksum_flexible_zone_impl(impl.dataBlock, i))
            {
                LOGGER_WARN("DataBlock '{}': release_write_slot failed — flexible zone checksum update failed for zone_index={} slot_index={}.",
                            impl.owner != nullptr ? impl.owner->name : "(unknown)", i, impl.slot_index);
                success = false;
                break;
            }
        }
    }

    // Commit the write (make it visible to readers)
    if (impl.committed && impl.rw_state != nullptr && impl.header != nullptr)
    {
        commit_write(impl.rw_state, impl.header);
        detail::update_producer_heartbeat_impl(impl.header, pylabhub::platform::get_pid());
        // Release write_lock so the slot can be reused on wrap-around (lap2+)
        impl.rw_state->write_lock.store(0, std::memory_order_release);
    }
    else if (impl.rw_state != nullptr)
    {
        // If not committed, simply release the write lock
        impl.rw_state->write_lock.store(0, std::memory_order_release);
        impl.rw_state->slot_state.store(SlotRWState::SlotState::FREE, std::memory_order_release);
    }

    // No thread guard exit needed here, as acquire_write doesn't use it.
    impl.released = true;
    return success;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- structured release flow; refactor would fragment logic
bool release_consume_handle(SlotConsumeHandleImpl &impl)
{
    if (impl.released)
    {
        return true;
    }
    bool success = true;

    // 1. Validate captured generation to detect wrap-around (if reader was pre-empted)
    if (impl.rw_state != nullptr && impl.header != nullptr)
    {
        if (!validate_read_impl(impl.rw_state, impl.header, impl.captured_generation))
        {
            LOGGER_WARN("DataBlock '{}': release_consume_slot failed — slot validation failed (wrap-around or slot overwritten) for slot_index={} slot_id={}.",
                        impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index, impl.slot_id);
            success = false;
        }
    }
    else
    {
        success = false; // Invalid state
    }

    // 2. Perform checksum verification if policy requires
    if (success && impl.owner != nullptr && impl.owner->checksum_policy != ChecksumPolicy::None &&
        impl.header != nullptr &&
        static_cast<ChecksumType>(impl.header->checksum_type) != ChecksumType::Unset)
    {
        if (!verify_checksum_slot_impl(impl.dataBlock, impl.slot_index))
        {
            LOGGER_WARN("DataBlock '{}': release_consume_slot failed — slot checksum verification failed for slot_index={} slot_id={}.",
                        impl.owner != nullptr ? impl.owner->name : "(unknown)", impl.slot_index, impl.slot_id);
            success = false;
        }
        for (size_t i = 0; i < impl.dataBlock->flexible_zone_info().size(); ++i)
        {
            if (!verify_checksum_flexible_zone_impl(impl.dataBlock, i))
            {
                LOGGER_WARN("DataBlock '{}': release_consume_slot failed — flexible zone checksum verification failed for zone_index={} slot_index={}.",
                            impl.owner != nullptr ? impl.owner->name : "(unknown)", i, impl.slot_index);
                success = false;
                break;
            }
        }
    }

    // 3. Release the read lock
    if (impl.rw_state != nullptr && impl.header != nullptr)
    {
        release_read(impl.rw_state, impl.header);
    }
    else
    {
        success = false; // Cannot release if state is invalid
    }

    // 4. Advance read position for Single_reader / Sync_reader
    if (success && impl.header != nullptr)
    {
        SharedMemoryHeader *header = impl.header;
        const ConsumerSyncPolicy policy = header->consumer_sync_policy;
        const uint64_t next = impl.slot_id + 1;
        if (policy == ConsumerSyncPolicy::Single_reader)
        {
            header->read_index.store(next, std::memory_order_release);
        }
        else if (policy == ConsumerSyncPolicy::Sync_reader && impl.consumer_heartbeat_slot >= 0 &&
                 impl.consumer_heartbeat_slot < static_cast<int>(MAX_CONSUMER_HEARTBEATS))
        {
            consumer_next_read_slot_ptr(header, static_cast<size_t>(impl.consumer_heartbeat_slot))
                ->store(next, std::memory_order_release);
            // read_index = min of all consumer positions (only count registered slots)
            uint64_t min_pos = next;
            for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i)
            {
                if (header->consumer_heartbeats[i].consumer_id.load(std::memory_order_acquire) != 0)
                {
                    uint64_t pos =
                        consumer_next_read_slot_ptr(header, i)->load(std::memory_order_acquire);
                    if (pos < min_pos)
                    {
                        min_pos = pos;
                    }
                }
            }
            header->read_index.store(min_pos, std::memory_order_release);
        }
    }

    impl.released = true;
    return success;
}
} // namespace

SlotWriteHandle::SlotWriteHandle() : pImpl(nullptr) {}

SlotWriteHandle::SlotWriteHandle(std::unique_ptr<SlotWriteHandleImpl> impl) : pImpl(std::move(impl))
{
}

SlotWriteHandle::~SlotWriteHandle() noexcept
{
    if (pImpl)
    {
        (void)release_write_handle(*pImpl);
    }
}

SlotWriteHandle::SlotWriteHandle(SlotWriteHandle &&other) noexcept = default;

SlotWriteHandle &SlotWriteHandle::operator=(SlotWriteHandle &&other) noexcept = default;

size_t SlotWriteHandle::slot_index() const noexcept
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotWriteHandle::slot_id() const noexcept
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<std::byte> SlotWriteHandle::buffer_span() noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || pImpl->buffer_size == 0)
    {
        return {};
    }
    return {reinterpret_cast<std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<std::byte> SlotWriteHandle::flexible_zone_span(size_t flexible_zone_idx) noexcept
{
    if (pImpl == nullptr || pImpl->owner == nullptr || pImpl->dataBlock == nullptr ||
        flexible_zone_idx >= pImpl->owner->flexible_zones_info.size())
    {
        return {};
    }

    const auto &zone_info = pImpl->owner->flexible_zones_info[flexible_zone_idx];
    char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();

    if (flexible_zone_base == nullptr || zone_info.size == 0)
    {
        return {};
    }

    return {reinterpret_cast<std::byte *>(flexible_zone_base + zone_info.offset), zone_info.size};
}

bool SlotWriteHandle::write(const void *src, size_t len, size_t offset) noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || len == 0)
    {
        return false;
    }
    if (offset + len > pImpl->buffer_size)
    {
        return false;
    }
    std::memcpy(pImpl->buffer_ptr + offset, src, len);
    return true;
}

bool SlotWriteHandle::commit(size_t bytes_written) noexcept
{
    if (pImpl == nullptr || pImpl->header == nullptr)
    {
        return false;
    }
    if (bytes_written > pImpl->buffer_size)
    {
        return false;
    }
    pImpl->bytes_written = bytes_written;
    pImpl->committed = true;

    return true;
}

bool SlotWriteHandle::update_checksum_slot() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return update_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotWriteHandle::update_checksum_flexible_zone(size_t flexible_zone_idx) noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return update_checksum_flexible_zone_impl(pImpl->dataBlock, flexible_zone_idx);
}

SlotConsumeHandle::SlotConsumeHandle() : pImpl(nullptr) {}

SlotConsumeHandle::SlotConsumeHandle(std::unique_ptr<SlotConsumeHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

SlotConsumeHandle::~SlotConsumeHandle() noexcept
{
    if (pImpl)
    {
        (void)release_consume_handle(*pImpl);
    }
}

SlotConsumeHandle::SlotConsumeHandle(SlotConsumeHandle &&other) noexcept = default;

SlotConsumeHandle &SlotConsumeHandle::operator=(SlotConsumeHandle &&other) noexcept = default;

size_t SlotConsumeHandle::slot_index() const noexcept
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotConsumeHandle::slot_id() const noexcept
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<const std::byte> SlotConsumeHandle::buffer_span() const noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || pImpl->buffer_size == 0)
    {
        return {};
    }
    return {reinterpret_cast<const std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<const std::byte> SlotConsumeHandle::flexible_zone_span(size_t flexible_zone_idx) const noexcept
{
    if (pImpl == nullptr || pImpl->owner == nullptr || pImpl->dataBlock == nullptr ||
        flexible_zone_idx >= pImpl->owner->flexible_zones_info.size())
    {
        return {};
    }

    const auto &zone_info = pImpl->owner->flexible_zones_info[flexible_zone_idx];
    const char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();

    if (flexible_zone_base == nullptr || zone_info.size == 0)
    {
        return {};
    }

    return {reinterpret_cast<const std::byte *>(flexible_zone_base + zone_info.offset),
            zone_info.size};
}

bool SlotConsumeHandle::read(void *dst, size_t len, size_t offset) const noexcept
{
    if (pImpl == nullptr || pImpl->buffer_ptr == nullptr || len == 0)
    {
        return false;
    }
    if (offset + len > pImpl->buffer_size)
    {
        return false;
    }
    std::memcpy(dst, pImpl->buffer_ptr + offset, len);
    return true;
}

bool SlotConsumeHandle::verify_checksum_slot() const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return verify_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotConsumeHandle::verify_checksum_flexible_zone(size_t flexible_zone_idx) const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return false;
    }
    return verify_checksum_flexible_zone_impl(pImpl->dataBlock, flexible_zone_idx);
}

bool SlotConsumeHandle::validate_read() const noexcept
{
    if (pImpl == nullptr || pImpl->rw_state == nullptr)
    {
        return false;
    }
    return validate_read_impl(pImpl->rw_state, pImpl->header, pImpl->captured_generation);
}

// ============================================================================
// DataBlockConsumer
// ============================================================================
DataBlockConsumer::DataBlockConsumer() : pImpl(nullptr) {}

DataBlockConsumer::DataBlockConsumer(std::unique_ptr<DataBlockConsumerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockConsumer::~DataBlockConsumer() noexcept = default;

DataBlockConsumer::DataBlockConsumer(DataBlockConsumer &&other) noexcept = default;

DataBlockConsumer &DataBlockConsumer::operator=(DataBlockConsumer &&other) noexcept = default;

SharedSpinLock DataBlockConsumer::get_spinlock(size_t index)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        throw std::runtime_error("DataBlockConsumer::get_spinlock: consumer is invalid.");
    }
    SharedSpinLockState *state = pImpl->dataBlock->get_shared_spinlock_state(index);
    return {state, fmt::format("{}:spinlock:{}", name(), index)};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency
uint32_t DataBlockConsumer::spinlock_count() const noexcept
{
    return static_cast<uint32_t>(detail::MAX_SHARED_SPINLOCKS);
}

bool DataBlockConsumer::verify_checksum_flexible_zone(size_t flexible_zone_idx) const noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? verify_checksum_flexible_zone_impl(pImpl->dataBlock.get(), flexible_zone_idx)
               : false;
}

std::span<const std::byte> DataBlockConsumer::flexible_zone_span(size_t index) const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr || index >= pImpl->flexible_zones_info.size())
    {
        return {};
    }
    const auto &zone_info = pImpl->flexible_zones_info[index];
    const char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();
    if (flexible_zone_base == nullptr || zone_info.size == 0)
    {
        return {};
    }
    return {reinterpret_cast<const std::byte *>(flexible_zone_base + zone_info.offset),
            zone_info.size};
}

bool DataBlockConsumer::verify_checksum_slot(size_t slot_index) const noexcept
{
    return (pImpl != nullptr && pImpl->dataBlock != nullptr)
               ? verify_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
               : false;
}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity) -- slot_rw_state may throw; structured acquire flow
std::unique_ptr<SlotConsumeHandle> DataBlockConsumer::acquire_consume_slot(int timeout_ms) noexcept
{
    if (pImpl == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto [header, slot_count] = get_header_and_slot_count(pImpl->dataBlock.get());
    if (header == nullptr || slot_count == 0)
    {
        return nullptr;
    }

    const ConsumerSyncPolicy policy = header->consumer_sync_policy;
    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;
    uint64_t slot_id = INVALID_SLOT_ID;
    size_t slot_index = 0;
    SlotRWState *rw_state = nullptr;
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res{};

    // Sync_reader: ensure registered and join-at-latest for new consumer
    if (policy == ConsumerSyncPolicy::Sync_reader)
    {
        if (pImpl->heartbeat_slot < 0)
        {
            if (register_heartbeat() < 0)
            {
                return nullptr;
            }
            // Join at latest: start reading from current commit_index
            uint64_t join_at = header->commit_index.load(std::memory_order_acquire);
            if (join_at != INVALID_SLOT_ID)
            {
                consumer_next_read_slot_ptr(header, static_cast<size_t>(pImpl->heartbeat_slot))
                    ->store(join_at, std::memory_order_release);
            }
            else
            {
                consumer_next_read_slot_ptr(header, static_cast<size_t>(pImpl->heartbeat_slot))
                    ->store(0, std::memory_order_release);
            }
        }
    }

    while (true)
    {
        const uint64_t next_to_read =
            get_next_slot_to_read(header, pImpl->last_consumed_slot_id, pImpl->heartbeat_slot);

        if (next_to_read != INVALID_SLOT_ID)
        {
            slot_index = static_cast<size_t>(next_to_read % slot_count);
            rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
            if (rw_state == nullptr)
            {
                return nullptr;
            }
            acquire_res = acquire_read(rw_state, header, &captured_generation);
            if (acquire_res == SLOT_ACQUIRE_OK)
            {
                slot_id = next_to_read;
                break;
            }
            if (acquire_res != SLOT_ACQUIRE_NOT_READY)
            {
                return nullptr;
            }
        }

        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            header->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        backoff(iteration++);
    }

    const size_t slot_stride_bytes = pImpl->dataBlock->layout().slot_stride_bytes(); // ring iteration step = logical
    const char *buf = pImpl->dataBlock->structured_data_buffer();
    if (buf == nullptr || slot_stride_bytes == 0)
    {
        release_read(rw_state, header);
        return nullptr;
    }

    auto handle_impl = make_slot_consume_handle_impl(
        pImpl.get(), pImpl->dataBlock.get(), header, slot_id, slot_index,
        buf, slot_stride_bytes, rw_state, captured_generation,
        (policy == ConsumerSyncPolicy::Sync_reader) ? pImpl->heartbeat_slot : -1);

    pImpl->last_consumed_slot_id = slot_id;
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) -- OOM would terminate; noexcept design choice
    return std::unique_ptr<SlotConsumeHandle>(new SlotConsumeHandle(std::move(handle_impl)));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters,bugprone-exception-escape) -- slot_id then timeout_ms; slot_rw_state may throw
std::unique_ptr<SlotConsumeHandle> DataBlockConsumer::acquire_consume_slot(uint64_t slot_id,
                                                                           int timeout_ms) noexcept
{
    if (pImpl == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto [header, slot_count] = get_header_and_slot_count(pImpl->dataBlock.get());
    if (header == nullptr || slot_count == 0)
    {
        return nullptr;
    }

    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;

    // Wait until this slot_id is committed
    while (header->commit_index.load(std::memory_order_acquire) < slot_id)
    {
        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            header->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        backoff(iteration++);
    }

    auto slot_index = static_cast<size_t>(slot_id % slot_count);
    SlotRWState *rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res = acquire_read(rw_state, header, &captured_generation);
    if (acquire_res != SLOT_ACQUIRE_OK)
    {
        return nullptr;
    }

    const size_t slot_stride_bytes = pImpl->dataBlock->layout().slot_stride_bytes(); // ring iteration step = logical
    const char *buf = pImpl->dataBlock->structured_data_buffer();
    if (buf == nullptr || slot_stride_bytes == 0)
    {
        release_read(rw_state, header);
        return nullptr;
    }

    auto handle_impl = make_slot_consume_handle_impl(
        pImpl.get(), pImpl->dataBlock.get(), header, slot_id, slot_index,
        buf, slot_stride_bytes, rw_state, captured_generation,
        (header->consumer_sync_policy == ConsumerSyncPolicy::Sync_reader) ? pImpl->heartbeat_slot : -1);

    pImpl->last_consumed_slot_id = slot_id;
    // NOLINTNEXTLINE(bugprone-unhandled-exception-at-new) -- OOM would terminate; noexcept design choice
    return std::unique_ptr<SlotConsumeHandle>(new SlotConsumeHandle(std::move(handle_impl)));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- member for API consistency with class
bool DataBlockConsumer::release_consume_slot(SlotConsumeHandle &handle) noexcept
{
    if (handle.pImpl == nullptr)
    {
        return false;
    }
    DataBlockConsumerImpl *owner = handle.pImpl->owner;
    if (owner == nullptr)
    {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(owner->mutex);
    return release_consume_handle(*handle.pImpl);
}

DataBlockSlotIterator DataBlockConsumer::slot_iterator()
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto impl = std::make_unique<DataBlockSlotIteratorImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->last_seen_slot_id = INVALID_SLOT_ID;
    return DataBlockSlotIterator(std::move(impl));
}

int DataBlockConsumer::register_heartbeat()
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr || pImpl->dataBlock->header() == nullptr)
    {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();
    uint64_t pid = pylabhub::platform::get_pid();

    for (size_t i = 0; i < detail::MAX_CONSUMER_HEARTBEATS; ++i)
    {
        uint64_t expected = 0;
        if (header->consumer_heartbeats[i].consumer_id.compare_exchange_strong(
                expected, pid, std::memory_order_acq_rel))
        {
            header->active_consumer_count.fetch_add(1, std::memory_order_relaxed);
            header->consumer_heartbeats[i].last_heartbeat_ns.store(
                pylabhub::platform::monotonic_time_ns(), std::memory_order_release);
            pImpl->heartbeat_slot = static_cast<int>(i);
            return static_cast<int>(i);
        }
    }
    return -1; // No available slot
}

void DataBlockConsumer::update_heartbeat(int slot)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr ||
        pImpl->dataBlock->header() == nullptr || slot < 0 ||
        slot >= static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();
    header->consumer_heartbeats[slot].last_heartbeat_ns.store(
        pylabhub::platform::monotonic_time_ns(), std::memory_order_release);
}

void DataBlockConsumer::unregister_heartbeat(int slot)
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr ||
        pImpl->dataBlock->header() == nullptr || slot < 0 ||
        slot >= static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(pImpl->mutex);
    auto *header = pImpl->dataBlock->header();
    uint64_t pid = pylabhub::platform::get_pid();
    uint64_t expected = pid; // Expected to be the current process's PID
    if (header->consumer_heartbeats[slot].consumer_id.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel))
    {
        header->active_consumer_count.fetch_sub(1, std::memory_order_relaxed);
        pImpl->heartbeat_slot = -1;
    }
}

int DataBlockConsumer::get_metrics(DataBlockMetrics &out_metrics) const noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_get_metrics(header, &out_metrics) : -1;
}

int DataBlockConsumer::reset_metrics() noexcept
{
    if (pImpl == nullptr || pImpl->dataBlock == nullptr)
    {
        return -1;
    }
    SharedMemoryHeader *header = pImpl->dataBlock->header();
    return (header != nullptr) ? slot_rw_reset_metrics(header) : -1;
}

// ============================================================================
// SharedMemoryHeader schema (layout + protocol check)
// ============================================================================
//
// Canonical rule: the schema field list lives next to SharedMemoryHeader in
// data_block.hpp (PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS). Update that
// list and the struct together so names, order, and types stay in one place.
// This .cpp only expands the list and adds the four fields with dynamic type_id.
// ============================================================================

pylabhub::schema::SchemaInfo get_shared_memory_header_schema_info()
{
    using pylabhub::schema::BLDSBuilder;
    using pylabhub::schema::SchemaInfo;
    using pylabhub::schema::SchemaVersion;

    BLDSBuilder builder{};
    // Header defines the list: PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS(OP) expands to
    //   OP(magic_number,"u32") OP(version_major,"u16") ... for every header field.
    // We define OP here: for each (member, type_id) call add_member(name, type_id, offset, size).
    // So the single line below expands to many b.add_member(...) calls in struct order.
#define PYLABHUB_ADD_SCHEMA_FIELD(member, type_id)                                            \
    builder.add_member(#member, type_id, offsetof(SharedMemoryHeader, member),               \
                      sizeof(SharedMemoryHeader::member));
    PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS(PYLABHUB_ADD_SCHEMA_FIELD);
#undef PYLABHUB_ADD_SCHEMA_FIELD
    // Trailing fields: type_id depends on constants (must stay in same order as struct)
    builder.add_member("consumer_heartbeats",
                 fmt::format("ConsumerHeartbeat[{}]", detail::MAX_CONSUMER_HEARTBEATS),
                 offsetof(SharedMemoryHeader, consumer_heartbeats),
                 sizeof(SharedMemoryHeader::consumer_heartbeats));
    builder.add_member("spinlock_states",
                 fmt::format("SharedSpinLockState[{}]", detail::MAX_SHARED_SPINLOCKS),
                 offsetof(SharedMemoryHeader, spinlock_states),
                 sizeof(SharedMemoryHeader::spinlock_states));
    builder.add_member("flexible_zone_checksums",
                 fmt::format("FlexibleZoneChecksumEntry[{}]", detail::MAX_FLEXIBLE_ZONE_CHECKSUMS),
                 offsetof(SharedMemoryHeader, flexible_zone_checksums),
                 sizeof(SharedMemoryHeader::flexible_zone_checksums));
    builder.add_member("reserved_header",
                 fmt::format("u8[{}]", sizeof(SharedMemoryHeader::reserved_header)),
                 offsetof(SharedMemoryHeader, reserved_header),
                 sizeof(SharedMemoryHeader::reserved_header));

    SchemaInfo info{};
    info.name = "pylabhub.hub.SharedMemoryHeader";
    info.version = SchemaVersion{detail::HEADER_VERSION_MAJOR, detail::HEADER_VERSION_MINOR, 0};
    info.struct_size = sizeof(SharedMemoryHeader);
    info.blds = builder.build();
    info.compute_hash();
    return info;
}

void validate_header_layout_hash(const SharedMemoryHeader *header)
{
    if (header == nullptr)
    {
        throw std::invalid_argument("validate_header_layout_hash: header is null");
    }
    pylabhub::schema::SchemaInfo expected = get_shared_memory_header_schema_info();
    const uint8_t *stored = header->reserved_header + detail::HEADER_LAYOUT_HASH_OFFSET;
    if (std::memcmp(expected.hash.data(), stored, detail::HEADER_LAYOUT_HASH_SIZE) != 0)
    {
        std::array<uint8_t, detail::CHECKSUM_BYTES> actual_hash;
        std::memcpy(actual_hash.data(), stored, detail::HEADER_LAYOUT_HASH_SIZE);
        throw pylabhub::schema::SchemaValidationException(
            "SharedMemoryHeader layout mismatch: producer and consumer have different ABI "
            "(offset/size).",
            expected.hash, actual_hash);
    }
}

// ============================================================================
// Layout checksum (segment layout-defining values; see DATAHUB_CPP_ABSTRACTION_DESIGN §4.8)
// ============================================================================
namespace
{
/** Layout checksum input: fixed order so producer and consumer hash the same bytes.
 *  Order: ring_buffer_capacity(4), physical_page_size(4), logical_unit_size(4),
 *  flexible_zone_size(8), checksum_type(1), policy(1), consumer_sync_policy(1), reserved(1). */
constexpr size_t LAYOUT_CHECKSUM_INPUT_BYTES = 24U;

inline void layout_checksum_fill(uint8_t *buf, const SharedMemoryHeader *header)
{
    if (buf == nullptr || header == nullptr)
    {
        return;
    }
    using pylabhub::utils::append_le_u32;
    using pylabhub::utils::append_le_u64;
    using pylabhub::utils::append_u8;
    size_t off = 0;
    append_le_u32(buf, off, header->ring_buffer_capacity);
    append_le_u32(buf, off, header->physical_page_size);
    append_le_u32(buf, off, header->logical_unit_size);
    append_le_u64(buf, off, static_cast<uint64_t>(header->flexible_zone_size));
    append_u8(buf, off, header->checksum_type);
    append_u8(buf, off, static_cast<uint8_t>(header->policy));
    append_u8(buf, off, static_cast<uint8_t>(header->consumer_sync_policy));
    append_u8(buf, off, 0); // reserved
    assert(off == LAYOUT_CHECKSUM_INPUT_BYTES);
}
} // namespace

void store_layout_checksum(SharedMemoryHeader *header)
{
    if (header == nullptr)
    {
        return;
    }
    std::array<uint8_t, LAYOUT_CHECKSUM_INPUT_BYTES> buf{};
    layout_checksum_fill(buf.data(), header);
    uint8_t *out = header->reserved_header + detail::LAYOUT_CHECKSUM_OFFSET;
    if (!pylabhub::crypto::compute_blake2b(out, buf.data(), buf.size()))
    {
        LOGGER_ERROR("[DataBlock] store_layout_checksum: compute_blake2b failed; storing zeros.");
        std::memset(out, 0, detail::LAYOUT_CHECKSUM_SIZE);
    }
}

bool validate_layout_checksum(const SharedMemoryHeader *header)
{
    if (header == nullptr)
    {
        return false;
    }
    std::array<uint8_t, LAYOUT_CHECKSUM_INPUT_BYTES> buf{};
    layout_checksum_fill(buf.data(), header);
    std::array<uint8_t, detail::CHECKSUM_BYTES> computed;
    if (!pylabhub::crypto::compute_blake2b(computed.data(), buf.data(), buf.size()))
    {
        return false;
    }
    const uint8_t *stored = header->reserved_header + detail::LAYOUT_CHECKSUM_OFFSET;
    return std::memcmp(computed.data(), stored, detail::LAYOUT_CHECKSUM_SIZE) == 0;
}

/** Single control surface for attach validation: layout checksum + optional config match.
 *  Call after validate_header_layout_hash(header). Returns false if layout checksum fails
 *  or if expected_config is non-null and header does not match it. */
static bool validate_attach_layout_and_config(const SharedMemoryHeader *header,
                                              const DataBlockConfig *expected_config)
{
    if (!validate_layout_checksum(header))
    {
        LOGGER_WARN("[DataBlock] Layout checksum validation failed during consumer attachment.");
        return false;
    }
    if (expected_config == nullptr)
    {
        return true;
    }
    const bool flex_ok = header->flexible_zone_size == expected_config->total_flexible_zone_size();
    const bool cap_ok = header->ring_buffer_capacity == expected_config->ring_buffer_capacity;
    const bool page_ok =
        header->physical_page_size == static_cast<uint32_t>(to_bytes(expected_config->physical_page_size));
    const bool stride_ok = detail::get_slot_stride_bytes(header) ==
                           static_cast<uint32_t>(expected_config->effective_logical_unit_size());
    const bool checksum_ok =
        header->checksum_type == static_cast<uint8_t>(expected_config->checksum_type);
    if (!flex_ok || !cap_ok || !page_ok || !stride_ok || !checksum_ok)
    {
        LOGGER_WARN("[DataBlock] Config mismatch during consumer attachment: flex_zone={}, cap={}, "
                    "page={}, stride={}, checksum={}",
                    flex_ok, cap_ok, page_ok, stride_ok, checksum_ok);
        return false;
    }
    return true;
}

// ============================================================================
// Factory Functions
// ============================================================================

// Internal implementation that accepts optional schema info
std::unique_ptr<DataBlockProducer>
create_datablock_producer_impl(MessageHub &hub, const std::string &name, DataBlockPolicy policy,
                               const DataBlockConfig &config,
                               const pylabhub::schema::SchemaInfo *schema_info)
{
    if (!lifecycle_initialized())
    {
        throw std::runtime_error(
            "DataBlock: Data Exchange Hub module not initialized. Create a LifecycleGuard in main() "
            "with pylabhub::hub::GetLifecycleModule() (and typically Logger, CryptoUtils) before creating producers.");
    }
    (void)policy; // Reserved for future policy-specific behavior
    auto impl = std::make_unique<DataBlockProducerImpl>();
    impl->name = name;
    impl->dataBlock = std::make_unique<DataBlock>(name, config);
    impl->checksum_policy = config.checksum_policy;
    impl->flexible_zones_info = build_flexible_zone_info(config.flexible_zone_configs);

    auto *header = impl->dataBlock->header();
    if (header != nullptr && schema_info != nullptr)
    {
        // Store schema hash (full 32 bytes)
        std::memcpy(header->schema_hash, schema_info->hash.data(), detail::CHECKSUM_BYTES);

        // Store packed schema version
        header->schema_version = schema_info->version.pack();

        LOGGER_DEBUG("[DataBlock:{}] Schema stored: {} v{}, hash={}...", name, schema_info->name,
                     schema_info->version.to_string(),
                     fmt::format("{:02x}{:02x}{:02x}{:02x}", schema_info->hash[0],
                                 schema_info->hash[1], schema_info->hash[2], schema_info->hash[3]));
    }
    else if (header != nullptr)
    {
        // No schema validation - zero out schema fields
        std::memset(header->schema_hash, 0, detail::CHECKSUM_BYTES);
        header->schema_version = 0;
    }

    ProducerInfo pinfo{};
    pinfo.shm_name = name;
    pinfo.producer_pid = pylabhub::platform::get_pid();
    pinfo.schema_hash.assign(reinterpret_cast<const char *>(header->schema_hash),
                             detail::CHECKSUM_BYTES);
    pinfo.schema_version = header->schema_version;
    if (!hub.register_producer(name, pinfo))
    {
        LOGGER_WARN("DataBlock: Failed to register producer '{}' with broker (discovery may be unavailable). Check broker connectivity and that the channel name is correct.", name);
    }
    return std::make_unique<DataBlockProducer>(std::move(impl));
}

// Non-template version (no schema validation)
std::unique_ptr<DataBlockProducer> create_datablock_producer(MessageHub &hub,
                                                             const std::string &name,
                                                             DataBlockPolicy policy,
                                                             const DataBlockConfig &config)
{
    return create_datablock_producer_impl(hub, name, policy, config, nullptr);
}

// Internal implementation that accepts optional schema info for validation
std::unique_ptr<DataBlockConsumer>
find_datablock_consumer_impl(MessageHub &hub, const std::string &name, uint64_t shared_secret,
                             const DataBlockConfig *expected_config,
                             const pylabhub::schema::SchemaInfo *schema_info)
{
    if (!lifecycle_initialized())
    {
        throw std::runtime_error(
            "DataBlock: Data Exchange Hub module not initialized. Create a LifecycleGuard in main() "
            "with pylabhub::hub::GetLifecycleModule() (and typically Logger, CryptoUtils) before finding consumers.");
    }
    auto impl = std::make_unique<DataBlockConsumerImpl>();
    impl->name = name;
    impl->dataBlock = std::make_unique<DataBlock>(name);

    auto *header = impl->dataBlock->header();
    if (header == nullptr)
    {
        return nullptr;
    }

    // Validate shared secret (first 8 bytes store capability for discovery)
    if (std::memcmp(header->shared_secret, &shared_secret, sizeof(shared_secret)) != 0)
    {
        return nullptr;
    }

    // Validate header layout (producer and consumer must have same SharedMemoryHeader ABI)
    try
    {
        validate_header_layout_hash(header);
    }
    catch (const pylabhub::schema::SchemaValidationException &)
    {
        header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
        LOGGER_WARN("[DataBlock:{}] Header layout mismatch during consumer attachment (ABI "
                    "incompatibility)",
                    name);
        return nullptr;
    }
    // Validate layout checksum + config (single control surface: see validate_attach_layout_and_config)
    if (!validate_attach_layout_and_config(header, expected_config))
    {
        LOGGER_WARN("[DataBlock:{}] Layout checksum or config mismatch during consumer attachment.",
                    name);
        return nullptr;
    }
    impl->checksum_policy = header->checksum_policy;
    if (expected_config != nullptr)
    {
        const auto &configs = expected_config->flexible_zone_configs;
        impl->flexible_zones_info = build_flexible_zone_info(configs);
        impl->dataBlock->set_flexible_zone_info_for_attach(configs);
    }

    // Validate schema if provided
    if (schema_info != nullptr)
    {
        // Check if producer stored a schema (non-zero hash)
        bool has_producer_schema = false;
        for (unsigned char byte : header->schema_hash)
        {
            if (byte != 0)
            {
                has_producer_schema = true;
                break;
            }
        }

        if (!has_producer_schema)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_WARN(
                "[DataBlock:{}] Producer did not store schema, but consumer expects schema '{}'",
                name, schema_info->name);
            return nullptr;
        }

        // Compare schema hashes
        if (std::memcmp(header->schema_hash, schema_info->hash.data(), detail::CHECKSUM_BYTES) != 0)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR(
                "[DataBlock:{}] Schema hash mismatch! Expected schema '{}' v{}, hash={}...", name,
                schema_info->name, schema_info->version.to_string(),
                fmt::format("{:02x}{:02x}{:02x}{:02x}", schema_info->hash[0], schema_info->hash[1],
                            schema_info->hash[2], schema_info->hash[3]));
            return nullptr;
        }

        // Validate schema version compatibility
        auto stored_version = pylabhub::schema::SchemaVersion::unpack(header->schema_version);
        if (stored_version.major != schema_info->version.major)
        {
            header->schema_mismatch_count.fetch_add(1, std::memory_order_relaxed);
            LOGGER_ERROR("[DataBlock:{}] Incompatible schema version! Producer: {}, Consumer: {}",
                         name, stored_version.to_string(), schema_info->version.to_string());
            return nullptr;
        }

        LOGGER_DEBUG("[DataBlock:{}] Schema validated: {} v{}", name, schema_info->name,
                     schema_info->version.to_string());
    }

    header->active_consumer_count.fetch_add(1, std::memory_order_relaxed);
    ConsumerInfo cinfo{};
    cinfo.shm_name = name;
    cinfo.schema_hash.assign(reinterpret_cast<const char *>(header->schema_hash),
                             detail::CHECKSUM_BYTES);
    cinfo.schema_version = header->schema_version;
    if (!hub.register_consumer(name, cinfo))
    {
        LOGGER_WARN("DataBlock: Failed to register consumer for '{}' with broker (discovery may be unavailable). Check broker connectivity and that the channel name is correct.", name);
    }
    return std::make_unique<DataBlockConsumer>(std::move(impl));
}

std::unique_ptr<DataBlockConsumer> find_datablock_consumer(MessageHub &hub, const std::string &name,
                                                           uint64_t shared_secret)
{
    return find_datablock_consumer_impl(hub, name, shared_secret, nullptr, nullptr);
}

std::unique_ptr<DataBlockConsumer> find_datablock_consumer(MessageHub &hub, const std::string &name,
                                                           uint64_t shared_secret,
                                                           const DataBlockConfig &expected_config)
{
    return find_datablock_consumer_impl(hub, name, shared_secret, &expected_config, nullptr);
}

WriteTransactionGuard::WriteTransactionGuard(DataBlockProducer &producer, int timeout_ms)
    : producer_(&producer), slot_(producer.acquire_write_slot(timeout_ms)),
      acquired_(static_cast<bool>(slot_)), committed_(false), aborted_(false)
{
}

WriteTransactionGuard::~WriteTransactionGuard() noexcept
{
    if (acquired_ && !aborted_ && slot_)
    {
        // The user is responsible for calling slot().commit(bytes_written).
        // We release the slot; in destructor we cannot throw, so we log on failure.
        if (!producer_->release_write_slot(*slot_))
        {
            LOGGER_WARN("WriteTransactionGuard: release_write_slot failed in destructor. name='{}' slot_id={} slot_index={} (slot may remain held; check for checksum/commit failure in previous log).",
                        producer_->name(), slot_->slot_id(), slot_->slot_index());
        }
    }
}

WriteTransactionGuard::WriteTransactionGuard(WriteTransactionGuard &&other) noexcept
    : producer_(other.producer_), slot_(std::move(other.slot_)), acquired_(other.acquired_),
      committed_(other.committed_), aborted_(other.aborted_)
{
    other.acquired_ = false;  // Transfer ownership
    other.committed_ = false; // Ensure other doesn't commit/release
    other.aborted_ = true;    // Mark other as aborted to prevent its destructor actions
}

WriteTransactionGuard &WriteTransactionGuard::operator=(WriteTransactionGuard &&other) noexcept
{
    if (this != &other)
    {
        // Release our current slot first if it was acquired
        if (acquired_ && !aborted_ && slot_)
        {
            if (!producer_->release_write_slot(*slot_))
            {
                LOGGER_WARN("WriteTransactionGuard::operator=: release_write_slot failed. name='{}' slot_id={} slot_index={} (slot may remain held; check for checksum/commit failure in previous log).",
                            producer_->name(), slot_->slot_id(), slot_->slot_index());
            }
        }

        // Transfer ownership from other
        producer_ = other.producer_;
        slot_ = std::move(other.slot_);
        acquired_ = other.acquired_;
        committed_ = other.committed_;
        aborted_ = other.aborted_;

        other.acquired_ = false;
        other.committed_ = false;
        other.aborted_ = true;
    }
    return *this;
}

WriteTransactionGuard::operator bool() const noexcept
{
    return acquired_ && !aborted_ && static_cast<bool>(slot_);
}

std::optional<std::reference_wrapper<SlotWriteHandle>> WriteTransactionGuard::slot() noexcept
{
    if (!slot_)
    {
        return std::nullopt;
    }
    return std::ref(*slot_);
}

void WriteTransactionGuard::commit()
{
    // This `commit()` merely sets an internal flag.
    // The user MUST call `slot().commit(bytes_written)` explicitly
    // to make the data visible. This flag is for internal state tracking.
    if (!acquired_)
    {
        throw std::runtime_error("WriteTransactionGuard: Cannot commit, slot not acquired.");
    }
    if (aborted_)
    {
        throw std::runtime_error("WriteTransactionGuard: Cannot commit, transaction aborted.");
    }
    committed_ = true;
}

void WriteTransactionGuard::abort() noexcept
{
    aborted_ = true;
    committed_ = false; // Ensure it's not marked as committed if aborted
}

// ============================================================================
// ReadTransactionGuard
// ============================================================================

ReadTransactionGuard::ReadTransactionGuard(DataBlockConsumer &consumer, uint64_t slot_id,
                                           int timeout_ms)
    : consumer_(&consumer), slot_(consumer.acquire_consume_slot(slot_id, timeout_ms)),
      acquired_(static_cast<bool>(slot_))
{
}

ReadTransactionGuard::~ReadTransactionGuard() noexcept
{
    if (acquired_ && slot_)
    {
        if (!consumer_->release_consume_slot(*slot_))
        {
            LOGGER_WARN("ReadTransactionGuard: release_consume_slot failed in destructor. name='{}' slot_id={} slot_index={} (slot may remain held; check for validation/checksum failure in previous log).",
                        consumer_->name(), slot_->slot_id(), slot_->slot_index());
        }
    }
}

ReadTransactionGuard::ReadTransactionGuard(ReadTransactionGuard &&other) noexcept
    : consumer_(other.consumer_), slot_(std::move(other.slot_)), acquired_(other.acquired_)
{
    other.acquired_ = false; // Transfer ownership
}

ReadTransactionGuard &ReadTransactionGuard::operator=(ReadTransactionGuard &&other) noexcept
{
    if (this != &other)
    {
        // Release our current slot first if it was acquired
        if (acquired_ && slot_)
        {
            if (!consumer_->release_consume_slot(*slot_))
            {
                LOGGER_WARN("ReadTransactionGuard::operator=: release_consume_slot failed. name='{}' slot_id={} slot_index={} (slot may remain held; check for validation/checksum failure in previous log).",
                            consumer_->name(), slot_->slot_id(), slot_->slot_index());
            }
        }

        // Transfer ownership from other
        consumer_ = other.consumer_;
        slot_ = std::move(other.slot_);
        acquired_ = other.acquired_;

        other.acquired_ = false;
    }
    return *this;
}

ReadTransactionGuard::operator bool() const noexcept
{
    return acquired_ && static_cast<bool>(slot_);
}

std::optional<std::reference_wrapper<const SlotConsumeHandle>>
ReadTransactionGuard::slot() const noexcept
{
    if (!slot_)
    {
        return std::nullopt;
    }
    return std::ref(*slot_);
}

// ============================================================================
// Slot RW Coordinator C API (extern "C" for ABI stability; global symbol names)
// ============================================================================
#include "utils/slot_rw_coordinator.h"

extern "C"
{

    SlotAcquireResult slot_rw_acquire_write(pylabhub::hub::SlotRWState *rw_state, int timeout_ms)
    {
        if (rw_state == nullptr)
        {
            return SLOT_ACQUIRE_ERROR;
        }
        return acquire_write(rw_state, nullptr, timeout_ms);
    }

    void slot_rw_commit(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state != nullptr)
        {
            commit_write(rw_state, nullptr);
        }
    }

    void slot_rw_release_write(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state != nullptr)
        {
            release_write(rw_state, nullptr);
        }
    }

    SlotAcquireResult slot_rw_acquire_read(pylabhub::hub::SlotRWState *rw_state,
                                           uint64_t *out_generation)
    {
        if (rw_state == nullptr || out_generation == nullptr)
        {
            return SLOT_ACQUIRE_ERROR;
        }
        return acquire_read(rw_state, nullptr, out_generation);
    }

    bool slot_rw_validate_read(pylabhub::hub::SlotRWState *rw_state, uint64_t generation)
    {
        if (rw_state == nullptr)
        {
            return false;
        }
        return validate_read_impl(rw_state, nullptr, generation);
    }

    void slot_rw_release_read(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state != nullptr)
        {
            release_read(rw_state, nullptr);
        }
    }

    const char *slot_acquire_result_string(SlotAcquireResult result)
    {
        switch (result)
        {
        case SLOT_ACQUIRE_OK:
            return "OK";
        case SLOT_ACQUIRE_TIMEOUT:
            return "TIMEOUT";
        case SLOT_ACQUIRE_NOT_READY:
            return "NOT_READY";
        case SLOT_ACQUIRE_LOCKED:
            return "LOCKED";
        case SLOT_ACQUIRE_ERROR:
            return "ERROR";
        case SLOT_ACQUIRE_INVALID_STATE:
            return "INVALID_STATE";
        default:
            return "UNKNOWN";
        }
    }

    int slot_rw_get_metrics(const pylabhub::hub::SharedMemoryHeader *shared_memory_header,
                            DataBlockMetrics *out_metrics)
    {
        if (shared_memory_header == nullptr || out_metrics == nullptr)
        {
            return -1;
        }
        /* State snapshot (not reset by reset_metrics) */
        out_metrics->commit_index =
            shared_memory_header->commit_index.load(std::memory_order_relaxed);
        out_metrics->slot_count = pylabhub::hub::detail::get_slot_count(shared_memory_header);
        out_metrics->_reserved_metrics_pad = 0;
        /* Metrics */
        out_metrics->writer_timeout_count =
            shared_memory_header->writer_timeout_count.load(std::memory_order_relaxed);
        out_metrics->writer_lock_timeout_count =
            shared_memory_header->writer_lock_timeout_count.load(std::memory_order_relaxed);
        out_metrics->writer_reader_timeout_count =
            shared_memory_header->writer_reader_timeout_count.load(std::memory_order_relaxed);
        out_metrics->writer_blocked_total_ns =
            shared_memory_header->writer_blocked_total_ns.load(std::memory_order_relaxed);
        out_metrics->write_lock_contention =
            shared_memory_header->write_lock_contention.load(std::memory_order_relaxed);
        out_metrics->write_generation_wraps =
            shared_memory_header->write_generation_wraps.load(std::memory_order_relaxed);
        out_metrics->reader_not_ready_count =
            shared_memory_header->reader_not_ready_count.load(std::memory_order_relaxed);
        out_metrics->reader_race_detected =
            shared_memory_header->reader_race_detected.load(std::memory_order_relaxed);
        out_metrics->reader_validation_failed =
            shared_memory_header->reader_validation_failed.load(std::memory_order_relaxed);
        out_metrics->reader_peak_count =
            shared_memory_header->reader_peak_count.load(std::memory_order_relaxed);
        out_metrics->last_error_timestamp_ns =
            shared_memory_header->last_error_timestamp_ns.load(std::memory_order_relaxed);
        out_metrics->last_error_code =
            shared_memory_header->last_error_code.load(std::memory_order_relaxed);
        out_metrics->error_sequence =
            shared_memory_header->error_sequence.load(std::memory_order_relaxed);
        out_metrics->slot_acquire_errors =
            shared_memory_header->slot_acquire_errors.load(std::memory_order_relaxed);
        out_metrics->slot_commit_errors =
            shared_memory_header->slot_commit_errors.load(std::memory_order_relaxed);
        out_metrics->checksum_failures =
            shared_memory_header->checksum_failures.load(std::memory_order_relaxed);
        out_metrics->zmq_send_failures =
            shared_memory_header->zmq_send_failures.load(std::memory_order_relaxed);
        out_metrics->zmq_recv_failures =
            shared_memory_header->zmq_recv_failures.load(std::memory_order_relaxed);
        out_metrics->zmq_timeout_count =
            shared_memory_header->zmq_timeout_count.load(std::memory_order_relaxed);
        out_metrics->recovery_actions_count =
            shared_memory_header->recovery_actions_count.load(std::memory_order_relaxed);
        out_metrics->schema_mismatch_count =
            shared_memory_header->schema_mismatch_count.load(std::memory_order_relaxed);
        out_metrics->heartbeat_sent_count =
            shared_memory_header->heartbeat_sent_count.load(std::memory_order_relaxed);
        out_metrics->heartbeat_failed_count =
            shared_memory_header->heartbeat_failed_count.load(std::memory_order_relaxed);
        out_metrics->last_heartbeat_ns =
            shared_memory_header->last_heartbeat_ns.load(std::memory_order_relaxed);
        out_metrics->total_slots_written =
            shared_memory_header->total_slots_written.load(std::memory_order_relaxed);
        out_metrics->total_slots_read =
            shared_memory_header->total_slots_read.load(std::memory_order_relaxed);
        out_metrics->total_bytes_written =
            shared_memory_header->total_bytes_written.load(std::memory_order_relaxed);
        out_metrics->total_bytes_read =
            shared_memory_header->total_bytes_read.load(std::memory_order_relaxed);
        out_metrics->uptime_seconds =
            shared_memory_header->uptime_seconds.load(std::memory_order_relaxed);
        out_metrics->creation_timestamp_ns =
            shared_memory_header->creation_timestamp_ns.load(std::memory_order_relaxed);
        return 0;
    }

    uint64_t slot_rw_get_total_slots_written(
        const pylabhub::hub::SharedMemoryHeader *header)
    {
        return header != nullptr
                   ? header->total_slots_written.load(std::memory_order_relaxed)
                   : 0;
    }

    uint64_t slot_rw_get_commit_index(const pylabhub::hub::SharedMemoryHeader *header)
    {
        return header != nullptr
                   ? header->commit_index.load(std::memory_order_relaxed)
                   : 0;
    }

    uint32_t slot_rw_get_slot_count(const pylabhub::hub::SharedMemoryHeader *header)
    {
        return header != nullptr ? pylabhub::hub::detail::get_slot_count(header) : 0;
    }

    int slot_rw_reset_metrics(pylabhub::hub::SharedMemoryHeader *shared_memory_header)
    {
        if (shared_memory_header == nullptr)
        {
            return -1;
        }
        shared_memory_header->writer_timeout_count.store(0, std::memory_order_release);
        shared_memory_header->writer_lock_timeout_count.store(0, std::memory_order_release);
        shared_memory_header->writer_reader_timeout_count.store(0, std::memory_order_release);
        shared_memory_header->writer_blocked_total_ns.store(0, std::memory_order_release);
        shared_memory_header->write_lock_contention.store(0, std::memory_order_release);
        shared_memory_header->write_generation_wraps.store(0, std::memory_order_release);
        shared_memory_header->reader_not_ready_count.store(0, std::memory_order_release);
        shared_memory_header->reader_race_detected.store(0, std::memory_order_release);
        shared_memory_header->reader_validation_failed.store(0, std::memory_order_release);
        shared_memory_header->reader_peak_count.store(0, std::memory_order_release);
        shared_memory_header->last_error_timestamp_ns.store(0, std::memory_order_release);
        shared_memory_header->last_error_code.store(0, std::memory_order_release);
        shared_memory_header->error_sequence.store(0, std::memory_order_release);
        shared_memory_header->slot_acquire_errors.store(0, std::memory_order_release);
        shared_memory_header->slot_commit_errors.store(0, std::memory_order_release);
        shared_memory_header->checksum_failures.store(0, std::memory_order_release);
        shared_memory_header->zmq_send_failures.store(0, std::memory_order_release);
        shared_memory_header->zmq_recv_failures.store(0, std::memory_order_release);
        shared_memory_header->zmq_timeout_count.store(0, std::memory_order_release);
        shared_memory_header->recovery_actions_count.store(0, std::memory_order_release);
        shared_memory_header->schema_mismatch_count.store(0, std::memory_order_release);
        shared_memory_header->heartbeat_sent_count.store(0, std::memory_order_release);
        shared_memory_header->heartbeat_failed_count.store(0, std::memory_order_release);
        shared_memory_header->last_heartbeat_ns.store(0, std::memory_order_release);
        shared_memory_header->total_slots_written.store(0, std::memory_order_release);
        shared_memory_header->total_slots_read.store(0, std::memory_order_release);
        shared_memory_header->total_bytes_written.store(0, std::memory_order_release);
        shared_memory_header->total_bytes_read.store(0, std::memory_order_release);
        shared_memory_header->uptime_seconds.store(0, std::memory_order_release);
        return 0;
    }

} // extern "C"

} // namespace pylabhub::hub
