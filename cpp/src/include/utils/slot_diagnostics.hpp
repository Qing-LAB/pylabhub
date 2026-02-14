/**
 * @file slot_diagnostics.hpp
 * @brief C++ wrapper for DataBlock slot diagnostic functions.
 */
#pragma once

#include "pylabhub_utils_export.h"
#include "utils/recovery_api.hpp"
#include <string>

namespace pylabhub::hub
{

/**
 * @class SlotDiagnostics
 * @brief Provides an object-oriented interface for slot diagnostics.
 *
 * This class wraps the C-style `datablock_diagnose_slot` function,
 * offering a convenient way to retrieve and query the state of a single
 * DataBlock slot.
 */
class PYLABHUB_UTILS_EXPORT SlotDiagnostics
{
  public:
    /**
     * @brief Constructs a diagnostics object for a specific slot.
     * @param shm_name The name of the shared memory DataBlock.
     * @param slot_index The physical index of the slot to diagnose.
     */
    SlotDiagnostics(std::string shm_name, uint32_t slot_index);

    /**
     * @brief Refreshes the diagnostic data from shared memory.
     * @return true on success, false if the slot could not be diagnosed.
     */
    [[nodiscard]] bool refresh();

    /** @return The monotonic ID of the slot. */
    uint64_t get_slot_id() const;
    /** @return The physical index of the slot. */
    uint32_t get_slot_index() const;
    /** @return The current state of the slot (see @ref SlotState). */
    uint8_t get_slot_state() const;
    /** @return The PID of the process holding the write lock, or 0 if none. */
    uint64_t get_write_lock_pid() const;
    /** @return The number of active readers. */
    uint32_t get_reader_count() const;
    /** @return A heuristic guess on whether the slot is stuck. */
    [[nodiscard]] bool is_stuck() const;
    /** @return The approximate duration in milliseconds the slot has been stuck. */
    uint64_t get_stuck_duration_ms() const;
    /** @return True if the diagnostic data is valid. */
    [[nodiscard]] bool is_valid() const;

  private:
    std::string shm_name_;
    uint32_t slot_index_;
    SlotDiagnostic diag_data_{};
    bool is_valid_ = false;
};

} // namespace pylabhub::hub
