#include "utils/slot_recovery.hpp"

namespace pylabhub::hub
{

SlotRecovery::SlotRecovery(std::string shm_name, uint32_t slot_index)
    : shm_name_(std::move(shm_name)), slot_index_(slot_index)
{
}

RecoveryResult SlotRecovery::force_reset(bool force)
{
    return datablock_force_reset_slot(shm_name_.c_str(), slot_index_, force);
}

RecoveryResult SlotRecovery::release_zombie_readers(bool force)
{
    return datablock_release_zombie_readers(shm_name_.c_str(), slot_index_, force);
}

RecoveryResult SlotRecovery::release_zombie_writer()
{
    return datablock_release_zombie_writer(shm_name_.c_str(), slot_index_);
}

} // namespace pylabhub::hub
