#include "utils/slot_diagnostics.hpp"
#include "utils/logger.hpp"

namespace pylabhub::hub
{

SlotDiagnostics::SlotDiagnostics(const std::string &shm_name, uint32_t slot_index)
    : shm_name_(shm_name), slot_index_(slot_index)
{
    if (!refresh())
    {
        LOGGER_DEBUG("SlotDiagnostics: Initial refresh failed for slot {} in '{}' (is_valid_=false; see previous log for error code).",
                     slot_index_, shm_name_);
    }
}

bool SlotDiagnostics::refresh()
{
    int result = datablock_diagnose_slot(shm_name_.c_str(), slot_index_, &diag_data_);
    if (result != 0)
    {
        LOGGER_ERROR("SlotDiagnostics: Failed to diagnose slot {} in '{}'. Error code: {}",
                     slot_index_, shm_name_, result);
        is_valid_ = false;
        return false;
    }
    is_valid_ = true;
    return true;
}

uint64_t SlotDiagnostics::get_slot_id() const
{
    return is_valid_ ? diag_data_.slot_id : 0;
}

uint32_t SlotDiagnostics::get_slot_index() const
{
    return slot_index_;
}

uint8_t SlotDiagnostics::get_slot_state() const
{
    return is_valid_ ? diag_data_.slot_state : 0;
}

uint64_t SlotDiagnostics::get_write_lock_pid() const
{
    return is_valid_ ? diag_data_.write_lock : 0;
}

uint32_t SlotDiagnostics::get_reader_count() const
{
    return is_valid_ ? diag_data_.reader_count : 0;
}

bool SlotDiagnostics::is_stuck() const
{
    return is_valid_ ? diag_data_.is_stuck : false;
}

uint64_t SlotDiagnostics::get_stuck_duration_ms() const
{
    return is_valid_ ? diag_data_.stuck_duration_ms : 0;
}

bool SlotDiagnostics::is_valid() const
{
    return is_valid_;
}

} // namespace pylabhub::hub
