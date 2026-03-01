// data_block_slot_ops.cpp
//
// Slot RW coordination functions — the state-machine core of the DataBlock layer.
//
// Contains definitions of the six low-level coordination functions declared in
// data_block_internal.hpp:
//   acquire_write / commit_write / release_write
//   acquire_read  / validate_read_impl / release_read
// Plus the exported is_writer_alive() wrapper.
//
// Design: HEP-CORE-0002 §4.2 (Slot State Machine)
// TOCTTOU: Reader path (acquire_read) uses double-check (reader_count then state re-check).
// Do not reorder without reviewing the HEP and tests.

#include "data_block_internal.hpp"

namespace pylabhub::hub
{
using namespace internal; // brings spin_elapsed_ms_exceeded, backoff, etc. into scope

// ============================================================================
// 4.2.1 Writer Acquisition Flow
// ============================================================================
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
            // Zombie lock — writer process is dead. Use CAS (not store) to reclaim so that
            // only one process wins when multiple processes detect the same zombie simultaneously.
            // If the CAS fails, another process already reclaimed it; fall through to spin/timeout.
            uint64_t zombie_pid = expected_lock;
            if (slot_rw_state->write_lock.compare_exchange_strong(
                    zombie_pid, my_pid, std::memory_order_acquire, std::memory_order_relaxed))
            {
                LOGGER_WARN("SlotRWState: Reclaimed zombie write lock from dead PID {}.",
                            expected_lock);
                if (header != nullptr)
                {
                    detail::increment_metric_write_lock_contention(header);
                }
                break; // Acquired
            }
            // Lost the reclaim race — another process grabbed it first; fall through to spin/timeout.
        }

        // Check for timeout if lock was not acquired or was valid contention.
        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            if (header != nullptr)
            {
                detail::increment_metric_writer_timeout(header);
                detail::increment_metric_writer_lock_timeout(header);
            }
            return SLOT_ACQUIRE_TIMEOUT;
        }
        backoff(iteration++);
    }

    // Now we hold the write_lock.
    // If the previous occupant committed data (COMMITTED), transition to DRAINING so new
    // readers see a non-COMMITTED state immediately and bail out, while existing in-progress
    // readers (reader_count > 0) drain naturally. FREE slots have no readers, so we skip
    // the DRAINING transition — no readers can be holding a FREE slot.
    SlotRWState::SlotState prev_state =
        slot_rw_state->slot_state.load(std::memory_order_acquire);
    bool entered_draining = (prev_state == SlotRWState::SlotState::COMMITTED);

    if (entered_draining)
    {
        // COMMITTED → DRAINING: write_lock held, so plain store is safe.
        slot_rw_state->slot_state.store(SlotRWState::SlotState::DRAINING,
                                        std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    slot_rw_state->writer_waiting.store(1, std::memory_order_relaxed); // diagnostic compat

    iteration = 0;
    while (true)
    {
        std::atomic_thread_fence(std::memory_order_seq_cst); // Force visibility

        uint32_t readers = slot_rw_state->reader_count.load(std::memory_order_acquire);
        if (readers == 0)
        {
            break; // All readers finished
        }

        if (spin_elapsed_ms_exceeded(start_time, timeout_ms))
        {
            slot_rw_state->writer_waiting.store(0, std::memory_order_relaxed);
            if (entered_draining)
            {
                // Previous commit data is still valid — restore so consumers can read it.
                slot_rw_state->slot_state.store(SlotRWState::SlotState::COMMITTED,
                                                std::memory_order_release);
            }
            slot_rw_state->write_lock.store(
                0, std::memory_order_release); // Release the lock before returning timeout
            if (header != nullptr)
            {
                detail::increment_metric_writer_timeout(header);
                detail::increment_metric_writer_reader_timeout(header);
            }
            return SLOT_ACQUIRE_TIMEOUT;
        }

        backoff(iteration++);
    }
    slot_rw_state->writer_waiting.store(0, std::memory_order_relaxed); // All readers drained

    // DRAINING → WRITING (or FREE → WRITING — same store either way)
    slot_rw_state->slot_state.store(SlotRWState::SlotState::WRITING, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst); // Ensure state change is visible

    return SLOT_ACQUIRE_OK;
}

// ============================================================================
// 4.2.2 Writer Commit Flow
// ============================================================================
// slot_id: the write_index-derived ID for this slot (stored in commit_index so consumers can
// locate the committed slot directly even when earlier slot_ids were aborted/skipped).
void commit_write(SlotRWState *slot_rw_state, SharedMemoryHeader *header, uint64_t slot_id)
{
    slot_rw_state->write_generation.fetch_add(
        1, std::memory_order_release); // Step 1: Increment generation counter
    slot_rw_state->slot_state.store(SlotRWState::SlotState::COMMITTED,
                                    std::memory_order_release); // Step 2: Transition to COMMITTED state
    if (header != nullptr)
    {
        detail::update_commit_index(header, slot_id); // Step 3: Store slot_id in commit_index (makes visible to consumers)
        detail::increment_metric_total_commits(header); // Metric: commit count
    }
    // Memory ordering: All writes before this release are visible to
    // any consumer that performs acquire on commit_index or slot_state
}

// ============================================================================
// 4.2.2b Writer Release (without commit) — for abort paths
// ============================================================================
void release_write(SlotRWState *slot_rw_state, SharedMemoryHeader * /*header*/)
{
    // State must be set to FREE before releasing the write lock. If the lock were released
    // first, another writer could CAS write_lock and set state=WRITING before this thread
    // stores FREE — silently clobbering the new writer's WRITING state.
    slot_rw_state->slot_state.store(SlotRWState::SlotState::FREE, std::memory_order_release);
    slot_rw_state->write_lock.store(0, std::memory_order_release);
}

// ============================================================================
// 4.2.3 Reader Acquisition Flow (TOCTTOU-Safe)
// ============================================================================
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
        slot_rw_state->reader_count.fetch_sub(1, std::memory_order_release);
        if (header != nullptr)
        {
            detail::increment_metric_reader_race_detected(header);
        }
        return SLOT_ACQUIRE_NOT_READY;
    }

    // Step 5: Capture generation for optimistic validation
    *out_generation = slot_rw_state->write_generation.load(std::memory_order_acquire);

    return SLOT_ACQUIRE_OK;
}

// ============================================================================
// 4.2.4 Reader Validation (Wrap-Around Detection)
// ============================================================================
/// Returns false if generation changed (wrap-around or slot overwritten); in-flight read is then invalid.
bool validate_read_impl(SlotRWState *slot_rw_state, SharedMemoryHeader *header,
                        uint64_t captured_gen)
{
    uint64_t current_gen = slot_rw_state->write_generation.load(std::memory_order_acquire);

    if (current_gen != captured_gen)
    {
        if (header != nullptr)
        {
            detail::increment_metric_reader_validation_failed(header);
        }
        return false;
    }

    return true;
}

// ============================================================================
// 4.2.5 Reader Release Flow
// ============================================================================
void release_read(SlotRWState *slot_rw_state, SharedMemoryHeader *header)
{
    uint32_t prev_count = slot_rw_state->reader_count.fetch_sub(1, std::memory_order_release);
    detail::update_reader_peak_count(header, prev_count);
    // If last reader and writer is waiting, writer will proceed
    // (writer polls reader_count with acquire ordering)
}

// ============================================================================
// is_writer_alive — exported wrapper (declared PYLABHUB_UTILS_EXPORT in data_block.hpp)
// ============================================================================
bool is_writer_alive(const SharedMemoryHeader *header, uint64_t pid) noexcept
{
    return detail::is_writer_alive_impl(header, pid);
}

} // namespace pylabhub::hub
