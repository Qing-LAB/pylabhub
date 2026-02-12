#include "utils/recovery_api.hpp"
#include "plh_platform.hpp"
#include "utils/data_block.hpp"
#include "utils/logger.hpp"
#include "utils/message_hub.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Map SlotRWState::SlotState enum to an integer for C API compatibility
static uint8_t map_slot_state(pylabhub::hub::SlotRWState::SlotState state)
{
    return static_cast<uint8_t>(state);
}

// Shared context for recovery APIs: holds handle and header to avoid repeating open/validation.
struct RecoveryContext
{
    std::unique_ptr<pylabhub::hub::DataBlockDiagnosticHandle> handle;
    pylabhub::hub::SharedMemoryHeader *header{nullptr};
    std::string shm_name;
};

// Opens datablock for diagnosis; logs and returns nullopt on failure. Use in all recovery C APIs.
static std::optional<RecoveryContext> open_for_recovery(const char *shm_name_cstr)
{
    if (!shm_name_cstr)
    {
        LOGGER_ERROR("recovery: Invalid arguments (null shm_name).");
        return std::nullopt;
    }
    std::string shm_name = shm_name_cstr;
    auto handle = pylabhub::hub::open_datablock_for_diagnostic(shm_name);
    if (!handle)
    {
        LOGGER_ERROR("recovery: Failed to open '{}' for diagnosis.", shm_name);
        return std::nullopt;
    }
    pylabhub::hub::SharedMemoryHeader *header = handle->header();
    if (!header)
    {
        LOGGER_ERROR("recovery: Failed to get header for '{}'.", shm_name);
        return std::nullopt;
    }
    return RecoveryContext{std::move(handle), header, std::move(shm_name)};
}

// Store platform monotonic timestamp (ns) into header->last_error_timestamp_ns. Used after recovery actions.
static void set_recovery_timestamp(pylabhub::hub::SharedMemoryHeader *header)
{
    if (header)
        header->last_error_timestamp_ns.store(pylabhub::platform::monotonic_time_ns(),
                                              std::memory_order_release);
}

// Implementation of datablock_diagnose_slot
extern "C"
{

    PYLABHUB_UTILS_EXPORT int datablock_diagnose_slot(const char *shm_name_cstr,
                                                      uint32_t slot_index, SlotDiagnostic *out)
    {
        if (!out)
        {
            LOGGER_ERROR("datablock_diagnose_slot: Invalid arguments (null pointer).");
            return -1; // Invalid arguments
        }
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return -2; // Internal error

        pylabhub::hub::SharedMemoryHeader *header = ctx->header;
        uint32_t ring_buffer_capacity = header->ring_buffer_capacity;
        if (slot_index >= ring_buffer_capacity)
        {
            LOGGER_ERROR("datablock_diagnose_slot: Invalid slot_index {} for capacity {}.",
                         slot_index, ring_buffer_capacity);
            return -3; // Invalid index
        }

        pylabhub::hub::SlotRWState *rw_state = ctx->handle->slot_rw_state(slot_index);
        if (!rw_state)
        {
            LOGGER_ERROR(
                "datablock_diagnose_slot: Failed to get slot_rw_state for slot {} in '{}'.",
                slot_index, ctx->shm_name);
            return -2;
        }

        try
        {
            // Populate SlotDiagnostic
            out->slot_index = slot_index;
            out->slot_id = header->commit_index.load(
                std::memory_order_acquire); // Use latest committed ID as reference
            out->slot_state = map_slot_state(rw_state->slot_state.load(std::memory_order_acquire));
            out->write_lock = rw_state->write_lock.load(std::memory_order_acquire);
            out->reader_count = rw_state->reader_count.load(std::memory_order_acquire);
            out->write_generation = rw_state->write_generation.load(std::memory_order_acquire);
            out->writer_waiting = rw_state->writer_waiting.load(std::memory_order_acquire);

            // Determine if stuck (heuristic)
            out->is_stuck = false;
            out->stuck_duration_ms = 0;

            if (out->write_lock != 0)
            { // If a process holds the write lock
                if (!pylabhub::platform::is_process_alive(out->write_lock))
                {
                    out->is_stuck = true;
                    // TODO: Calculate stuck_duration_ms (requires timestamp when lock was acquired)
                    // For now, assume a generic stuck state.
                }
            }
            else if (out->reader_count > 0)
            { // If readers exist, but no active process
                // This is harder to diagnose without tracking individual reader PIDs.
                // For now, a non-zero reader_count with no corresponding active consumer PID
                // is a potential stuck state.
                // This needs refinement based on actual consumer heartbeat tracking.
                // if (header->active_consumer_count.load() == 0) out->is_stuck = true;
            }
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_diagnose_slot: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return -4; // Runtime error during DataBlock access
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_diagnose_slot: Unexpected error for '{}': {}", ctx->shm_name,
                         ex.what());
            return -5; // General unexpected error
        }

        return 0; // Success
    }

    PYLABHUB_UTILS_EXPORT int datablock_diagnose_all_slots(const char *shm_name_cstr,
                                                           SlotDiagnostic *out_array,
                                                           size_t array_capacity, size_t *out_count)
    {
        if (!out_array || !out_count)
        {
            LOGGER_ERROR("datablock_diagnose_all_slots: Invalid arguments (null pointer).");
            return -1; // Invalid arguments
        }
        *out_count = 0; // Initialize count
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return -2;

        try
        {
            uint32_t ring_buffer_capacity = ctx->header->ring_buffer_capacity;

            for (uint32_t i = 0; i < ring_buffer_capacity; ++i)
            {
                if (*out_count < array_capacity)
                {
                    int result = datablock_diagnose_slot(shm_name_cstr, i, &out_array[*out_count]);
                    if (result != 0)
                    {
                        // Log error but continue with other slots if possible
                        LOGGER_ERROR("datablock_diagnose_all_slots: Failed to diagnose slot {} for "
                                     "'{}'. Error code: {}.",
                                     i, ctx->shm_name, result);
                    }
                    else
                    {
                        (*out_count)++;
                    }
                }
                else
                {
                    LOGGER_WARN("datablock_diagnose_all_slots: Array capacity {} exceeded. "
                                "Stopping at {} slots.",
                                array_capacity, *out_count);
                    break; // Stop if output array is full
                }
            }
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_diagnose_all_slots: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return -4; // Runtime error during DataBlock access
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_diagnose_all_slots: Unexpected error for '{}': {}", ctx->shm_name,
                         ex.what());
            return -5; // General unexpected error
        }

        return 0; // Success
    }

    PYLABHUB_UTILS_EXPORT bool datablock_is_process_alive(uint64_t pid)
    {
        return pylabhub::platform::is_process_alive(pid);
    }

    PYLABHUB_UTILS_EXPORT RecoveryResult datablock_force_reset_slot(const char *shm_name_cstr,
                                                                    uint32_t slot_index, bool force)
    {
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return RECOVERY_FAILED;

        pylabhub::hub::SharedMemoryHeader *header = ctx->header;
        try
        {
            uint32_t ring_buffer_capacity = header->ring_buffer_capacity;
            if (slot_index >= ring_buffer_capacity)
            {
                LOGGER_ERROR("datablock_force_reset_slot: Invalid slot_index {} for capacity {}.",
                             slot_index, ring_buffer_capacity);
                return RECOVERY_INVALID_SLOT;
            }

            pylabhub::hub::SlotRWState *rw_state = ctx->handle->slot_rw_state(slot_index);
            if (!rw_state)
            {
                LOGGER_ERROR(
                    "datablock_force_reset_slot: Failed to get slot_rw_state for slot {} in '{}'.",
                    slot_index, ctx->shm_name);
                return RECOVERY_FAILED;
            }

            uint64_t current_write_lock = rw_state->write_lock.load(std::memory_order_acquire);
            uint32_t current_reader_count = rw_state->reader_count.load(std::memory_order_acquire);
            pylabhub::hub::SlotRWState::SlotState current_slot_state =
                rw_state->slot_state.load(std::memory_order_acquire);

            if (current_write_lock != 0 && pylabhub::platform::is_process_alive(current_write_lock))
            {
                if (!force)
                {
                    LOGGER_ERROR("datablock_force_reset_slot: Slot {} write lock held by ALIVE "
                                 "process {}. Cannot reset without force flag.",
                                 slot_index, current_write_lock);
                    return RECOVERY_UNSAFE;
                }
                else
                {
                    LOGGER_WARN("datablock_force_reset_slot: FORCE resetting slot {} even though "
                                "write lock is held by ALIVE process {}.",
                                slot_index, current_write_lock);
                }
            }

            if (current_reader_count > 0 && !force)
            {
                LOGGER_WARN("datablock_force_reset_slot: Slot {} has {} active readers. Cannot "
                            "reset without force flag.",
                            slot_index, current_reader_count);
                return RECOVERY_UNSAFE;
            }

            LOGGER_WARN("RECOVERY: Resetting slot {} in '{}'. State before: {{lock={}, readers={}, "
                        "state={}}}.",
                        slot_index, ctx->shm_name, current_write_lock, current_reader_count,
                        map_slot_state(current_slot_state));

            rw_state->write_lock.store(0, std::memory_order_release);
            rw_state->reader_count.store(0, std::memory_order_release);
            rw_state->slot_state.store(pylabhub::hub::SlotRWState::SlotState::FREE,
                                       std::memory_order_release);
            rw_state->writer_waiting.store(0, std::memory_order_release);

            header->recovery_actions_count.fetch_add(1, std::memory_order_relaxed);
            set_recovery_timestamp(header);

            LOGGER_WARN("RECOVERY: Slot {} in '{}' reset to FREE.", slot_index, ctx->shm_name);
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_force_reset_slot: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_force_reset_slot: Unexpected error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }

        return RECOVERY_SUCCESS;
    }

    PYLABHUB_UTILS_EXPORT RecoveryResult datablock_force_reset_all_slots(const char *shm_name_cstr,
                                                                         bool force)
    {
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return RECOVERY_FAILED;

        pylabhub::hub::SharedMemoryHeader *header = ctx->header;
        RecoveryResult overall_result = RECOVERY_SUCCESS;

        try
        {
            uint32_t ring_buffer_capacity = header->ring_buffer_capacity;

            LOGGER_WARN("RECOVERY: Attempting to force reset ALL {} slots in '{}'. Force flag: {}.",
                        ring_buffer_capacity, ctx->shm_name, force ? "TRUE" : "FALSE");

            for (uint32_t i = 0; i < ring_buffer_capacity; ++i)
            {
                RecoveryResult result = datablock_force_reset_slot(shm_name_cstr, i, force);
                if (result != RECOVERY_SUCCESS)
                {
                    LOGGER_ERROR("datablock_force_reset_all_slots: Failed to reset slot {} in "
                                 "'{}'. Result: {}.",
                                 i, ctx->shm_name, static_cast<int>(result));
                    if (overall_result == RECOVERY_SUCCESS)
                    {
                        overall_result = result;
                    }
                }
            }
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_force_reset_all_slots: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_force_reset_all_slots: Unexpected error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }

        LOGGER_WARN("RECOVERY: Completed force reset of all slots in '{}'. Overall result: {}.",
                    ctx->shm_name, static_cast<int>(overall_result));
        return overall_result;
    }

    PYLABHUB_UTILS_EXPORT RecoveryResult datablock_release_zombie_readers(const char *shm_name_cstr,
                                                                          uint32_t slot_index,
                                                                          bool force)
    {
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return RECOVERY_FAILED;

        pylabhub::hub::SharedMemoryHeader *header = ctx->header;
        try
        {
            uint32_t ring_buffer_capacity = header->ring_buffer_capacity;
            if (slot_index >= ring_buffer_capacity)
            {
                LOGGER_ERROR(
                    "datablock_release_zombie_readers: Invalid slot_index {} for capacity {}.",
                    slot_index, ring_buffer_capacity);
                return RECOVERY_INVALID_SLOT;
            }

            pylabhub::hub::SlotRWState *rw_state = ctx->handle->slot_rw_state(slot_index);
            if (!rw_state)
            {
                LOGGER_ERROR("datablock_release_zombie_readers: Failed to get slot_rw_state for "
                             "slot {} in '{}'.",
                             slot_index, ctx->shm_name);
                return RECOVERY_FAILED;
            }

            uint64_t current_write_lock_pid = rw_state->write_lock.load(std::memory_order_acquire);
            uint32_t current_reader_count = rw_state->reader_count.load(std::memory_order_acquire);
            pylabhub::hub::SlotRWState::SlotState current_slot_state =
                rw_state->slot_state.load(std::memory_order_acquire);

            bool producer_is_alive = (current_write_lock_pid != 0) &&
                                     pylabhub::platform::is_process_alive(current_write_lock_pid);

            if (current_reader_count == 0)
            {
                LOGGER_INFO("datablock_release_zombie_readers: Slot {} has no active readers.",
                            slot_index);
                return RECOVERY_NOT_STUCK;
            }

            if (!force && producer_is_alive)
            {
                LOGGER_ERROR("datablock_release_zombie_readers: Slot {} has active readers and "
                             "producer (PID {}) is alive. Cannot release without force flag.",
                             slot_index, current_write_lock_pid);
                return RECOVERY_UNSAFE;
            }

            LOGGER_WARN("RECOVERY: Releasing zombie readers for slot {} in '{}'. State before: "
                        "{{readers={}, state={}}}. Force: {}.",
                        slot_index, ctx->shm_name, current_reader_count,
                        map_slot_state(current_slot_state), force);

            rw_state->reader_count.store(0, std::memory_order_release);
            if (current_slot_state == pylabhub::hub::SlotRWState::SlotState::DRAINING)
            {
                rw_state->slot_state.store(pylabhub::hub::SlotRWState::SlotState::FREE,
                                           std::memory_order_release);
                rw_state->writer_waiting.store(0, std::memory_order_release);
            }

            header->recovery_actions_count.fetch_add(1, std::memory_order_relaxed);
            set_recovery_timestamp(header);

            LOGGER_WARN("RECOVERY: Zombie readers for slot {} in '{}' released.", slot_index,
                        ctx->shm_name);
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_release_zombie_readers: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_release_zombie_readers: Unexpected error for '{}': {}",
                         ctx->shm_name, ex.what());
            return RECOVERY_FAILED;
        }

        return RECOVERY_SUCCESS;
    }

    PYLABHUB_UTILS_EXPORT RecoveryResult datablock_release_zombie_writer(const char *shm_name_cstr,
                                                                         uint32_t slot_index)
    {
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return RECOVERY_FAILED;

        pylabhub::hub::SharedMemoryHeader *header = ctx->header;
        try
        {
            uint32_t ring_buffer_capacity = header->ring_buffer_capacity;
            if (slot_index >= ring_buffer_capacity)
            {
                LOGGER_ERROR(
                    "datablock_release_zombie_writer: Invalid slot_index {} for capacity {}.",
                    slot_index, ring_buffer_capacity);
                return RECOVERY_INVALID_SLOT;
            }

            pylabhub::hub::SlotRWState *rw_state = ctx->handle->slot_rw_state(slot_index);
            if (!rw_state)
            {
                LOGGER_ERROR("datablock_release_zombie_writer: Failed to get slot_rw_state for "
                             "slot {} in '{}'.",
                             slot_index, ctx->shm_name);
                return RECOVERY_FAILED;
            }

            uint64_t current_write_lock_pid = rw_state->write_lock.load(std::memory_order_acquire);

            if (current_write_lock_pid == 0)
            {
                LOGGER_INFO("datablock_release_zombie_writer: Slot {} has no writer lock.",
                            slot_index);
                return RECOVERY_NOT_STUCK;
            }

            if (pylabhub::platform::is_process_alive(current_write_lock_pid))
            {
                LOGGER_ERROR("datablock_release_zombie_writer: Slot {} write lock held by ALIVE "
                             "process {}. Cannot release.",
                             slot_index, current_write_lock_pid);
                return RECOVERY_UNSAFE;
            }

            LOGGER_WARN("RECOVERY: Releasing zombie writer for slot {} in '{}'. PID {}.",
                        slot_index, ctx->shm_name, current_write_lock_pid);

            rw_state->write_lock.store(0, std::memory_order_release);
            rw_state->slot_state.store(pylabhub::hub::SlotRWState::SlotState::FREE,
                                       std::memory_order_release);
            rw_state->writer_waiting.store(0, std::memory_order_release);

            header->recovery_actions_count.fetch_add(1, std::memory_order_relaxed);
            set_recovery_timestamp(header);

            LOGGER_WARN("RECOVERY: Zombie writer for slot {} in '{}' released.", slot_index,
                        ctx->shm_name);
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_release_zombie_writer: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_release_zombie_writer: Unexpected error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }

        return RECOVERY_SUCCESS;
    }

    PYLABHUB_UTILS_EXPORT RecoveryResult datablock_cleanup_dead_consumers(const char *shm_name_cstr)
    {
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return RECOVERY_FAILED;

        pylabhub::hub::SharedMemoryHeader *header = ctx->header;
        try
        {
            LOGGER_INFO("RECOVERY: Starting cleanup of dead consumers in '{}'.", ctx->shm_name);

            int cleaned_count = 0;
            for (size_t i = 0; i < pylabhub::hub::detail::MAX_CONSUMER_HEARTBEATS; ++i)
            {
                uint64_t pid = header->consumer_heartbeats[i].consumer_id.load(
                    std::memory_order_acquire); // Corrected
                if (pid != 0 && !pylabhub::platform::is_process_alive(pid))
                {

                    uint64_t expected_pid = pid;
                    if (header->consumer_heartbeats[i].consumer_id.compare_exchange_strong(
                            expected_pid, 0, std::memory_order_acq_rel))
                    { // Corrected
                        header->active_consumer_count.fetch_sub(1, std::memory_order_relaxed);
                        cleaned_count++;
                        LOGGER_WARN("RECOVERY: Cleaned up dead consumer with PID {}.", pid);
                    }
                }
            }

            if (cleaned_count > 0)
            {
                header->recovery_actions_count.fetch_add(cleaned_count, std::memory_order_relaxed);
                set_recovery_timestamp(header);
            }

            LOGGER_INFO("RECOVERY: Finished cleanup. Removed {} dead consumers from '{}'.",
                        cleaned_count, ctx->shm_name);
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_cleanup_dead_consumers: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_cleanup_dead_consumers: Unexpected error for '{}': {}",
                         ctx->shm_name, ex.what());
            return RECOVERY_FAILED;
        }

        return RECOVERY_SUCCESS;
    }

    PYLABHUB_UTILS_EXPORT RecoveryResult datablock_validate_integrity(const char *shm_name_cstr,
                                                                      bool repair)
    {
        auto ctx = open_for_recovery(shm_name_cstr);
        if (!ctx)
            return RECOVERY_FAILED;

        pylabhub::hub::SharedMemoryHeader *header = ctx->header;
        RecoveryResult overall_result = RECOVERY_SUCCESS;

        try
        {
            LOGGER_INFO("INTEGRITY_CHECK: Starting integrity validation for '{}'. Repair mode: {}.",
                        ctx->shm_name, repair ? "ON" : "OFF");

            // 1. Magic Number
            if (!pylabhub::hub::detail::is_header_magic_valid(&header->magic_number,
                                                               pylabhub::hub::detail::DATABLOCK_MAGIC_NUMBER))
            {
                LOGGER_ERROR("INTEGRITY_CHECK: Invalid magic number! Expected {:#x}, found {:#x}.",
                             pylabhub::hub::detail::DATABLOCK_MAGIC_NUMBER,
                             header->magic_number.load(std::memory_order_acquire));
                overall_result = RECOVERY_FAILED;
                // Cannot repair this.
            }

            // 2. Header Version
            if (header->version_major != pylabhub::hub::detail::HEADER_VERSION_MAJOR ||
                header->version_minor > pylabhub::hub::detail::HEADER_VERSION_MINOR)
            {
                LOGGER_ERROR("INTEGRITY_CHECK: Version mismatch! Expected {}.{}, found {}.{}.",
                             pylabhub::hub::detail::HEADER_VERSION_MAJOR,
                             pylabhub::hub::detail::HEADER_VERSION_MINOR,
                             header->version_major, header->version_minor);
                overall_result = RECOVERY_FAILED;
                // Cannot repair this.
            }

            // 3. Build expected config from header for consumer/producer calls
            pylabhub::hub::DataBlockConfig expected_config;
            expected_config.name = ctx->shm_name;
            expected_config.ring_buffer_capacity = header->ring_buffer_capacity;
            if (header->unit_block_size == 4096u)
                expected_config.unit_block_size = pylabhub::hub::DataBlockUnitSize::Size4K;
            else if (header->unit_block_size == 4194304u)
                expected_config.unit_block_size = pylabhub::hub::DataBlockUnitSize::Size4M;
            else if (header->unit_block_size == 16777216u)
                expected_config.unit_block_size = pylabhub::hub::DataBlockUnitSize::Size16M;
            else
                expected_config.unit_block_size = pylabhub::hub::DataBlockUnitSize::Size4K;
            expected_config.policy = header->policy;
            expected_config.enable_checksum = header->enable_checksum;
            expected_config.checksum_policy = header->checksum_policy;
            uint64_t secret = 0;
            std::memcpy(&secret, header->shared_secret, sizeof(secret));
            expected_config.shared_secret = secret;

            // 4. Checksums
            if (header->enable_checksum)
            {
                auto consumer = pylabhub::hub::find_datablock_consumer(
                    pylabhub::hub::MessageHub::get_instance(), ctx->shm_name, expected_config.shared_secret,
                    expected_config);

                if (!consumer)
                {
                    LOGGER_ERROR("INTEGRITY_CHECK: Could not create a consumer to verify checksums "
                                 "for '{}'.",
                                 ctx->shm_name);
                    return RECOVERY_FAILED;
                }

                // Flexible zone checksums
                for (size_t i = 0; i < expected_config.flexible_zone_configs.size(); ++i)
                {
                        if (!consumer->verify_checksum_flexible_zone(i))
                        { // Pass index
                            LOGGER_WARN(
                                "INTEGRITY_CHECK: Flexible zone {} checksum is invalid for '{}'.", i,
                                ctx->shm_name);
                        if (repair)
                        {
                            LOGGER_WARN("REPAIR: Attempting to recalculate flexible zone {} "
                                        "checksum for '{}'.",
                                        i, ctx->shm_name);
                            auto producer = pylabhub::hub::create_datablock_producer(
                                pylabhub::hub::MessageHub::get_instance(), ctx->shm_name,
                                expected_config.policy, expected_config); // Removed schema_instance
                            if (producer && producer->update_checksum_flexible_zone(i))
                            { // Pass index
                                LOGGER_WARN("REPAIR: Successfully recalculated flexible zone {} "
                                            "checksum for '{}'.",
                                            i, ctx->shm_name);
                            }
                            else
                            {
                                LOGGER_ERROR("REPAIR: Failed to recalculate flexible zone {} "
                                             "checksum for '{}'.",
                                             i, ctx->shm_name);
                                overall_result = RECOVERY_FAILED;
                            }
                        }
                        else
                        {
                            overall_result = RECOVERY_FAILED;
                        }
                    }
                }

                // Slot checksums
                uint64_t commit_idx = header->commit_index.load(std::memory_order_acquire);
                for (uint32_t i = 0; i < header->ring_buffer_capacity; ++i)
                {
                    // Only check committed slots up to commit_index
                    if (i <= (commit_idx % header->ring_buffer_capacity))
                    {
                        if (!consumer->verify_checksum_slot(i))
                        {
                            LOGGER_WARN("INTEGRITY_CHECK: Slot {} checksum is invalid for '{}'.", i,
                                        ctx->shm_name);
                            if (repair)
                            {
                                LOGGER_WARN("REPAIR: Attempting to recalculate checksum for slot "
                                            "{} in '{}'.",
                                            i, ctx->shm_name);
                                auto producer = pylabhub::hub::create_datablock_producer(
                                    pylabhub::hub::MessageHub::get_instance(), ctx->shm_name,
                                    expected_config.policy,
                                    expected_config); // Removed schema_instance
                                if (producer && producer->update_checksum_slot(i))
                                {
                                    LOGGER_WARN("REPAIR: Successfully recalculated checksum for "
                                                "slot {} in '{}'.",
                                                i, ctx->shm_name);
                                }
                                else
                                {
                                    LOGGER_ERROR("REPAIR: Failed to recalculate checksum for slot "
                                                 "{} in '{}'.",
                                                 i, ctx->shm_name);
                                    overall_result = RECOVERY_FAILED;
                                }
                            }
                            else
                            {
                                overall_result = RECOVERY_FAILED;
                            }
                        }
                    }
                }
            }

            LOGGER_INFO("INTEGRITY_CHECK: Finished for '{}'. Overall result: {}.", ctx->shm_name,
                        static_cast<int>(overall_result));
        }
        catch (const std::runtime_error &ex)
        {
            LOGGER_ERROR("datablock_validate_integrity: Runtime error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }
        catch (const std::exception &ex)
        {
            LOGGER_ERROR("datablock_validate_integrity: Unexpected error for '{}': {}", ctx->shm_name,
                         ex.what());
            return RECOVERY_FAILED;
        }
        return overall_result;
    }
} // extern "C"