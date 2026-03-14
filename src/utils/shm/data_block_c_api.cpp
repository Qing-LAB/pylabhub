// data_block_c_api.cpp
//
// Slot RW Coordinator C API — extern "C" wrappers for ABI stability.
// All functions declared in utils/slot_rw_coordinator.h are defined here.
//
// These are thin wrappers over the coordination functions defined in
// data_block_slot_ops.cpp (declared in data_block_internal.hpp).
//
// LIMITATION — metrics not updated via C API:
//   The internal acquire_write() / acquire_read() functions accept a
//   SharedMemoryHeader* for populating performance counters (writer_timeout_count,
//   writer_blocked_total_ns, etc.). The C API wrappers pass nullptr for that
//   parameter, so metrics are NOT updated when code calls the raw C API directly.
//   The C API is intended for low-level testing and recovery tools, not for
//   production data paths. Production code should use DataBlockProducer /
//   DataBlockConsumer (C++ API), which always supply the header pointer.

#include "data_block_internal.hpp"
#include "utils/slot_rw_coordinator.h"

namespace pylabhub::hub
{
using namespace internal;

extern "C"
{

    PYLABHUB_UTILS_EXPORT SlotAcquireResult slot_rw_acquire_write(pylabhub::hub::SlotRWState *rw_state, int timeout_ms)
    {
        if (rw_state == nullptr)
        {
            return SLOT_ACQUIRE_ERROR;
        }
        return acquire_write(rw_state, nullptr, timeout_ms);
    }

    PYLABHUB_UTILS_EXPORT void slot_rw_commit(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state != nullptr)
        {
            // slot_id=0: header is nullptr so commit_index is not updated by this C API path.
            // Callers that need commit_index tracking must use DataBlockProducer::release_write_slot().
            commit_write(rw_state, nullptr, 0);
        }
    }

    PYLABHUB_UTILS_EXPORT void slot_rw_release_write(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state != nullptr)
        {
            release_write(rw_state, nullptr);
        }
    }

    PYLABHUB_UTILS_EXPORT SlotAcquireResult slot_rw_acquire_read(pylabhub::hub::SlotRWState *rw_state,
                                           uint64_t *out_generation)
    {
        if (rw_state == nullptr || out_generation == nullptr)
        {
            return SLOT_ACQUIRE_ERROR;
        }
        return acquire_read(rw_state, nullptr, out_generation);
    }

    PYLABHUB_UTILS_EXPORT bool slot_rw_validate_read(pylabhub::hub::SlotRWState *rw_state, uint64_t generation)
    {
        if (rw_state == nullptr)
        {
            return false;
        }
        return validate_read_impl(rw_state, nullptr, generation);
    }

    PYLABHUB_UTILS_EXPORT void slot_rw_release_read(pylabhub::hub::SlotRWState *rw_state)
    {
        if (rw_state != nullptr)
        {
            release_read(rw_state, nullptr);
        }
    }

    PYLABHUB_UTILS_EXPORT const char *slot_acquire_result_string(SlotAcquireResult result)
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

    PYLABHUB_UTILS_EXPORT int slot_rw_get_metrics(const pylabhub::hub::SharedMemoryHeader *shared_memory_header,
                            DataBlockMetrics *out_metrics)
    {
        if (shared_memory_header == nullptr || out_metrics == nullptr)
        {
            return -1;
        }
        /* State snapshot (not reset by reset_metrics) */
        // acquire: pairs with the release store in update_commit_index() so the caller
        // observes a commit_index that is consistent with the data written before it.
        out_metrics->commit_index =
            shared_memory_header->commit_index.load(std::memory_order_acquire);
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

    PYLABHUB_UTILS_EXPORT uint64_t slot_rw_get_total_slots_written(
        const pylabhub::hub::SharedMemoryHeader *header)
    {
        return header != nullptr
                   ? header->total_slots_written.load(std::memory_order_relaxed)
                   : 0;
    }

    PYLABHUB_UTILS_EXPORT uint64_t slot_rw_get_commit_index(const pylabhub::hub::SharedMemoryHeader *header)
    {
        // acquire: consistent with the release store in update_commit_index()
        return header != nullptr
                   ? header->commit_index.load(std::memory_order_acquire)
                   : 0;
    }

    PYLABHUB_UTILS_EXPORT uint32_t slot_rw_get_slot_count(const pylabhub::hub::SharedMemoryHeader *header)
    {
        return header != nullptr ? pylabhub::hub::detail::get_slot_count(header) : 0;
    }

    PYLABHUB_UTILS_EXPORT int slot_rw_reset_metrics(pylabhub::hub::SharedMemoryHeader *shared_memory_header)
    {
        if (shared_memory_header == nullptr)
        {
            return -1;
        }
        // Metrics are observational — no ordering requirement between resets.
        shared_memory_header->writer_timeout_count.store(0, std::memory_order_relaxed);
        shared_memory_header->writer_lock_timeout_count.store(0, std::memory_order_relaxed);
        shared_memory_header->writer_reader_timeout_count.store(0, std::memory_order_relaxed);
        shared_memory_header->writer_blocked_total_ns.store(0, std::memory_order_relaxed);
        shared_memory_header->write_lock_contention.store(0, std::memory_order_relaxed);
        shared_memory_header->write_generation_wraps.store(0, std::memory_order_relaxed);
        shared_memory_header->reader_not_ready_count.store(0, std::memory_order_relaxed);
        shared_memory_header->reader_race_detected.store(0, std::memory_order_relaxed);
        shared_memory_header->reader_validation_failed.store(0, std::memory_order_relaxed);
        shared_memory_header->reader_peak_count.store(0, std::memory_order_relaxed);
        shared_memory_header->last_error_timestamp_ns.store(0, std::memory_order_relaxed);
        shared_memory_header->last_error_code.store(0, std::memory_order_relaxed);
        shared_memory_header->error_sequence.store(0, std::memory_order_relaxed);
        shared_memory_header->slot_acquire_errors.store(0, std::memory_order_relaxed);
        shared_memory_header->slot_commit_errors.store(0, std::memory_order_relaxed);
        shared_memory_header->checksum_failures.store(0, std::memory_order_relaxed);
        shared_memory_header->zmq_send_failures.store(0, std::memory_order_relaxed);
        shared_memory_header->zmq_recv_failures.store(0, std::memory_order_relaxed);
        shared_memory_header->zmq_timeout_count.store(0, std::memory_order_relaxed);
        shared_memory_header->recovery_actions_count.store(0, std::memory_order_relaxed);
        shared_memory_header->schema_mismatch_count.store(0, std::memory_order_relaxed);
        shared_memory_header->heartbeat_sent_count.store(0, std::memory_order_relaxed);
        shared_memory_header->heartbeat_failed_count.store(0, std::memory_order_relaxed);
        shared_memory_header->last_heartbeat_ns.store(0, std::memory_order_relaxed);
        shared_memory_header->total_slots_written.store(0, std::memory_order_relaxed);
        shared_memory_header->total_slots_read.store(0, std::memory_order_relaxed);
        shared_memory_header->total_bytes_written.store(0, std::memory_order_relaxed);
        shared_memory_header->total_bytes_read.store(0, std::memory_order_relaxed);
        shared_memory_header->uptime_seconds.store(0, std::memory_order_relaxed);
        return 0;
    }

    PYLABHUB_UTILS_EXPORT int slot_rw_get_channel_identity(
        const pylabhub::hub::SharedMemoryHeader *header, plh_channel_identity_t *out)
    {
        if (header == nullptr || out == nullptr)
        {
            return -1;
        }
        std::memcpy(out->hub_uid, header->hub_uid, sizeof(out->hub_uid));
        std::memcpy(out->hub_name, header->hub_name, sizeof(out->hub_name));
        std::memcpy(out->producer_uid, header->producer_uid, sizeof(out->producer_uid));
        std::memcpy(out->producer_name, header->producer_name, sizeof(out->producer_name));
        // Guarantee null-termination regardless of what the producer wrote
        out->hub_uid[sizeof(out->hub_uid) - 1]             = '\0';
        out->hub_name[sizeof(out->hub_name) - 1]           = '\0';
        out->producer_uid[sizeof(out->producer_uid) - 1]   = '\0';
        out->producer_name[sizeof(out->producer_name) - 1] = '\0';
        return 0;
    }

    PYLABHUB_UTILS_EXPORT int slot_rw_list_consumers(
        const pylabhub::hub::SharedMemoryHeader *header, plh_consumer_identity_t *out_array,
        int array_capacity, int *out_count)
    {
        if (header == nullptr || out_array == nullptr || array_capacity <= 0 ||
            out_count == nullptr)
        {
            return -1;
        }
        *out_count = 0;
        for (size_t i = 0; i < pylabhub::hub::detail::MAX_CONSUMER_HEARTBEATS; ++i)
        {
            uint64_t pid =
                header->consumer_heartbeats[i].consumer_pid.load(std::memory_order_acquire);
            if (pid == 0)
            {
                continue;
            }
            if (*out_count >= array_capacity)
            {
                break;
            }
            plh_consumer_identity_t &entry = out_array[*out_count];
            entry.consumer_pid             = pid;
            entry.last_heartbeat_ns        = header->consumer_heartbeats[i].last_heartbeat_ns.load(
                std::memory_order_relaxed);
            entry.slot_index = static_cast<int>(i);
            std::memcpy(entry.consumer_uid, header->consumer_heartbeats[i].consumer_uid,
                        sizeof(entry.consumer_uid));
            entry.consumer_uid[sizeof(entry.consumer_uid) - 1] = '\0';
            std::memcpy(entry.consumer_name, header->consumer_heartbeats[i].consumer_name,
                        sizeof(entry.consumer_name));
            entry.consumer_name[sizeof(entry.consumer_name) - 1] = '\0';
            (*out_count)++;
        }
        return 0;
    }

} // extern "C"

} // namespace pylabhub::hub
