// data_block_internal.hpp
//
// PRIVATE header — include ONLY from data_block*.cpp implementation files in this directory.
// Not part of the public API; not installed.
//
// Contains:
//   1. Common #includes for all data_block*.cpp translation units.
//   2. namespace detail  — inline metric/heartbeat helpers (private to the shm layer).
//   3. namespace internal — promoted constants + inline helpers from the former anonymous
//                           namespace, plus forward declarations of the six slot coordination
//                           functions defined in data_block_slot_ops.cpp.
//
// The six coordination functions (acquire_write, commit_write, release_write,
// acquire_read, validate_read_impl, release_read) are declared in namespace pylabhub::hub
// (not in internal::) so they are accessible without qualification inside any .cpp that
// opens namespace pylabhub::hub.

#pragma once

#include "plh_platform.hpp"
#include "utils/backoff_strategy.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/data_block.hpp"
#include "utils/messenger.hpp"
#include "utils/deterministic_checksum.hpp"
#include "utils/data_block_mutex.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pylabhub::hub
{

// ============================================================================
// namespace detail — inline helpers (private, shm layer only)
// ============================================================================
namespace detail
{
/// Memory layout constants (Phase 2 refactoring)
/// OS mmap alignment boundary used throughout the DataBlock memory layout.
/// This is the minimum granularity for mmap allocations on all supported platforms (x86-64, ARM64).
/// It is distinct from DataBlockConfig::physical_page_size, which is the per-slot stride
/// (the DataBlock "page" size — 4K, 4M, or 16M slots). PAGE_ALIGNMENT is the OS alignment
/// requirement for section boundaries (data region start, flex zone, ring buffer).
/// Per HEP-CORE-0002 §3 (Memory Layout and Data Structures):
/// - Data region start must be PAGE_ALIGNMENT-aligned
/// - Flex zone size is rounded up to a PAGE_ALIGNMENT boundary
/// - All major memory sections align on PAGE_ALIGNMENT boundaries
inline constexpr size_t PAGE_ALIGNMENT = 4096;

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

// ============================================================================
// Centralized Header Access Functions (Phase 1)
// ============================================================================
// All header field access must go through these functions to ensure
// consistency, validation, and correct memory ordering.

// === Metrics Access Functions ===

inline void increment_metric_writer_timeout(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void increment_metric_writer_lock_timeout(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->writer_lock_timeout_count.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void increment_metric_writer_reader_timeout(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->writer_reader_timeout_count.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void increment_metric_write_lock_contention(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->write_lock_contention.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void increment_metric_reader_race_detected(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->reader_race_detected.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void increment_metric_reader_validation_failed(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->reader_validation_failed.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void increment_metric_slot_acquire_errors(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->slot_acquire_errors.fetch_add(1, std::memory_order_relaxed);
    }
}

/**
 * Increment total commit count. Called on every successful slot commit.
 * Memory ordering: release (synchronizes with readers checking commit progress).
 */
inline void increment_metric_total_commits(SharedMemoryHeader* header) noexcept {
    if (header != nullptr) {
        header->total_slots_written.fetch_add(1, std::memory_order_release);
    }
}

/**
 * Get total number of commits (slots written and committed).
 * Memory ordering: acquire (sees all writes up to last commit).
 *
 * @return Total commits, or 0 if header is null.
 */
inline uint64_t get_total_commits(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->total_slots_written.load(std::memory_order_acquire) : 0;
}

/**
 * Check if any commits have been made.
 *
 * @return true if at least one commit has been made, false otherwise.
 */
inline bool has_any_commits(const SharedMemoryHeader* header) noexcept {
    return get_total_commits(header) > 0;
}

/**
 * Update peak reader count metric if current count exceeds stored peak.
 */
inline void update_reader_peak_count(SharedMemoryHeader* header, uint32_t current_count) noexcept {
    if (header == nullptr) {
        return;
    }
    uint64_t peak = header->reader_peak_count.load(std::memory_order_relaxed);
    while (current_count > peak &&
           !header->reader_peak_count.compare_exchange_weak(peak, current_count,
                                                            std::memory_order_relaxed,
                                                            std::memory_order_relaxed))
    {
    }
}

// === Index Access Functions ===

/**
 * Get the current commit index (last committed slot ID).
 * Memory ordering: acquire.
 */
inline uint64_t get_commit_index(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->commit_index.load(std::memory_order_acquire) : std::numeric_limits<uint64_t>::max();
}

/**
 * Update commit index to the given slot_id.
 * Memory ordering: release.
 */
inline void update_commit_index(SharedMemoryHeader* header, uint64_t slot_id) noexcept {
    if (header != nullptr) {
        header->commit_index.store(slot_id, std::memory_order_release);
    }
}

/**
 * Get the current write index.
 */
inline uint64_t get_write_index(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->write_index.load(std::memory_order_acquire) : 0;
}

/**
 * Get the current read index (for Single_reader policy).
 */
inline uint64_t get_read_index(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->read_index.load(std::memory_order_acquire) : 0;
}

// === Config Access Functions (read-only) ===

inline DataBlockPolicy get_policy(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->policy : DataBlockPolicy::Unset;
}

inline ConsumerSyncPolicy get_consumer_sync_policy(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->consumer_sync_policy : ConsumerSyncPolicy::Unset;
}

inline uint32_t get_ring_buffer_capacity(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->ring_buffer_capacity : 0;
}

inline uint32_t get_physical_page_size(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->physical_page_size : 0;
}

inline uint32_t get_logical_unit_size(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->logical_unit_size : 0;
}

inline ChecksumType get_checksum_type(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? static_cast<ChecksumType>(header->checksum_type) : ChecksumType::Unset;
}

inline ChecksumPolicy get_checksum_policy(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? header->checksum_policy : ChecksumPolicy::None;
}

// End of Centralized Access Functions
// ============================================================================


/// Returns true if producer heartbeat exists for this pid and is fresh (within threshold).
inline bool is_producer_heartbeat_fresh(const SharedMemoryHeader *header, uint64_t pid)
{
    if (header == nullptr || pid == 0) {
        return false;
    }
    auto *mut_header = const_cast<SharedMemoryHeader *>(header);
    uint64_t stored_id = producer_heartbeat_id_ptr(mut_header)->load(std::memory_order_acquire);
    if (stored_id != pid) {
        return false;
    }
    uint64_t stored_ns = producer_heartbeat_ns_ptr(mut_header)->load(std::memory_order_acquire);
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
// namespace internal — promoted constants + inline helpers
// ============================================================================
// These were previously in an anonymous namespace in data_block.cpp.
// Extracted here so data_block_slot_ops.cpp can share them without repeating them.
// Consumers: bring into scope with `using namespace internal;` inside the enclosing
// `namespace pylabhub::hub { ... }` block.
namespace internal
{
using pylabhub::utils::backoff;

// Import version constants from public header
using pylabhub::hub::detail::HEADER_VERSION_MAJOR;
using pylabhub::hub::detail::HEADER_VERSION_MINOR;
using pylabhub::hub::detail::MAX_CONSUMER_HEARTBEATS;
using pylabhub::hub::detail::MAX_SHARED_SPINLOCKS;
using pylabhub::hub::detail::CONSUMER_READ_POSITIONS_OFFSET;
using pylabhub::hub::detail::PRODUCER_HEARTBEAT_OFFSET;
using pylabhub::hub::detail::PRODUCER_HEARTBEAT_STALE_THRESHOLD_NS;

constexpr uint16_t DATABLOCK_VERSION_MAJOR = HEADER_VERSION_MAJOR;
constexpr uint16_t DATABLOCK_VERSION_MINOR = HEADER_VERSION_MINOR;
constexpr uint64_t INVALID_SLOT_ID = std::numeric_limits<uint64_t>::max();

/// Sync_reader: pointer to the slot_index-th consumer's next-read slot id in reserved_header.
inline std::atomic<uint64_t> *consumer_next_read_slot_ptr(SharedMemoryHeader *header,
                                                          size_t slot_index)
{
    return reinterpret_cast<std::atomic<uint64_t> *>(header->reserved_header +
                                                    CONSUMER_READ_POSITIONS_OFFSET) +
           slot_index;
}

constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000ULL;

/// Returns true if elapsed time since start_time_ns has exceeded timeout_ms.
/// Convention (standard, matches POSIX/WinAPI):
///   timeout_ms =  0 : non-blocking — always returns true immediately
///   timeout_ms = -1 : no timeout   — always returns false (spin forever)
///   timeout_ms >  0 : wait up to N ms
inline bool spin_elapsed_ms_exceeded(uint64_t start_time_ns, int timeout_ms)
{
    if (timeout_ms == 0) {
        return true; // non-blocking: expire immediately on first miss
    }
    if (timeout_ms < 0) {
        return false; // infinite: never expire
    }
    const uint64_t timeout_ns = static_cast<uint64_t>(timeout_ms) * kNanosecondsPerMillisecond;
    return pylabhub::platform::elapsed_time_ns(start_time_ns) >= timeout_ns;
}

/// Returns the SHM attach timeout in milliseconds (init handshake only, not runtime ops).
/// Default: 5000ms. Override via PYLABHUB_DATABLOCK_ATTACH_TIMEOUT_MS env var.
inline int get_attach_timeout_ms() noexcept
{
    const char *env = std::getenv("PYLABHUB_DATABLOCK_ATTACH_TIMEOUT_MS");
    if (env)
    {
        try { return std::stoi(env); } catch (...) {}
    }
    return 5000;
}

/// Waits for the SHM header magic number to become valid (creator finished initializing).
/// @return true if magic is valid within timeout_ms, false on timeout.
inline bool wait_for_header_magic_valid(SharedMemoryHeader *header, int timeout_ms)
{
    auto start_time = pylabhub::platform::monotonic_time_ns();
    int iteration = 0;
    while (true)
    {
        if (detail::is_header_magic_valid(&header->magic_number, detail::DATABLOCK_MAGIC_NUMBER))
            return true;
        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
            return false;
        backoff(iteration++);
    }
}

/// Slot buffer pointer: base + slot_index * slot_stride_bytes (logical stride).
inline char *slot_buffer_ptr(char *base, size_t slot_index, size_t slot_stride_bytes)
{
    return base + slot_index * slot_stride_bytes;
}
inline const char *slot_buffer_ptr(const char *base, size_t slot_index, size_t slot_stride_bytes)
{
    return base + slot_index * slot_stride_bytes;
}

/**
 * Policy-based next slot to read. Single place for Latest_only / Single_reader / Sync_reader.
 * Used by DataBlockConsumer::acquire_consume_slot.
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

} // namespace internal

// ============================================================================
// Slot coordination function declarations
// ============================================================================
// Defined in data_block_slot_ops.cpp; declared here for use by data_block.cpp and
// data_block_c_api.cpp. These are in namespace pylabhub::hub (not internal::) so
// they are accessible without qualification from within any namespace pylabhub::hub
// block in a TU that includes this header.
// Note: These functions are NOT PYLABHUB_UTILS_EXPORT — they are shm-layer internals
// with hidden visibility (inherited from -fvisibility=hidden on the library target).

// 4.2.1 Writer Acquisition Flow
SlotAcquireResult acquire_write(SlotRWState *slot_rw_state, SharedMemoryHeader *header,
                                int timeout_ms);

// 4.2.2 Writer Commit Flow
void commit_write(SlotRWState *slot_rw_state, SharedMemoryHeader *header, uint64_t slot_id);

// 4.2.2b Writer Release (without commit)
void release_write(SlotRWState *slot_rw_state, SharedMemoryHeader *header);

// 4.2.3 Reader Acquisition Flow
SlotAcquireResult acquire_read(SlotRWState *slot_rw_state, SharedMemoryHeader *header,
                               uint64_t *out_generation);

// 4.2.4 Reader Validation
bool validate_read_impl(SlotRWState *slot_rw_state, SharedMemoryHeader *header,
                        uint64_t captured_gen);

// 4.2.5 Reader Release Flow
void release_read(SlotRWState *slot_rw_state, SharedMemoryHeader *header);

} // namespace pylabhub::hub
