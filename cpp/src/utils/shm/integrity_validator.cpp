#include "utils/integrity_validator.hpp"

namespace pylabhub::hub
{

IntegrityValidator::IntegrityValidator(std::string shm_name) : shm_name_(std::move(shm_name)) {}

RecoveryResult IntegrityValidator::validate(bool repair)
{
    return datablock_validate_integrity(shm_name_.c_str(), repair);
}

} // namespace pylabhub::hub
