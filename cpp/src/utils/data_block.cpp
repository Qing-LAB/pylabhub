#include "plh_service.hpp" // Includes crypto_utils, logger, lifecycle
#include "plh_platform.hpp"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include "utils/data_block_mutex.hpp"
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <type_traits>

namespace pylabhub::hub
{
// Forward declaration for use in anonymous namespace helpers
class DataBlock;

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

// Use detail::DATABLOCK_MAGIC_NUMBER and detail::is_header_magic_valid from data_block.hpp
static constexpr uint16_t DATABLOCK_VERSION_MAJOR = HEADER_VERSION_MAJOR;
static constexpr uint16_t DATABLOCK_VERSION_MINOR = HEADER_VERSION_MINOR;
static constexpr uint64_t INVALID_SLOT_ID = std::numeric_limits<uint64_t>::max();

/// Sync_reader: pointer to the i-th consumer's next-read slot id in reserved_header.
inline std::atomic<uint64_t> *consumer_next_read_slot_ptr(SharedMemoryHeader *h, size_t i)
{
    return reinterpret_cast<std::atomic<uint64_t> *>(h->reserved_header +
                                                     CONSUMER_READ_POSITIONS_OFFSET) +
           i;
}

/**
 * Policy-based next slot to read. Single place for Latest_only / Single_reader / Sync_reader.
 * Used by DataBlockConsumer::acquire_consume_slot and DataBlockSlotIterator::try_next.
 * @return Next slot_id to try, or INVALID_SLOT_ID if none available yet.
 */
inline uint64_t get_next_slot_to_read(const SharedMemoryHeader *h,
                                      uint64_t last_seen_or_consumed_slot_id,
                                      int heartbeat_slot)
{
    const ConsumerSyncPolicy policy = h->consumer_sync_policy;
    if (policy == ConsumerSyncPolicy::Latest_only)
    {
        uint64_t next = h->commit_index.load(std::memory_order_acquire);
        if (next == INVALID_SLOT_ID || next == last_seen_or_consumed_slot_id)
            return INVALID_SLOT_ID;
        return next;
    }
    if (policy == ConsumerSyncPolicy::Single_reader)
    {
        uint64_t next = h->read_index.load(std::memory_order_acquire);
        if (h->commit_index.load(std::memory_order_acquire) < next)
            return INVALID_SLOT_ID;
        return next;
    }
    // Sync_reader
    if (heartbeat_slot < 0 || heartbeat_slot >= static_cast<int>(MAX_CONSUMER_HEARTBEATS))
        return INVALID_SLOT_ID;
    uint64_t next =
        consumer_next_read_slot_ptr(const_cast<SharedMemoryHeader *>(h),
                                    static_cast<size_t>(heartbeat_slot))
            ->load(std::memory_order_acquire);
    if (h->commit_index.load(std::memory_order_acquire) < next)
        return INVALID_SLOT_ID;
    return next;
}

// === SlotRWState Coordination Logic ===
// Note: backoff() is now provided by pylabhub::utils::backoff() from backoff_strategy.hpp

// 4.2.1 Writer Acquisition Flow
SlotAcquireResult acquire_write(SlotRWState *rw, SharedMemoryHeader *header, int timeout_ms)
{
    auto start_time = pylabhub::platform::monotonic_time_ns();
    uint64_t my_pid = pylabhub::platform::get_pid();
    int iteration = 0;

    while (true)
    {
        uint64_t expected_lock = 0;
        if (rw->write_lock.compare_exchange_strong(expected_lock, my_pid, std::memory_order_acquire,
                                                   std::memory_order_relaxed))
        {
            // Lock acquired
            break;
        }
        else
        {
            // Lock held by another process
            if (pylabhub::platform::is_process_alive(expected_lock))
            {
                // Valid contention, continue waiting or timeout
            }
            else
            {
                // Zombie lock - force reclaim
                LOGGER_WARN("SlotRWState: Detected zombie write lock by PID {}. Force reclaiming.",
                            expected_lock);
                rw->write_lock.store(my_pid, std::memory_order_release); // Reclaim
                if (header)
                    header->write_lock_contention.fetch_add(1, std::memory_order_relaxed);
                break; // Acquired
            }
        }

        // Check for timeout if lock was not acquired or was valid contention
        if (timeout_ms > 0 && pylabhub::platform::elapsed_time_ns(start_time) / 1'000'000 >=
                                  static_cast<uint64_t>(timeout_ms))
        {
            if (header)
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
    rw->writer_waiting.store(1, std::memory_order_relaxed); // Signal readers to drain

    iteration = 0;
    while (true)
    {
        std::atomic_thread_fence(std::memory_order_seq_cst); // Force visibility

        uint32_t readers = rw->reader_count.load(std::memory_order_acquire);
        if (readers == 0)
        {
            break; // All readers finished
        }

        // Check timeout
        if (timeout_ms > 0 && pylabhub::platform::elapsed_time_ns(start_time) / 1'000'000 >=
                                  static_cast<uint64_t>(timeout_ms))
        {
            rw->writer_waiting.store(0, std::memory_order_relaxed);
            rw->write_lock.store(
                0, std::memory_order_release); // Release the lock before returning timeout
            if (header)
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
    rw->writer_waiting.store(0, std::memory_order_relaxed); // All readers drained

    // Transition to WRITING state
    rw->slot_state.store(SlotRWState::SlotState::WRITING, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst); // Ensure state change is visible

    return SLOT_ACQUIRE_OK;
}

// 4.2.2 Writer Commit Flow
void commit_write(SlotRWState *rw, SharedMemoryHeader *header)
{
    rw->write_generation.fetch_add(
        1, std::memory_order_release); // Step 1: Increment generation counter
    rw->slot_state.store(SlotRWState::SlotState::COMMITTED,
                         std::memory_order_release); // Step 2: Transition to COMMITTED state
    if (header)
        header->commit_index.fetch_add(
            1, std::memory_order_release); // Step 3: Increment global commit index (makes visible
                                           // to consumers)
    // Memory ordering: All writes before this release are visible to
    // any consumer that performs acquire on commit_index or slot_state
}

// 4.2.2b Writer Release (without commit) - for C API and abort paths
void release_write(SlotRWState *rw, SharedMemoryHeader * /*header*/)
{
    rw->write_lock.store(0, std::memory_order_release);
    rw->slot_state.store(SlotRWState::SlotState::FREE, std::memory_order_release);
}

// 4.2.3 Reader Acquisition Flow (TOCTTOU-Safe)
SlotAcquireResult acquire_read(SlotRWState *rw, SharedMemoryHeader *header,
                               uint64_t *out_generation)
{
    // Step 1: Check slot state (first check)
    SlotRWState::SlotState state = rw->slot_state.load(std::memory_order_acquire);
    if (state != SlotRWState::SlotState::COMMITTED)
    {
        return SLOT_ACQUIRE_NOT_READY;
    }

    // Step 2: Register as reader (minimize race window)
    rw->reader_count.fetch_add(1, std::memory_order_acq_rel);

    // Step 3: Memory fence (force writer visibility)
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Step 4: Double-check slot state (TOCTTOU mitigation)
    state = rw->slot_state.load(std::memory_order_acquire);
    if (state != SlotRWState::SlotState::COMMITTED)
    {
        // Race detected! Writer changed state after our first check
        // but before we registered. Safely abort.
        rw->reader_count.fetch_sub(1, std::memory_order_release);
        if (header)
            header->reader_race_detected.fetch_add(1, std::memory_order_relaxed);
        return SLOT_ACQUIRE_NOT_READY;
    }

    // Step 5: Capture generation for optimistic validation
    *out_generation = rw->write_generation.load(std::memory_order_acquire);

    return SLOT_ACQUIRE_OK;
}

// 4.2.4 Reader Validation (Wrap-Around Detection)
bool validate_read(SlotRWState *rw, SharedMemoryHeader *header, uint64_t captured_gen)
{
    // Check if slot was overwritten during read
    uint64_t current_gen = rw->write_generation.load(std::memory_order_acquire);

    if (current_gen != captured_gen)
    {
        // Slot was reused (ring buffer wrapped around)
        if (header)
            header->reader_validation_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

// 4.2.5 Reader Release Flow
void release_read(SlotRWState *rw, SharedMemoryHeader *header)
{
    // Decrement reader count
    uint32_t prev_count = rw->reader_count.fetch_sub(1, std::memory_order_release);

    // Track peak reader count (optional; header may be null for C API)
    if (header)
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

// ============================================================================
// DataBlockLayout - Centralized layout calculation (single source of truth)
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

    static DataBlockLayout from_config(const DataBlockConfig &config)
    {
        DataBlockLayout L;
        L.slot_rw_state_offset = sizeof(SharedMemoryHeader);
        L.slot_rw_state_size = config.ring_buffer_capacity * sizeof(SlotRWState);
        L.slot_checksum_size = config.enable_checksum
                                  ? (config.ring_buffer_capacity * detail::SLOT_CHECKSUM_ENTRY_SIZE)
                                  : 0;
        L.slot_checksum_offset = L.slot_rw_state_offset + L.slot_rw_state_size;
        L.flexible_zone_size = config.total_flexible_zone_size();
        L.flexible_zone_offset = L.slot_checksum_offset + L.slot_checksum_size;
        L.structured_buffer_size = config.structured_buffer_size();
        L.structured_buffer_offset = L.flexible_zone_offset + L.flexible_zone_size;
        L.total_size = L.structured_buffer_offset + L.structured_buffer_size;
        return L;
    }

    static DataBlockLayout from_header(const SharedMemoryHeader *h)
    {
        DataBlockLayout L;
        if (!h)
            return L;
        uint32_t cap = (h->ring_buffer_capacity > 0) ? h->ring_buffer_capacity : 1u;
        L.slot_rw_state_offset = sizeof(SharedMemoryHeader);
        L.slot_rw_state_size = cap * sizeof(SlotRWState);
        L.slot_checksum_size =
            h->enable_checksum ? (cap * detail::SLOT_CHECKSUM_ENTRY_SIZE) : 0;
        L.slot_checksum_offset = L.slot_rw_state_offset + L.slot_rw_state_size;
        L.flexible_zone_size = h->flexible_zone_size;
        L.flexible_zone_offset = L.slot_checksum_offset + L.slot_checksum_size;
        L.structured_buffer_size = cap * h->unit_block_size;
        L.structured_buffer_offset = L.flexible_zone_offset + L.flexible_zone_size;
        L.total_size = L.structured_buffer_offset + L.structured_buffer_size;
        return L;
    }

    char *slot_checksum_base(char *segment_base) const
    {
        return segment_base + slot_checksum_offset;
    }
    const char *slot_checksum_base(const char *segment_base) const
    {
        return segment_base + slot_checksum_offset;
    }

#if !defined(NDEBUG)
    bool validate() const
    {
        if (slot_rw_state_offset != sizeof(SharedMemoryHeader))
            return false;
        if (slot_checksum_offset != slot_rw_state_offset + slot_rw_state_size)
            return false;
        if (flexible_zone_offset != slot_checksum_offset + slot_checksum_size)
            return false;
        if (structured_buffer_offset != flexible_zone_offset + flexible_zone_size)
            return false;
        if (total_size != structured_buffer_offset + structured_buffer_size)
            return false;
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
    for (const auto &c : configs)
    {
        FlexibleZoneInfo info;
        info.offset = offset;
        info.size = c.size;
        info.spinlock_index = c.spinlock_index;
        out.push_back(info);
        offset += c.size;
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
    DataBlock(const std::string &name, const DataBlockConfig &config)
        : m_name(name), m_is_creator(true)
    {
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
            throw std::runtime_error("Failed to create file mapping for '" + m_name +
                                     "'. Error: " + std::to_string(GetLastError()));
#else
            throw std::runtime_error("shm_create failed for '" + m_name +
                                     "'. Error: " + std::to_string(errno));
#endif
        }
        // Placement new for SharedMemoryHeader (value-initializes; do not memset non-trivials)
        m_header = new (m_shm.base) SharedMemoryHeader();

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
        m_header->unit_block_size = static_cast<uint32_t>(to_bytes(config.unit_block_size));
        m_header->ring_buffer_capacity = config.ring_buffer_capacity;
        m_header->flexible_zone_size = m_layout.flexible_zone_size;
        m_header->enable_checksum = config.enable_checksum;
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
        for (size_t i = 0; i < 2; ++i)
            m_header->reserved_errors[i].store(0, std::memory_order_release);

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
        for (size_t i = 0; i < 2; ++i)
            m_header->reserved_perf[i].store(0, std::memory_order_release);

        // Initialize Consumer Heartbeats
        for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i)
        {
            m_header->consumer_heartbeats[i].consumer_id.store(0, std::memory_order_release);
            m_header->consumer_heartbeats[i].last_heartbeat_ns.store(0, std::memory_order_release);
        }

        // Initialize SharedSpinLock states (same factory logic as in-process spinlock)
        for (size_t i = 0; i < MAX_SHARED_SPINLOCKS; ++i)
            init_spinlock_state(&m_header->spinlock_states[i]);

        // 3. Initialize SlotRWState array (using layout)
        m_slot_rw_states_array = reinterpret_cast<SlotRWState *>(
            reinterpret_cast<char *>(m_shm.base) + m_layout.slot_rw_state_offset);

        for (uint32_t i = 0; i < config.ring_buffer_capacity; ++i)
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
            m_flexible_zone_info[configs[i].name] = vec[i];

        // Sync_reader: initialize per-consumer read positions in reserved_header
        for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i)
            consumer_next_read_slot_ptr(m_header, i)->store(0, std::memory_order_release);

        std::atomic_thread_fence(std::memory_order_release);
        m_header->magic_number.store(
            detail::DATABLOCK_MAGIC_NUMBER,
            std::memory_order_release); // Set magic number last for atomicity

        // Store header layout hash for protocol check (consumer validates same ABI)
        pylabhub::schema::SchemaInfo header_schema = get_shared_memory_header_schema_info();
        std::memcpy(m_header->reserved_header + detail::HEADER_LAYOUT_HASH_OFFSET,
                    header_schema.hash.data(), detail::HEADER_LAYOUT_HASH_SIZE);

        LOGGER_INFO("DataBlock '{}' created with total size {} bytes.", m_name, m_size);
    }

    DataBlock(const std::string &name) : m_name(name), m_is_creator(false)
    {
        m_shm = pylabhub::platform::shm_attach(m_name.c_str());
        if (m_shm.base == nullptr)
        {
#if defined(PYLABHUB_PLATFORM_WIN64)
            throw std::runtime_error("Failed to open file mapping for consumer '" + m_name +
                                     "'. Error: " + std::to_string(GetLastError()));
#else
            throw std::runtime_error("shm_attach failed for consumer '" + m_name +
                                     "'. Error: " + std::to_string(errno));
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
                "DataBlock '" + m_name +
                "' initialization timeout - producer may have crashed or not fully initialized.");
        }

        // Validate version compatibility
        if (m_header->version_major != DATABLOCK_VERSION_MAJOR ||
            m_header->version_minor >
                DATABLOCK_VERSION_MINOR) // Consumer can read older minor versions
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error("DataBlock '" + m_name + "' version mismatch. Producer: " +
                                     std::to_string(m_header->version_major) + "." +
                                     std::to_string(m_header->version_minor) +
                                     ", Consumer: " + std::to_string(DATABLOCK_VERSION_MAJOR) +
                                     "." + std::to_string(DATABLOCK_VERSION_MINOR));
        }

        // Validate total size
        if (m_size != m_header->total_block_size)
        {
            pylabhub::platform::shm_close(&m_shm);
            throw std::runtime_error("DataBlock '" + m_name + "' size mismatch. Expected " +
                                     std::to_string(m_header->total_block_size) + ", got " +
                                     std::to_string(m_size));
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
        throw std::runtime_error("DataBlock '" + m_name + "': No free spinlock slots.");
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
        if (!m_header || index >= m_header->ring_buffer_capacity)
        {
            throw std::out_of_range("SlotRWState index " + std::to_string(index) +
                                    " out of range or header invalid.");
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
            m_flexible_zone_info[configs[i].name] = vec[i];
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
inline bool update_checksum_flexible_zone_impl(DataBlock *block, size_t flexible_zone_idx)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->enable_checksum) // Use enable_checksum
        return false;

    // Get the FlexibleZoneInfo from the block's flexible_zone_info map
    if (flexible_zone_idx >= detail::MAX_FLEXIBLE_ZONE_CHECKSUMS ||
        flexible_zone_idx >= block->flexible_zone_info().size())
    {
        return false;
    }
    auto it = block->flexible_zone_info().begin();
    std::advance(it, flexible_zone_idx);
    const FlexibleZoneInfo &zone_info = it->second;

    char *flex = block->flexible_data_zone();
    size_t len = zone_info.size;              // Use size from zone_info
    char *zone_ptr = flex + zone_info.offset; // Calculate pointer to specific zone

    if (!zone_ptr || len == 0)
        return false;

    // Checksum data is now per flexible zone, stored in SharedMemoryHeader::flexible_zone_checksums
    if (!pylabhub::crypto::compute_blake2b(
            h->flexible_zone_checksums[flexible_zone_idx].checksum_bytes, zone_ptr,
            len)) // Use flexible_zone_checksums
        return false;

    h->flexible_zone_checksums[flexible_zone_idx].valid.store(
        1, std::memory_order_release); // Use valid
    return true;
}

inline bool update_checksum_slot_impl(DataBlock *block, size_t slot_index)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->enable_checksum)
        return false;
    uint32_t slot_count = (h->ring_buffer_capacity > 0) ? h->ring_buffer_capacity : 1u;
    if (slot_index >= slot_count)
        return false;
    size_t slot_size = h->unit_block_size;
    if (slot_size == 0)
        return false;
    char *buf = block->structured_data_buffer();
    if (!buf)
        return false;
    char *base = reinterpret_cast<char *>(block->segment());
    char *slot_checksum_base_ptr = block->layout().slot_checksum_base(base);
    uint8_t *slot_checksum = reinterpret_cast<uint8_t *>(
        slot_checksum_base_ptr + slot_index * detail::SLOT_CHECKSUM_ENTRY_SIZE);
    std::atomic<uint8_t> *slot_valid =
        reinterpret_cast<std::atomic<uint8_t> *>(slot_checksum + detail::CHECKSUM_BYTES);
    const void *slot_data = buf + slot_index * slot_size;
    if (!pylabhub::crypto::compute_blake2b(slot_checksum, slot_data, slot_size))
        return false;
    slot_valid->store(1, std::memory_order_release);
    return true;
}

inline bool verify_checksum_flexible_zone_impl(const DataBlock *block, size_t flexible_zone_idx)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->enable_checksum) // Use enable_checksum
        return false;

    // Get the FlexibleZoneInfo from the block's flexible_zone_info map
    if (flexible_zone_idx >= detail::MAX_FLEXIBLE_ZONE_CHECKSUMS ||
        flexible_zone_idx >= block->flexible_zone_info().size())
    {
        return false;
    }
    auto it = block->flexible_zone_info().begin();
    std::advance(it, flexible_zone_idx);
    const FlexibleZoneInfo &zone_info = it->second;

    if (h->flexible_zone_checksums[flexible_zone_idx].valid.load(std::memory_order_acquire) !=
        1) // Use valid
        return false;

    const char *flex = block->flexible_data_zone();
    size_t len = zone_info.size;                    // Use size from zone_info
    const char *zone_ptr = flex + zone_info.offset; // Calculate pointer to specific zone

    if (!zone_ptr || len == 0)
        return false;

    // Checksum data is now per flexible zone, stored in SharedMemoryHeader::flexible_zone_checksums
    return pylabhub::crypto::verify_blake2b(
        h->flexible_zone_checksums[flexible_zone_idx].checksum_bytes, zone_ptr,
        len); // Use flexible_zone_checksums
}

inline bool verify_checksum_slot_impl(const DataBlock *block, size_t slot_index)
{
    if (!block || !block->header())
        return false;
    auto *h = block->header();
    if (!h->enable_checksum)
        return false;
    uint32_t slot_count = (h->ring_buffer_capacity > 0) ? h->ring_buffer_capacity : 1u;
    if (slot_index >= slot_count)
        return false;
    const char *base = reinterpret_cast<const char *>(block->segment());
    const char *slot_checksum_base_ptr = block->layout().slot_checksum_base(base);
    const uint8_t *slot_checksum = reinterpret_cast<const uint8_t *>(
        slot_checksum_base_ptr + slot_index * detail::SLOT_CHECKSUM_ENTRY_SIZE);
    const std::atomic<uint8_t> *slot_valid =
        reinterpret_cast<const std::atomic<uint8_t> *>(slot_checksum + detail::CHECKSUM_BYTES);
    if (slot_valid->load(std::memory_order_acquire) != 1)
        return false;
    size_t slot_size = h->unit_block_size;
    if (slot_size == 0)
        return false;
    const char *buf = block->structured_data_buffer();
    if (!buf)
        return false;
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
bool release_write_handle(SlotWriteHandleImpl &);
bool release_consume_handle(SlotConsumeHandleImpl &);
} // namespace

// ============================================================================
// DataBlockProducerImpl
// ============================================================================
struct DataBlockProducerImpl
{
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;
    std::vector<FlexibleZoneInfo> flexible_zones_info; // New member
};

// ============================================================================
// DataBlockProducer
// ============================================================================
DataBlockProducer::DataBlockProducer() : pImpl(nullptr) {}

DataBlockProducer::DataBlockProducer(std::unique_ptr<DataBlockProducerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockProducer::~DataBlockProducer() = default;

DataBlockProducer::DataBlockProducer(DataBlockProducer &&other) noexcept = default;

DataBlockProducer &DataBlockProducer::operator=(DataBlockProducer &&other) noexcept = default;

// ============================================================================
// DataBlockConsumerImpl
// ============================================================================
struct DataBlockConsumerImpl
{
    std::string name;
    std::unique_ptr<DataBlock> dataBlock;
    ChecksumPolicy checksum_policy = ChecksumPolicy::Enforced;
    uint64_t last_consumed_slot_id = INVALID_SLOT_ID;  // This field is still needed
    std::vector<FlexibleZoneInfo> flexible_zones_info; // New member
    int heartbeat_slot = -1; // For Sync_reader: index into consumer_heartbeats / read positions

    ~DataBlockConsumerImpl()
    {
        LOGGER_INFO("DataBlockConsumerImpl: Shutting down for '{}'.", name);
    }
};

// ============================================================================
// DataBlockDiagnosticHandle implementation
// ============================================================================
DataBlockDiagnosticHandle::DataBlockDiagnosticHandle(
    std::unique_ptr<DataBlockDiagnosticHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockDiagnosticHandle::~DataBlockDiagnosticHandle()
{
    if (pImpl)
        pylabhub::platform::shm_close(&pImpl->m_shm);
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
    if (!pImpl || !pImpl->slot_rw_states || index >= pImpl->ring_buffer_capacity)
        return nullptr;
    return &pImpl->slot_rw_states[index];
}

std::unique_ptr<DataBlockDiagnosticHandle> open_datablock_for_diagnostic(const std::string &name)
{
    auto impl = std::make_unique<DataBlockDiagnosticHandleImpl>();
    try
    {
        impl->m_shm = pylabhub::platform::shm_attach(name.c_str());
        if (impl->m_shm.base == nullptr)
            return nullptr;
        impl->header_ptr = reinterpret_cast<SharedMemoryHeader *>(impl->m_shm.base);
        if (!detail::is_header_magic_valid(&impl->header_ptr->magic_number,
                                            detail::DATABLOCK_MAGIC_NUMBER))
            return nullptr;
        impl->ring_buffer_capacity = impl->header_ptr->ring_buffer_capacity;
        DataBlockLayout layout = DataBlockLayout::from_header(impl->header_ptr);
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

bool DataBlockProducer::update_checksum_flexible_zone(size_t flexible_zone_idx)
{
    return pImpl && pImpl->dataBlock
               ? update_checksum_flexible_zone_impl(pImpl->dataBlock.get(), flexible_zone_idx)
               : false;
}

std::span<std::byte> DataBlockProducer::flexible_zone_span(size_t index)
{
    if (!pImpl || !pImpl->dataBlock || index >= pImpl->flexible_zones_info.size())
        return {};
    const auto &zone_info = pImpl->flexible_zones_info[index];
    char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();
    if (!flexible_zone_base || zone_info.size == 0)
        return {};
    return {reinterpret_cast<std::byte *>(flexible_zone_base + zone_info.offset), zone_info.size};
}

bool DataBlockProducer::update_checksum_slot(size_t slot_index)
{
    return pImpl && pImpl->dataBlock ? update_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
                                     : false;
}

std::unique_ptr<SlotWriteHandle> DataBlockProducer::acquire_write_slot(int timeout_ms)
{
    if (!pImpl || !pImpl->dataBlock)
        return nullptr;

    auto *h = pImpl->dataBlock->header();
    if (!h)
        return nullptr;

    uint32_t slot_count = h->ring_buffer_capacity;
    if (slot_count == 0)
        return nullptr;

    const ConsumerSyncPolicy policy = h->consumer_sync_policy;
    if (policy == ConsumerSyncPolicy::Single_reader || policy == ConsumerSyncPolicy::Sync_reader)
    {
        auto start_time = pylabhub::platform::monotonic_time_ns();
        int iteration = 0;
        while (true)
        {
            uint64_t w = h->write_index.load(std::memory_order_acquire);
            uint64_t r = h->read_index.load(std::memory_order_acquire);
            if (w - r < static_cast<uint64_t>(slot_count))
                break;
            if (timeout_ms > 0 &&
                pylabhub::platform::elapsed_time_ns(start_time) / 1'000'000 >=
                    static_cast<uint64_t>(timeout_ms))
            {
                h->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            backoff(iteration++);
        }
    }

    // Acquire a new slot ID (monotonically increasing)
    uint64_t slot_id = h->write_index.fetch_add(1, std::memory_order_acq_rel);
    size_t slot_index = static_cast<size_t>(slot_id % slot_count);

    // Get the SlotRWState for this slot
    SlotRWState *rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
    if (!rw_state)
    {
        return nullptr; // Should not happen, slot_rw_state throws on error
    }

    // Acquire write lock for this slot
    SlotAcquireResult acquire_res = acquire_write(rw_state, h, timeout_ms);
    if (acquire_res != SLOT_ACQUIRE_OK)
    {
        // Error already logged in acquire_write. Just return nullptr.
        return nullptr;
    }

    size_t slot_size = h->unit_block_size;
    char *buf = pImpl->dataBlock->structured_data_buffer();
    if (!buf || slot_size == 0)
    {
        // Release write lock if buffer is invalid before returning
        rw_state->write_lock.store(0, std::memory_order_release);
        return nullptr;
    }

    auto impl = std::make_unique<SlotWriteHandleImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->header = h;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = buf + slot_index * slot_size;
    impl->buffer_size = slot_size;
    impl->rw_state = rw_state; // New: Pointer to the SlotRWState for this slot
    // flexible_ptr and flexible_size are no longer directly used in SlotWriteHandleImpl
    return std::unique_ptr<SlotWriteHandle>(new SlotWriteHandle(std::move(impl)));
}

bool DataBlockProducer::release_write_slot(SlotWriteHandle &handle)
{
    if (!handle.pImpl)
        return false;
    return release_write_handle(*handle.pImpl);
}

void DataBlockProducer::check_consumer_health()
{
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header())
    {
        return;
    }
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
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header())
    {
        return false;
    }
    ProducerInfo info;
    info.shm_name = pImpl->name;
    info.producer_pid = pylabhub::platform::get_pid();
    info.schema_hash.assign(reinterpret_cast<const char *>(pImpl->dataBlock->header()->schema_hash),
                            32);
    info.schema_version = pImpl->dataBlock->header()->schema_version;
    return hub.register_producer(channel_name, info);
}

namespace
{
bool release_write_handle(SlotWriteHandleImpl &impl);
bool release_consume_handle(SlotConsumeHandleImpl &impl);
} // namespace

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

DataBlockSlotIterator::~DataBlockSlotIterator() = default;

DataBlockSlotIterator::DataBlockSlotIterator(DataBlockSlotIterator &&other) noexcept = default;

DataBlockSlotIterator &
DataBlockSlotIterator::operator=(DataBlockSlotIterator &&other) noexcept = default;

DataBlockSlotIterator::NextResult DataBlockSlotIterator::try_next(int timeout_ms)
{
    NextResult r;
    r.ok = false;
    r.error_code = 0;
    if (!pImpl || !pImpl->dataBlock)
    {
        r.error_code = 1;
        return r;
    }
    auto *h = pImpl->dataBlock->header();
    if (!h)
    {
        r.error_code = 1;
        return r;
    }

    const ConsumerSyncPolicy policy = h->consumer_sync_policy;
    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;
    uint64_t slot_id = INVALID_SLOT_ID;
    size_t slot_index = 0;
    SlotRWState *rw_state = nullptr;
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res;

    uint32_t slot_count = h->ring_buffer_capacity;
    if (slot_count == 0)
    {
        r.error_code = 1;
        return r;
    }

    const int heartbeat_slot =
        (pImpl->owner != nullptr) ? pImpl->owner->heartbeat_slot : -1;

    while (true)
    {
        const uint64_t next_to_read =
            get_next_slot_to_read(h, pImpl->last_seen_slot_id, heartbeat_slot);

        if (next_to_read != INVALID_SLOT_ID)
        {
            slot_index = static_cast<size_t>(next_to_read % slot_count);
            rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
            if (!rw_state)
            {
                r.error_code = 3;
                return r;
            }
            acquire_res = acquire_read(rw_state, h, &captured_generation);
            if (acquire_res == SLOT_ACQUIRE_OK)
            {
                slot_id = next_to_read;
                break;
            }
            if (acquire_res != SLOT_ACQUIRE_NOT_READY)
            {
                r.error_code = 3;
                return r;
            }
        }

        if (timeout_ms > 0 && pylabhub::platform::elapsed_time_ns(start_time) / 1'000'000 >=
                                  static_cast<uint64_t>(timeout_ms))
        {
            h->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            r.error_code = 2;
            return r;
        }
        backoff(iteration++);
    }

    auto impl = std::make_unique<SlotConsumeHandleImpl>();
    impl->owner = pImpl->owner;
    impl->dataBlock = pImpl->dataBlock;
    impl->header = h;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = pImpl->dataBlock->structured_data_buffer() + slot_index * h->unit_block_size;
    impl->buffer_size = h->unit_block_size;
    impl->rw_state = rw_state;
    impl->captured_generation = captured_generation;
    if (policy == ConsumerSyncPolicy::Sync_reader && pImpl->owner)
        impl->consumer_heartbeat_slot = pImpl->owner->heartbeat_slot;

    r.next = SlotConsumeHandle(std::move(impl));
    r.ok = true;
    pImpl->last_seen_slot_id = slot_id;
    return r;
}

SlotConsumeHandle DataBlockSlotIterator::next(int timeout_ms)
{
    auto res = try_next(timeout_ms);
    if (!res.ok)
    {
        throw std::runtime_error("DataBlockSlotIterator::next: slot not available (error " +
                                 std::to_string(res.error_code) + ")");
    }
    return std::move(res.next);
}

void DataBlockSlotIterator::seek_latest()
{
    if (!pImpl || !pImpl->dataBlock)
        return;
    auto *h = pImpl->dataBlock->header();
    if (!h)
        return;
    pImpl->last_seen_slot_id = h->commit_index.load(std::memory_order_acquire);
}

void DataBlockSlotIterator::seek_to(uint64_t slot_id)
{
    if (!pImpl)
        return;
    pImpl->last_seen_slot_id = slot_id;
}

uint64_t DataBlockSlotIterator::last_slot_id() const
{
    return pImpl ? pImpl->last_seen_slot_id : INVALID_SLOT_ID;
}

bool DataBlockSlotIterator::is_valid() const
{
    return pImpl && pImpl->dataBlock;
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
        return true;
    bool ok = true;

    // Perform checksum updates if policy requires and committed
    if (impl.committed && impl.owner && impl.owner->checksum_policy != ChecksumPolicy::None &&
        impl.header && impl.header->enable_checksum)
    {
        // Update slot checksum
        if (!update_checksum_slot_impl(impl.dataBlock, impl.slot_index))
        {
            ok = false;
        }

        // Update flexible zone checksums
        for (size_t i = 0; i < impl.dataBlock->flexible_zone_info().size(); ++i)
        {
            if (!update_checksum_flexible_zone_impl(impl.dataBlock, i))
            { // pass flexible zone index
                ok = false;
                break;
            }
        }
    }

    // Commit the write (make it visible to readers)
    if (impl.committed && impl.rw_state && impl.header)
    {
        commit_write(impl.rw_state, impl.header);
        // Release write_lock so the slot can be reused on wrap-around (lap2+)
        impl.rw_state->write_lock.store(0, std::memory_order_release);
    }
    else if (impl.rw_state)
    {
        // If not committed, simply release the write lock
        impl.rw_state->write_lock.store(0, std::memory_order_release);
        impl.rw_state->slot_state.store(SlotRWState::SlotState::FREE, std::memory_order_release);
    }

    // No thread guard exit needed here, as acquire_write doesn't use it.
    impl.released = true;
    return ok;
}

bool release_consume_handle(SlotConsumeHandleImpl &impl)
{
    if (impl.released)
        return true;
    bool ok = true;

    // 1. Validate captured generation to detect wrap-around (if reader was pre-empted)
    if (impl.rw_state && impl.header)
    {
        if (!validate_read(impl.rw_state, impl.header, impl.captured_generation))
        {
            ok = false; // Validation failed (slot overwritten or corrupted)
        }
    }
    else
    {
        ok = false; // Invalid state
    }

    // 2. Perform checksum verification if policy requires
    if (ok && impl.owner && impl.owner->checksum_policy != ChecksumPolicy::None && impl.header &&
        impl.header->enable_checksum)
    {
        if (!verify_checksum_slot_impl(impl.dataBlock, impl.slot_index))
        {
            ok = false;
        }
        for (size_t i = 0; i < impl.dataBlock->flexible_zone_info().size(); ++i)
        {
            if (!verify_checksum_flexible_zone_impl(impl.dataBlock, i))
            {
                ok = false;
                break;
            }
        }
    }

    // 3. Release the read lock
    if (impl.rw_state && impl.header)
    {
        release_read(impl.rw_state, impl.header);
    }
    else
    {
        ok = false; // Cannot release if state is invalid
    }

    // 4. Advance read position for Single_reader / Sync_reader
    if (ok && impl.header)
    {
        SharedMemoryHeader *h = impl.header;
        const ConsumerSyncPolicy policy = h->consumer_sync_policy;
        const uint64_t next = impl.slot_id + 1;
        if (policy == ConsumerSyncPolicy::Single_reader)
        {
            h->read_index.store(next, std::memory_order_release);
        }
        else if (policy == ConsumerSyncPolicy::Sync_reader && impl.consumer_heartbeat_slot >= 0 &&
                 impl.consumer_heartbeat_slot < static_cast<int>(MAX_CONSUMER_HEARTBEATS))
        {
            consumer_next_read_slot_ptr(h, static_cast<size_t>(impl.consumer_heartbeat_slot))
                ->store(next, std::memory_order_release);
            // read_index = min of all consumer positions (only count registered slots)
            uint64_t min_pos = next;
            for (size_t i = 0; i < MAX_CONSUMER_HEARTBEATS; ++i)
            {
                if (h->consumer_heartbeats[i].consumer_id.load(std::memory_order_acquire) != 0)
                {
                    uint64_t p =
                        consumer_next_read_slot_ptr(h, i)->load(std::memory_order_acquire);
                    if (p < min_pos)
                        min_pos = p;
                }
            }
            h->read_index.store(min_pos, std::memory_order_release);
        }
    }

    impl.released = true;
    return ok;
}
} // namespace

SlotWriteHandle::SlotWriteHandle() : pImpl(nullptr) {}

SlotWriteHandle::SlotWriteHandle(std::unique_ptr<SlotWriteHandleImpl> impl) : pImpl(std::move(impl))
{
}

SlotWriteHandle::~SlotWriteHandle()
{
    if (pImpl)
    {
        (void)release_write_handle(*pImpl);
    }
}

SlotWriteHandle::SlotWriteHandle(SlotWriteHandle &&other) noexcept = default;

SlotWriteHandle &SlotWriteHandle::operator=(SlotWriteHandle &&other) noexcept = default;

size_t SlotWriteHandle::slot_index() const
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotWriteHandle::slot_id() const
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<std::byte> SlotWriteHandle::buffer_span()
{
    if (!pImpl || !pImpl->buffer_ptr || pImpl->buffer_size == 0)
        return {};
    return {reinterpret_cast<std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<std::byte> SlotWriteHandle::flexible_zone_span(size_t index)
{
    if (!pImpl || !pImpl->owner || !pImpl->dataBlock ||
        index >= pImpl->owner->flexible_zones_info.size())
        return {};

    const auto &zone_info = pImpl->owner->flexible_zones_info[index];
    char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();

    if (!flexible_zone_base || zone_info.size == 0)
        return {};

    return {reinterpret_cast<std::byte *>(flexible_zone_base + zone_info.offset), zone_info.size};
}

bool SlotWriteHandle::write(const void *src, size_t len, size_t offset)
{
    if (!pImpl || !pImpl->buffer_ptr || len == 0)
        return false;
    if (offset + len > pImpl->buffer_size)
        return false;
    std::memcpy(pImpl->buffer_ptr + offset, src, len);
    return true;
}

bool SlotWriteHandle::commit(size_t bytes_written)
{
    if (!pImpl || !pImpl->header)
        return false;
    if (bytes_written > pImpl->buffer_size)
        return false;
    pImpl->bytes_written = bytes_written;
    pImpl->committed = true;

    if (pImpl->owner && pImpl->owner->checksum_policy == ChecksumPolicy::Manual)
    {
        pImpl->header->commit_index.store(pImpl->slot_id, std::memory_order_release);
    }
    return true;
}

bool SlotWriteHandle::update_checksum_slot()
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return update_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotWriteHandle::update_checksum_flexible_zone(size_t index)
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return update_checksum_flexible_zone_impl(pImpl->dataBlock, index);
}

SlotConsumeHandle::SlotConsumeHandle() : pImpl(nullptr) {}

SlotConsumeHandle::SlotConsumeHandle(std::unique_ptr<SlotConsumeHandleImpl> impl)
    : pImpl(std::move(impl))
{
}

SlotConsumeHandle::~SlotConsumeHandle()
{
    if (pImpl)
    {
        (void)release_consume_handle(*pImpl);
    }
}

SlotConsumeHandle::SlotConsumeHandle(SlotConsumeHandle &&other) noexcept = default;

SlotConsumeHandle &SlotConsumeHandle::operator=(SlotConsumeHandle &&other) noexcept = default;

size_t SlotConsumeHandle::slot_index() const
{
    return pImpl ? pImpl->slot_index : 0;
}

uint64_t SlotConsumeHandle::slot_id() const
{
    return pImpl ? pImpl->slot_id : 0;
}

std::span<const std::byte> SlotConsumeHandle::buffer_span() const
{
    if (!pImpl || !pImpl->buffer_ptr || pImpl->buffer_size == 0)
        return {};
    return {reinterpret_cast<const std::byte *>(pImpl->buffer_ptr), pImpl->buffer_size};
}

std::span<const std::byte> SlotConsumeHandle::flexible_zone_span(size_t index) const
{
    if (!pImpl || !pImpl->owner || !pImpl->dataBlock ||
        index >= pImpl->owner->flexible_zones_info.size())
        return {};

    const auto &zone_info = pImpl->owner->flexible_zones_info[index];
    const char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();

    if (!flexible_zone_base || zone_info.size == 0)
        return {};

    return {reinterpret_cast<const std::byte *>(flexible_zone_base + zone_info.offset),
            zone_info.size};
}

bool SlotConsumeHandle::read(void *dst, size_t len, size_t offset) const
{
    if (!pImpl || !pImpl->buffer_ptr || len == 0)
        return false;
    if (offset + len > pImpl->buffer_size)
        return false;
    std::memcpy(dst, pImpl->buffer_ptr + offset, len);
    return true;
}

bool SlotConsumeHandle::verify_checksum_slot() const
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return verify_checksum_slot_impl(pImpl->dataBlock, pImpl->slot_index);
}

bool SlotConsumeHandle::verify_checksum_flexible_zone(size_t index) const
{
    if (!pImpl || !pImpl->dataBlock)
        return false;
    return verify_checksum_flexible_zone_impl(pImpl->dataBlock, index);
}

// ============================================================================
// DataBlockConsumer
// ============================================================================
DataBlockConsumer::DataBlockConsumer() : pImpl(nullptr) {}

DataBlockConsumer::DataBlockConsumer(std::unique_ptr<DataBlockConsumerImpl> impl)
    : pImpl(std::move(impl))
{
}

DataBlockConsumer::~DataBlockConsumer() = default;

DataBlockConsumer::DataBlockConsumer(DataBlockConsumer &&other) noexcept = default;

DataBlockConsumer &DataBlockConsumer::operator=(DataBlockConsumer &&other) noexcept = default;

bool DataBlockConsumer::verify_checksum_flexible_zone(size_t flexible_zone_idx) const
{
    return pImpl && pImpl->dataBlock
               ? verify_checksum_flexible_zone_impl(pImpl->dataBlock.get(), flexible_zone_idx)
               : false;
}

std::span<const std::byte> DataBlockConsumer::flexible_zone_span(size_t index) const
{
    if (!pImpl || !pImpl->dataBlock || index >= pImpl->flexible_zones_info.size())
        return {};
    const auto &zone_info = pImpl->flexible_zones_info[index];
    const char *flexible_zone_base = pImpl->dataBlock->flexible_data_zone();
    if (!flexible_zone_base || zone_info.size == 0)
        return {};
    return {reinterpret_cast<const std::byte *>(flexible_zone_base + zone_info.offset),
            zone_info.size};
}

bool DataBlockConsumer::verify_checksum_slot(size_t slot_index) const
{
    return pImpl && pImpl->dataBlock ? verify_checksum_slot_impl(pImpl->dataBlock.get(), slot_index)
                                     : false;
}

std::unique_ptr<SlotConsumeHandle> DataBlockConsumer::acquire_consume_slot(int timeout_ms)
{
    if (!pImpl || !pImpl->dataBlock)
        return nullptr;

    auto *h = pImpl->dataBlock->header();
    if (!h)
        return nullptr;

    uint32_t slot_count = h->ring_buffer_capacity;
    if (slot_count == 0)
        return nullptr;

    const ConsumerSyncPolicy policy = h->consumer_sync_policy;
    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;
    uint64_t slot_id = INVALID_SLOT_ID;
    size_t slot_index = 0;
    SlotRWState *rw_state = nullptr;
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res;

    // Sync_reader: ensure registered and join-at-latest for new consumer
    if (policy == ConsumerSyncPolicy::Sync_reader)
    {
        if (pImpl->heartbeat_slot < 0)
        {
            if (register_heartbeat() < 0)
                return nullptr;
            // Join at latest: start reading from current commit_index
            uint64_t join_at = h->commit_index.load(std::memory_order_acquire);
            if (join_at != INVALID_SLOT_ID)
                consumer_next_read_slot_ptr(h, static_cast<size_t>(pImpl->heartbeat_slot))
                    ->store(join_at, std::memory_order_release);
            else
                consumer_next_read_slot_ptr(h, static_cast<size_t>(pImpl->heartbeat_slot))
                    ->store(0, std::memory_order_release);
        }
    }

    while (true)
    {
        const uint64_t next_to_read =
            get_next_slot_to_read(h, pImpl->last_consumed_slot_id, pImpl->heartbeat_slot);

        if (next_to_read != INVALID_SLOT_ID)
        {
            slot_index = static_cast<size_t>(next_to_read % slot_count);
            rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
            if (!rw_state)
                return nullptr;
            acquire_res = acquire_read(rw_state, h, &captured_generation);
            if (acquire_res == SLOT_ACQUIRE_OK)
            {
                slot_id = next_to_read;
                break;
            }
            if (acquire_res != SLOT_ACQUIRE_NOT_READY)
                return nullptr;
        }

        if (timeout_ms > 0 && pylabhub::platform::elapsed_time_ns(start_time) / 1'000'000 >=
                                  static_cast<uint64_t>(timeout_ms))
        {
            h->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        backoff(iteration++);
    }

    size_t slot_size = h->unit_block_size;
    const char *buf = pImpl->dataBlock->structured_data_buffer();
    if (!buf || slot_size == 0)
    {
        release_read(rw_state, h);
        return nullptr;
    }

    auto impl = std::make_unique<SlotConsumeHandleImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->header = h;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = buf + slot_index * slot_size;
    impl->buffer_size = slot_size;
    impl->rw_state = rw_state;
    impl->captured_generation = captured_generation;
    if (policy == ConsumerSyncPolicy::Sync_reader)
        impl->consumer_heartbeat_slot = pImpl->heartbeat_slot;

    pImpl->last_consumed_slot_id = slot_id;
    return std::unique_ptr<SlotConsumeHandle>(new SlotConsumeHandle(std::move(impl)));
}

std::unique_ptr<SlotConsumeHandle> DataBlockConsumer::acquire_consume_slot(uint64_t slot_id,
                                                                           int timeout_ms)
{
    if (!pImpl || !pImpl->dataBlock)
        return nullptr;

    auto *h = pImpl->dataBlock->header();
    if (!h)
        return nullptr;

    uint32_t slot_count = h->ring_buffer_capacity;
    if (slot_count == 0)
        return nullptr;

    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;

    // Wait until this slot_id is committed
    while (h->commit_index.load(std::memory_order_acquire) < slot_id)
    {
        if (timeout_ms > 0 && pylabhub::platform::elapsed_time_ns(start_time) / 1'000'000 >=
                                  static_cast<uint64_t>(timeout_ms))
        {
            h->reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        backoff(iteration++);
    }

    size_t slot_index = static_cast<size_t>(slot_id % slot_count);
    SlotRWState *rw_state = pImpl->dataBlock->slot_rw_state(slot_index);
    uint64_t captured_generation = 0;
    SlotAcquireResult acquire_res = acquire_read(rw_state, h, &captured_generation);
    if (acquire_res != SLOT_ACQUIRE_OK)
        return nullptr;

    size_t slot_size = h->unit_block_size;
    const char *buf = pImpl->dataBlock->structured_data_buffer();
    if (!buf || slot_size == 0)
    {
        release_read(rw_state, h);
        return nullptr;
    }

    auto impl = std::make_unique<SlotConsumeHandleImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->header = h;
    impl->slot_id = slot_id;
    impl->slot_index = slot_index;
    impl->buffer_ptr = buf + slot_index * slot_size;
    impl->buffer_size = slot_size;
    impl->rw_state = rw_state;
    impl->captured_generation = captured_generation;
    if (h->consumer_sync_policy == ConsumerSyncPolicy::Sync_reader)
        impl->consumer_heartbeat_slot = pImpl->heartbeat_slot;

    pImpl->last_consumed_slot_id = slot_id;
    return std::unique_ptr<SlotConsumeHandle>(new SlotConsumeHandle(std::move(impl)));
}

bool DataBlockConsumer::release_consume_slot(SlotConsumeHandle &handle)
{
    if (!handle.pImpl)
        return false;
    return release_consume_handle(*handle.pImpl);
}

DataBlockSlotIterator DataBlockConsumer::slot_iterator()
{
    if (!pImpl || !pImpl->dataBlock)
    {
        return DataBlockSlotIterator();
    }
    auto impl = std::make_unique<DataBlockSlotIteratorImpl>();
    impl->owner = pImpl.get();
    impl->dataBlock = pImpl->dataBlock.get();
    impl->last_seen_slot_id = INVALID_SLOT_ID;
    return DataBlockSlotIterator(std::move(impl));
}

int DataBlockConsumer::register_heartbeat()
{
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header())
    {
        return -1;
    }
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
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header() || slot < 0 ||
        slot >= static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
    {
        return;
    }
    auto *header = pImpl->dataBlock->header();
    header->consumer_heartbeats[slot].last_heartbeat_ns.store(
        pylabhub::platform::monotonic_time_ns(), std::memory_order_release);
}

void DataBlockConsumer::unregister_heartbeat(int slot)
{
    if (!pImpl || !pImpl->dataBlock || !pImpl->dataBlock->header() || slot < 0 ||
        slot >= static_cast<int>(detail::MAX_CONSUMER_HEARTBEATS))
    {
        return;
    }
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

// ============================================================================
// SharedMemoryHeader schema (layout + protocol check)
// ============================================================================

pylabhub::schema::SchemaInfo get_shared_memory_header_schema_info()
{
    using pylabhub::schema::BLDSBuilder;
    using pylabhub::schema::SchemaInfo;
    using pylabhub::schema::SchemaVersion;

    BLDSBuilder b;
    // All members with offset and size so hash reflects ABI layout
#define ADD(member, type_id)                                                                       \
    b.add_member(#member, type_id, offsetof(SharedMemoryHeader, member),                           \
                 sizeof(SharedMemoryHeader::member))

    ADD(magic_number, "u32");
    ADD(version_major, "u16");
    ADD(version_minor, "u16");
    ADD(total_block_size, "u64");
    ADD(shared_secret, "u8[64]");
    ADD(schema_hash, "u8[32]");
    ADD(schema_version, "u32");
    ADD(padding_sec, "u8[28]");
    ADD(policy, "u32");
    ADD(consumer_sync_policy, "u32");
    ADD(unit_block_size, "u32");
    ADD(ring_buffer_capacity, "u32");
    ADD(flexible_zone_size, "u64");
    ADD(enable_checksum, "b");
    ADD(checksum_policy, "u32");
    ADD(write_index, "u64");
    ADD(commit_index, "u64");
    ADD(read_index, "u64");
    ADD(active_consumer_count, "u32");
    ADD(writer_timeout_count, "u64");
    ADD(writer_blocked_total_ns, "u64");
    ADD(write_lock_contention, "u64");
    ADD(write_generation_wraps, "u64");
    ADD(reader_not_ready_count, "u64");
    ADD(reader_race_detected, "u64");
    ADD(reader_validation_failed, "u64");
    ADD(reader_peak_count, "u64");
    ADD(reader_timeout_count, "u64");
    ADD(last_error_timestamp_ns, "u64");
    ADD(last_error_code, "u32");
    ADD(error_sequence, "u32");
    ADD(slot_acquire_errors, "u64");
    ADD(slot_commit_errors, "u64");
    ADD(checksum_failures, "u64");
    ADD(zmq_send_failures, "u64");
    ADD(zmq_recv_failures, "u64");
    ADD(zmq_timeout_count, "u64");
    ADD(recovery_actions_count, "u64");
    ADD(schema_mismatch_count, "u64");
    ADD(reserved_errors, "u64[2]");
    ADD(heartbeat_sent_count, "u64");
    ADD(heartbeat_failed_count, "u64");
    ADD(last_heartbeat_ns, "u64");
    ADD(reserved_hb, "u64");
    ADD(total_slots_written, "u64");
    ADD(total_slots_read, "u64");
    ADD(total_bytes_written, "u64");
    ADD(total_bytes_read, "u64");
    ADD(uptime_seconds, "u64");
    ADD(creation_timestamp_ns, "u64");
    ADD(reserved_perf, "u64[2]");
    ADD(consumer_heartbeats,
        "ConsumerHeartbeat[" + std::to_string(detail::MAX_CONSUMER_HEARTBEATS) + "]");
    ADD(spinlock_states,
        "SharedSpinLockState[" + std::to_string(detail::MAX_SHARED_SPINLOCKS) + "]");
    ADD(flexible_zone_checksums,
        "FlexibleZoneChecksumEntry[" + std::to_string(detail::MAX_FLEXIBLE_ZONE_CHECKSUMS) + "]");
    ADD(reserved_header, "u8[" + std::to_string(sizeof(SharedMemoryHeader::reserved_header)) + "]");

#undef ADD

    SchemaInfo info;
    info.name = "pylabhub.hub.SharedMemoryHeader";
    info.version = SchemaVersion{detail::HEADER_VERSION_MAJOR, detail::HEADER_VERSION_MINOR, 0};
    info.struct_size = sizeof(SharedMemoryHeader);
    info.blds = b.build();
    info.compute_hash();
    return info;
}

void validate_header_layout_hash(const SharedMemoryHeader *header)
{
    if (!header)
    {
        throw std::invalid_argument("validate_header_layout_hash: header is null");
    }
    pylabhub::schema::SchemaInfo expected = get_shared_memory_header_schema_info();
    const uint8_t *stored = header->reserved_header + detail::HEADER_LAYOUT_HASH_OFFSET;
    if (std::memcmp(expected.hash.data(), stored, detail::HEADER_LAYOUT_HASH_SIZE) != 0)
    {
        std::array<uint8_t, 32> actual_hash;
        std::memcpy(actual_hash.data(), stored, detail::HEADER_LAYOUT_HASH_SIZE);
        throw pylabhub::schema::SchemaValidationException(
            "SharedMemoryHeader layout mismatch: producer and consumer have different ABI "
            "(offset/size).",
            expected.hash, actual_hash);
    }
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
    (void)policy; // Reserved for future policy-specific behavior
    auto impl = std::make_unique<DataBlockProducerImpl>();
    impl->name = name;
    impl->dataBlock = std::make_unique<DataBlock>(name, config);
    impl->checksum_policy = config.checksum_policy;
    impl->flexible_zones_info = build_flexible_zone_info(config.flexible_zone_configs);

    auto *header = impl->dataBlock->header();
    if (header && schema_info)
    {
        // Store schema hash (full 32 bytes)
        std::memcpy(header->schema_hash, schema_info->hash.data(), 32);

        // Store packed schema version
        header->schema_version = schema_info->version.pack();

        LOGGER_DEBUG("[DataBlock:{}] Schema stored: {} v{}, hash={}...", name, schema_info->name,
                     schema_info->version.to_string(),
                     fmt::format("{:02x}{:02x}{:02x}{:02x}", schema_info->hash[0],
                                 schema_info->hash[1], schema_info->hash[2], schema_info->hash[3]));
    }
    else if (header)
    {
        // No schema validation - zero out schema fields
        std::memset(header->schema_hash, 0, 32);
        header->schema_version = 0;
    }

    ProducerInfo pinfo;
    pinfo.shm_name = name;
    pinfo.producer_pid = pylabhub::platform::get_pid();
    pinfo.schema_hash.assign(reinterpret_cast<const char *>(header->schema_hash), 32);
    pinfo.schema_version = header->schema_version;
    hub.register_producer(name, pinfo);
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
    auto impl = std::make_unique<DataBlockConsumerImpl>();
    impl->name = name;
    impl->dataBlock = std::make_unique<DataBlock>(name);

    auto *header = impl->dataBlock->header();
    if (!header)
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

    // Validate config if provided
    if (expected_config)
    {
        if (header->flexible_zone_size != expected_config->total_flexible_zone_size() ||
            header->ring_buffer_capacity != expected_config->ring_buffer_capacity ||
            header->unit_block_size != static_cast<uint32_t>(expected_config->unit_block_size) ||
            header->enable_checksum != expected_config->enable_checksum)
        {
            LOGGER_WARN("[DataBlock:{}] Config mismatch during consumer attachment", name);
            return nullptr;
        }
        const auto &configs = expected_config->flexible_zone_configs;
        impl->flexible_zones_info = build_flexible_zone_info(configs);
        impl->dataBlock->set_flexible_zone_info_for_attach(configs);
    }

    // Validate schema if provided
    if (schema_info)
    {
        // Check if producer stored a schema (non-zero hash)
        bool has_producer_schema = false;
        for (size_t i = 0; i < 32; ++i)
        {
            if (header->schema_hash[i] != 0)
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
        if (std::memcmp(header->schema_hash, schema_info->hash.data(), 32) != 0)
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
    ConsumerInfo cinfo;
    cinfo.shm_name = name;
    cinfo.schema_hash.assign(reinterpret_cast<const char *>(header->schema_hash), 32);
    cinfo.schema_version = header->schema_version;
    hub.register_consumer(name, cinfo);
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
        // The guard only ensures the slot is released.
        // We do not implicitly commit here.
        producer_->release_write_slot(*slot_);
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
            producer_->release_write_slot(*slot_);
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
        return std::nullopt;
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
        consumer_->release_consume_slot(*slot_);
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
            consumer_->release_consume_slot(*slot_);
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
        return std::nullopt;
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
        if (!rw_state)
            return SLOT_ACQUIRE_ERROR;
        return acquire_write(rw_state, nullptr, timeout_ms);
    }

    void slot_rw_commit(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state)
            commit_write(rw_state, nullptr);
    }

    void slot_rw_release_write(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state)
            release_write(rw_state, nullptr);
    }

    SlotAcquireResult slot_rw_acquire_read(pylabhub::hub::SlotRWState *rw_state,
                                           uint64_t *out_generation)
    {
        if (!rw_state || !out_generation)
            return SLOT_ACQUIRE_ERROR;
        return acquire_read(rw_state, nullptr, out_generation);
    }

    bool slot_rw_validate_read(pylabhub::hub::SlotRWState *rw_state, uint64_t generation)
    {
        if (!rw_state)
            return false;
        return validate_read(rw_state, nullptr, generation);
    }

    void slot_rw_release_read(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state)
            release_read(rw_state, nullptr);
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
        if (!shared_memory_header || !out_metrics)
            return -1;
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

    int slot_rw_reset_metrics(pylabhub::hub::SharedMemoryHeader *shared_memory_header)
    {
        if (!shared_memory_header)
            return -1;
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
