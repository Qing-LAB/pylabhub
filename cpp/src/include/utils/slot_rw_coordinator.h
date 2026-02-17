#ifndef PYLABHUB_SLOT_RW_COORDINATOR_H
#define PYLABHUB_SLOT_RW_COORDINATOR_H

/**
 * @file slot_rw_coordinator.h
 * @brief C API for slot-level read/write coordination (SlotRWState).
 *
 * **Thread safety:** This C API does **not** provide any internal locking.
 * Locking and multithread safety are entirely the caller's responsibility.
 * Use a single thread per SlotRWState, or implement external synchronization
 * (e.g. mutex) when calling these functions from multiple threads. The C++
 * DataBlockProducer/DataBlockConsumer wrap this layer and are thread-safe.
 */
#include <stdint.h>
#include <stdbool.h>
#include "pylabhub_utils_export.h"

#if defined(__cplusplus) && __cplusplus >= 201703L
#define PYLABHUB_NODISCARD [[nodiscard]]
#else
#define PYLABHUB_NODISCARD
#endif

// Forward declarations of C++ types used in the C API.
// Must be outside extern "C" — namespace keyword is not valid in C linkage context.
namespace pylabhub::hub
{
struct SlotRWState;
struct SharedMemoryHeader;
}

#ifdef __cplusplus
extern "C"
{
#endif

    // Result codes for slot acquisition operations
    typedef enum
    {
        SLOT_ACQUIRE_OK = 0,
        SLOT_ACQUIRE_TIMEOUT = 1,
        SLOT_ACQUIRE_NOT_READY = 2,
        SLOT_ACQUIRE_LOCKED = 3,
        SLOT_ACQUIRE_ERROR = 4,
        SLOT_ACQUIRE_INVALID_STATE = 5
    } SlotAcquireResult;

    // === Writer API ===
    /**
     * @brief Acquires a write lock for a SlotRWState.
     * @param rw_state Pointer to the SlotRWState structure in shared memory.
     * @param timeout_ms Maximum time to wait in milliseconds. 0 = non-blocking, -1 = no timeout (wait forever), >0 = wait up to N ms.
     * @return SLOT_ACQUIRE_OK on success, or an error code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    SlotAcquireResult slot_rw_acquire_write(pylabhub::hub::SlotRWState *rw_state, int timeout_ms);

    /**
     * @brief Commits data written to a slot, making it visible to readers.
     * @param rw_state Pointer to the SlotRWState structure.
     */
    PYLABHUB_UTILS_EXPORT
    void slot_rw_commit(pylabhub::hub::SlotRWState *rw_state);

    /**
     * @brief Releases a previously acquired write lock.
     * @param rw_state Pointer to the SlotRWState structure.
     */
    PYLABHUB_UTILS_EXPORT
    void slot_rw_release_write(pylabhub::hub::SlotRWState *rw_state);

    // === Reader API ===
    /**
     * @brief Acquires read access to a slot.
     * @param rw_state Pointer to the SlotRWState structure.
     * @param out_generation Pointer to a uint64_t to store the captured write generation.
     * @return SLOT_ACQUIRE_OK on success, or an error code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    SlotAcquireResult slot_rw_acquire_read(pylabhub::hub::SlotRWState *rw_state,
                                           uint64_t *out_generation);

    /**
     * @brief Validates that a slot has not been overwritten since read acquisition.
     * @param rw_state Pointer to the SlotRWState structure.
     * @param generation The write generation captured during acquire_read.
     * @return true if the slot is still valid, false if it was overwritten.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    bool slot_rw_validate_read(pylabhub::hub::SlotRWState *rw_state, uint64_t generation);

    /**
     * @brief Releases previously acquired read access to a slot.
     * @param rw_state Pointer to the SlotRWState structure.
     */
    PYLABHUB_UTILS_EXPORT
    void slot_rw_release_read(pylabhub::hub::SlotRWState *rw_state);

    // === Metrics API ===

    /**
     * @brief Snapshot of DataBlock metrics and key state (read-only surface).
     * All metric/state reads should go through slot_rw_get_metrics() or datablock_get_metrics().
     * total_slots_written is the commit count (0 = no commits yet). commit_index and slot_count
     * are state used e.g. by integrity validation; they are not reset by slot_rw_reset_metrics.
     */
    typedef struct
    {
        /* State snapshot (not reset by reset_metrics) */
        uint64_t commit_index;  /**< Last committed slot id (monotonic). */
        uint32_t slot_count;    /**< Ring buffer capacity (number of slots). */
        uint32_t _reserved_metrics_pad;
        /* Metrics (reset by slot_rw_reset_metrics) */
        uint64_t writer_timeout_count;
        uint64_t writer_lock_timeout_count;
        uint64_t writer_reader_timeout_count;
        uint64_t writer_blocked_total_ns;
        uint64_t write_lock_contention;
        uint64_t write_generation_wraps;
        uint64_t reader_not_ready_count;
        uint64_t reader_race_detected;
        uint64_t reader_validation_failed;
        uint64_t reader_peak_count;
        uint64_t last_error_timestamp_ns;
        uint32_t last_error_code;
        uint32_t error_sequence;
        uint64_t slot_acquire_errors;
        uint64_t slot_commit_errors;
        uint64_t checksum_failures;
        uint64_t zmq_send_failures;
        uint64_t zmq_recv_failures;
        uint64_t zmq_timeout_count;
        uint64_t recovery_actions_count;
        uint64_t schema_mismatch_count;
        uint64_t heartbeat_sent_count;
        uint64_t heartbeat_failed_count;
        uint64_t last_heartbeat_ns;
        uint64_t total_slots_written;  /**< Total commits so far (0 = no commits yet). */
        uint64_t total_slots_read;
        uint64_t total_bytes_written;
        uint64_t total_bytes_read;
        uint64_t uptime_seconds;
        uint64_t creation_timestamp_ns;
    } DataBlockMetrics;

    /**
     * @brief Retrieves current metrics and state snapshot from the shared memory header.
     * 
     * Provides a comprehensive snapshot of DataBlock performance and error metrics:
     * - **State**: commit_index (last committed slot), slot_count (ring buffer capacity)
     * - **Writer metrics**: Various timeout counts, lock contention, blocked time
     * - **Reader metrics**: Race detection, validation failures, peak concurrent readers
     * - **Error tracking**: Last error timestamp, error codes, sequence numbers
     * - **Performance**: Total slots/bytes written and read, uptime, creation timestamp
     * 
     * This function uses relaxed memory ordering for efficient snapshots. Metrics are
     * consistent within reasonable bounds but may not reflect absolute ordering with
     * concurrent operations.
     * 
     * @param shared_memory_header Pointer to the SharedMemoryHeader (must not be null).
     * @param out_metrics Pointer to a DataBlockMetrics struct to fill (must not be null).
     * @return 0 on success, -1 if either pointer is null.
     * 
     * @note This is a C API function - no exceptions, returns error codes.
     * @note Thread-safe and can be called concurrently with normal operations.
     * @note For C++ API, use DataBlockProducer::get_metrics() or DataBlockConsumer::get_metrics()
     * 
     * @par Usage from C++
     * @code
     * auto header = get_shared_memory_header();  // From your access method
     * DataBlockMetrics metrics;
     * if (slot_rw_get_metrics(header, &metrics) == 0) {
     *     printf("Total commits: %llu\n", metrics.total_slots_written);
     *     printf("Has any commits: %s\n", metrics.total_slots_written > 0 ? "yes" : "no");
     * }
     * @endcode
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    int slot_rw_get_metrics(const pylabhub::hub::SharedMemoryHeader *shared_memory_header,
                            DataBlockMetrics *out_metrics);

    /**
     * @brief Resets all metric counters in the shared memory header to zero.
     * 
     * Resets performance and error counters while preserving state information:
     * - **Reset**: All timeout counts, contention metrics, error counts, performance counters
     * - **Preserved**: commit_index, slot_count (state snapshot fields in DataBlockMetrics)
     * 
     * Useful for measuring metrics over specific time intervals or after resolving issues.
     * 
     * @param shared_memory_header Pointer to the SharedMemoryHeader (must not be null).
     * @return 0 on success, -1 if pointer is null.
     * 
     * @warning Use cautiously in production - resets diagnostic history.
     * @note This is a C API function - no exceptions, returns error codes.
     * @note Thread-safe but should be coordinated with monitoring systems.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    int slot_rw_reset_metrics(pylabhub::hub::SharedMemoryHeader *shared_memory_header);

    /**
     * @brief Lightweight accessors for single values (one load each). Use instead of full
     * slot_rw_get_metrics() when only one or a few values are needed (e.g. "has any commit?").
     * @return 0 if header is null or invalid; otherwise the stored value.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    uint64_t slot_rw_get_total_slots_written(const pylabhub::hub::SharedMemoryHeader *header);
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    uint64_t slot_rw_get_commit_index(const pylabhub::hub::SharedMemoryHeader *header);
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT
    uint32_t slot_rw_get_slot_count(const pylabhub::hub::SharedMemoryHeader *header);

    // === Error Handling ===
    /**
     * @brief Returns a string representation of a SlotAcquireResult.
     * @param result The SlotAcquireResult enum value.
     * @return A C-style string describing the result.
     */
    PYLABHUB_UTILS_EXPORT
    const char *slot_acquire_result_string(SlotAcquireResult result);

#ifdef __cplusplus
}
#endif

#endif // PYLABHUB_SLOT_RW_COORDINATOR_H
