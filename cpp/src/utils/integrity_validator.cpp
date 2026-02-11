#include "plh_integrity_validator.hpp"

namespace pylabhub::hub
{

IntegrityValidator::IntegrityValidator(const std::string &shm_name) : shm_name_(shm_name) {}

RecoveryResult IntegrityValidator::validate(bool repair)
{
    return datablock_validate_integrity(shm_name_.c_str(), repair);
}

} // namespace pylabhub::hub
