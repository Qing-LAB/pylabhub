#pragma once

/**
 * @file recovery_api.hpp
 * @brief C-style API for DataBlock error recovery and diagnostics.
 *
 * This API provides a set of functions to diagnose and recover from common
 * failure scenarios in shared memory DataBlocks, such as crashed producers or
 * consumers. Implemented in data_block_recovery.cpp.
 *
 * **Thread safety:** The C recovery/diagnostic API does **not** provide internal
 * locking. Locking and multithread safety are the caller's responsibility when
 * invoking these functions from multiple threads.
 *
 * ## Error Codes Reference
 *
 * **Diagnostics (datablock_diagnose_slot, datablock_diagnose_all_slots):**
 * - 0   Success
 * - -1  Invalid arguments (null pointer)
 * - -2  Internal error (open failed, etc.)
 * - -3  Invalid slot_index (out of bounds)
 * - -4  Runtime error during DataBlock access
 * - -5  Unexpected/general error
 *
 * **Recovery operations:** Return RecoveryResult enum (see below).
 */
#include "pylabhub_utils_export.h"
#include "utils/slot_rw_coordinator.h"
#include <cstddef>
#include <cstdint>
#include <stdbool.h>

#if defined(__cplusplus) && __cplusplus >= 201703L
#define PYLABHUB_NODISCARD [[nodiscard]]
#else
#define PYLABHUB_NODISCARD
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Diagnostic information for a single data slot.
     */
    typedef struct
    {
        uint64_t slot_id;          ///< Monotonic ID of the slot.
        uint32_t slot_index;       ///< Physical index in the ring buffer.
        uint8_t slot_state;        ///< Current state (FREE, WRITING, etc.). See @ref SlotState.
        uint64_t write_lock;       ///< PID of the process holding the write lock (0 if none).
        uint32_t reader_count;     ///< Number of active readers.
        uint64_t write_generation; ///< Incremented on each write cycle.
        uint8_t writer_waiting;    ///< 1 if a writer is blocked for readers to drain.
        bool is_stuck;             ///< Heuristic: true if the slot appears to be stuck.
        uint64_t
            stuck_duration_ms; ///< Approximate duration the slot has been stuck (if applicable).
    } SlotDiagnostic;

    /**
     * @brief Result codes for recovery operations.
     */
    typedef enum
    {
        RECOVERY_SUCCESS = 0,     ///< Operation completed successfully.
        RECOVERY_FAILED = 1,      ///< Operation failed due to an internal error.
        RECOVERY_UNSAFE = 2,      ///< Operation was deemed unsafe and was not performed.
        RECOVERY_NOT_STUCK = 3,   ///< The target resource was not stuck; no action was taken.
        RECOVERY_INVALID_SLOT = 4 ///< The specified slot index was out of bounds.
    } RecoveryResult;

    // --- Diagnostics ---

    /**
     * @brief Gets diagnostic information for a single shared memory data slot.
     * @param shm_name The name of the shared memory DataBlock.
     * @param slot_index The physical index of the slot to diagnose.
     * @param out Pointer to a SlotDiagnostic struct to be filled.
     * @return 0 on success, non-zero on error.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT int datablock_diagnose_slot(const char *shm_name,
                                                                          uint32_t slot_index,
                                                                          SlotDiagnostic *out);

    /**
     * @brief Gets diagnostic information for all slots in a DataBlock.
     * @param shm_name The name of the shared memory DataBlock.
     * @param out_array Pointer to an array of SlotDiagnostic structs to fill.
     * @param array_capacity The maximum number of structs `out_array` can hold.
     * @param out_count Pointer to a size_t that will store the number of slots written.
     * @return 0 on success, non-zero on error.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT int datablock_diagnose_all_slots(const char *shm_name,
                                                                               SlotDiagnostic *out_array,
                                                                               size_t array_capacity,
                                                                               size_t *out_count);

    /**
     * @brief Checks if a process with the given PID is currently alive.
     * @param pid The process ID to check.
     * @return True if the process is alive, false otherwise.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT bool datablock_is_process_alive(uint64_t pid);

    // --- Recovery Operations ---

    /**
     * @brief Forcefully resets the state of a single DataBlock slot.
     * @warning This is a DANGEROUS operation. Use with caution.
     * @param shm_name The name of the shared memory DataBlock.
     * @param slot_index The physical index of the slot to reset.
     * @param force If true, bypasses safety checks (e.g., if a live process holds a lock).
     * @return A `RecoveryResult` code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT RecoveryResult datablock_force_reset_slot(
        const char *shm_name, uint32_t slot_index, bool force);

    /**
     * @brief Forcefully resets the state of all slots in a DataBlock.
     * @warning This is a VERY DANGEROUS operation. Use with extreme caution.
     * @param shm_name The name of the shared memory DataBlock.
     * @param force If true, bypasses safety checks for each slot.
     * @return A `RecoveryResult` code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT RecoveryResult datablock_force_reset_all_slots(
        const char *shm_name, bool force);

    /**
     * @brief Releases readers that are presumed to be zombies (i.e., dead processes).
     * @param shm_name The name of the shared memory DataBlock.
     * @param slot_index The physical index of the slot to clean up.
     * @param force If true, clears the reader count regardless of other checks.
     * @return A `RecoveryResult` code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT RecoveryResult datablock_release_zombie_readers(
        const char *shm_name, uint32_t slot_index, bool force);

    /**
     * @brief Releases a writer that is presumed to be a zombie (i.e., a dead process).
     * @param shm_name The name of the shared memory DataBlock.
     * @param slot_index The physical index of the slot to clean up.
     * @return A `RecoveryResult` code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT RecoveryResult datablock_release_zombie_writer(
        const char *shm_name, uint32_t slot_index);

    /**
     * @brief Scans the consumer heartbeat table and cleans up any dead consumers.
     * @param shm_name The name of the shared memory DataBlock.
     * @return A `RecoveryResult` code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT RecoveryResult datablock_cleanup_dead_consumers(
        const char *shm_name);

    /**
     * @brief Validates the integrity of the DataBlock's control structures and checksums.
     * @param shm_name The name of the shared memory DataBlock.
     * @param repair If true, attempts to recalculate invalid checksums.
     * @return A `RecoveryResult` code.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT RecoveryResult datablock_validate_integrity(
        const char *shm_name, bool repair);

    // --- Metrics (name-based; same surface as slot_rw_get_metrics / slot_rw_reset_metrics) ---

    /**
     * @brief Retrieves current metrics and state snapshot for a DataBlock by name.
     * 
     * Opens the DataBlock in read-only diagnostic mode and retrieves comprehensive metrics:
     * - State snapshot: commit_index, slot_count
     * - Writer/reader metrics: timeouts, contention, races, validation failures
     * - Error tracking: timestamps, error codes, sequences
     * - Performance: total slots/bytes written and read
     * 
     * This is a name-based convenience wrapper around slot_rw_get_metrics() for external
     * diagnostics and monitoring tools that don't have direct Producer/Consumer handles.
     * 
     * @param shm_name The name of the shared memory DataBlock (must not be null).
     * @param out_metrics Pointer to a DataBlockMetrics struct to fill (must not be null).
     * @return 0 on success, -1 on error (invalid args, DataBlock not found, open failed).
     * 
     * @note This is a C API function - no exceptions, returns error codes.
     * @note Opens and closes the DataBlock internally - not for hot path use.
     * @note For active producers/consumers, use DataBlockProducer::get_metrics() or
     *       DataBlockConsumer::get_metrics() instead (more efficient).
     * 
     * @par Example
     * @code
     * DataBlockMetrics metrics;
     * if (datablock_get_metrics("my_datablock", &metrics) == 0) {
     *     printf("Commit index: %llu\n", metrics.commit_index);
     *     printf("Total commits: %llu (has_commits: %s)\n", 
     *            metrics.total_slots_written,
     *            metrics.total_slots_written > 0 ? "yes" : "no");
     *     printf("Writer timeouts: %llu\n", metrics.writer_timeout_count);
     *     printf("Reader races: %llu\n", metrics.reader_race_detected);
     * }
     * @endcode
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT int datablock_get_metrics(const char *shm_name,
                                                                       DataBlockMetrics *out_metrics);

    /**
     * @brief Resets metrics for a DataBlock by name.
     * 
     * Resets all metric counters to zero while preserving state fields (commit_index, slot_count).
     * Opens the DataBlock in read-write diagnostic mode to perform the reset.
     * 
     * @param shm_name The name of the shared memory DataBlock (must not be null).
     * @return 0 on success, -1 on error (invalid args, DataBlock not found, open failed).
     * 
     * @warning Use cautiously - resets diagnostic history.
     * @note Opens and closes the DataBlock internally.
     */
    PYLABHUB_NODISCARD PYLABHUB_UTILS_EXPORT int datablock_reset_metrics(const char *shm_name);

#ifdef __cplusplus
}
#endif
