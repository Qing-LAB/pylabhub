#ifndef PYLABHUB_SLOT_RW_COORDINATOR_H
#define PYLABHUB_SLOT_RW_COORDINATOR_H

#include <stdint.h>
#include <stdbool.h>

// Forward declaration of the C++ SlotRWState struct
// The C functions will operate on a pointer to this type,
// with the expectation that the C++ implementation will cast it.
namespace pylabhub::hub
{
struct SlotRWState;
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
     * @param timeout_ms Maximum time to wait in milliseconds. 0 for no timeout.
     * @return SLOT_ACQUIRE_OK on success, or an error code.
     */
    SlotAcquireResult slot_rw_acquire_write(pylabhub::hub::SlotRWState *rw_state, int timeout_ms);

    /**
     * @brief Commits data written to a slot, making it visible to readers.
     * @param rw_state Pointer to the SlotRWState structure.
     */
    void slot_rw_commit(pylabhub::hub::SlotRWState *rw_state);

    /**
     * @brief Releases a previously acquired write lock.
     * @param rw_state Pointer to the SlotRWState structure.
     */
    void slot_rw_release_write(pylabhub::hub::SlotRWState *rw_state);

    // === Reader API ===
    /**
     * @brief Acquires read access to a slot.
     * @param rw_state Pointer to the SlotRWState structure.
     * @param out_generation Pointer to a uint64_t to store the captured write generation.
     * @return SLOT_ACQUIRE_OK on success, or an error code.
     */
    SlotAcquireResult slot_rw_acquire_read(pylabhub::hub::SlotRWState *rw_state,
                                           uint64_t *out_generation);

    /**
     * @brief Validates that a slot has not been overwritten since read acquisition.
     * @param rw_state Pointer to the SlotRWState structure.
     * @param generation The write generation captured during acquire_read.
     * @return true if the slot is still valid, false if it was overwritten.
     */
    bool slot_rw_validate_read(pylabhub::hub::SlotRWState *rw_state, uint64_t generation);

    /**
     * @brief Releases previously acquired read access to a slot.
     * @param rw_state Pointer to the SlotRWState structure.
     */
    void slot_rw_release_read(pylabhub::hub::SlotRWState *rw_state);

    // === Metrics API ===
    // Forward declaration of SharedMemoryHeader to access metrics
    namespace pylabhub::hub
    {
    struct SharedMemoryHeader;
    }

    typedef struct
    {
        uint64_t writer_timeout_count;
        uint64_t writer_blocked_total_ns;
        uint64_t write_lock_contention;
        uint64_t write_generation_wraps;
        uint64_t reader_not_ready_count;
        uint64_t reader_race_detected;
        uint64_t reader_validation_failed;
        uint64_t reader_peak_count;
        // Add other metrics as needed from SharedMemoryHeader
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
        uint64_t total_slots_written;
        uint64_t total_slots_read;
        uint64_t total_bytes_written;
        uint64_t total_bytes_read;
        uint64_t uptime_seconds;
        uint64_t creation_timestamp_ns;
    } DataBlockMetrics;

    /**
     * @brief Retrieves current metrics from the shared memory header.
     * @param shared_memory_header Pointer to the SharedMemoryHeader.
     * @param out_metrics Pointer to a DataBlockMetrics struct to fill.
     * @return 0 on success, -1 on error.
     */
    int slot_rw_get_metrics(const pylabhub::hub::SharedMemoryHeader *shared_memory_header,
                            DataBlockMetrics *out_metrics);

    /**
     * @brief Resets metrics in the shared memory header.
     * @param shared_memory_header Pointer to the SharedMemoryHeader.
     * @return 0 on success, -1 on error.
     */
    int slot_rw_reset_metrics(pylabhub::hub::SharedMemoryHeader *shared_memory_header);

    // === Error Handling ===
    /**
     * @brief Returns a string representation of a SlotAcquireResult.
     * @param result The SlotAcquireResult enum value.
     * @return A C-style string describing the result.
     */
    const char *slot_acquire_result_string(SlotAcquireResult result);

#ifdef __cplusplus
}
#endif

#endif // PYLABHUB_SLOT_RW_COORDINATOR_H
