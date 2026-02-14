/**
 * @file slot_recovery.hpp
 * @brief C++ wrapper for DataBlock slot recovery functions.
 */
#pragma once

#include "pylabhub_utils_export.h"
#include "utils/recovery_api.hpp"
#include <string>

namespace pylabhub::hub
{

/**
 * @class SlotRecovery
 * @brief Provides an object-oriented interface for slot recovery actions.
 *
 * This class wraps the C-style recovery functions for a single DataBlock slot,
 * allowing for easier and safer execution of recovery operations.
 */
class PYLABHUB_UTILS_EXPORT SlotRecovery
{
  public:
    /**
     * @brief Constructs a recovery object for a specific slot.
     * @param shm_name The name of the shared memory DataBlock.
     * @param slot_index The physical index of the slot to perform recovery on.
     */
    SlotRecovery(std::string shm_name, uint32_t slot_index);

    /**
     * @brief Forcefully resets the slot's state to FREE.
     * @warning This is a DANGEROUS operation.
     * @param force If true, bypasses safety checks (e.g., if a live process holds a lock).
     * @return A `RecoveryResult` code indicating the outcome.
     */
    [[nodiscard]] RecoveryResult force_reset(bool force = false);

    /**
     * @brief Attempts to release readers that are presumed to be zombies.
     * @param force If true, clears the reader count regardless of other checks.
     * @return A `RecoveryResult` code indicating the outcome.
     */
    [[nodiscard]] RecoveryResult release_zombie_readers(bool force = false);

    /**
     * @brief Attempts to release a writer that is presumed to be a zombie.
     * @return A `RecoveryResult` code indicating the outcome.
     */
    [[nodiscard]] RecoveryResult release_zombie_writer();

  private:
    std::string shm_name_;
    uint32_t slot_index_;
};

} // namespace pylabhub::hub
