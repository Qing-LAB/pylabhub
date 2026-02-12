/**
 * @file integrity_validator.hpp
 * @brief C++ wrapper for DataBlock integrity validation.
 */
#pragma once

#include "pylabhub_utils_export.h"
#include "utils/recovery_api.hpp"
#include <string>

namespace pylabhub::hub
{

/**
 * @class IntegrityValidator
 * @brief Provides an object-oriented interface for DataBlock integrity validation.
 *
 * This class wraps the C-style `datablock_validate_integrity` function,
 * allowing for easy validation and optional repair of a DataBlock's
 * control structures and checksums.
 */
class PYLABHUB_UTILS_EXPORT IntegrityValidator
{
  public:
    /**
     * @brief Constructs an integrity validator for a specific DataBlock.
     * @param shm_name The name of the shared memory DataBlock.
     */
    explicit IntegrityValidator(const std::string &shm_name);

    /**
     * @brief Validates the integrity of the DataBlock.
     *
     * This method checks the DataBlock's magic number, header version, and checksums.
     *
     * @param repair If true, attempts to repair detected issues, such as by
     *               recalculating invalid checksums.
     * @return A `RecoveryResult` code indicating the outcome.
     */
    RecoveryResult validate(bool repair = false);

  private:
    std::string shm_name_;
};

} // namespace pylabhub::hub
