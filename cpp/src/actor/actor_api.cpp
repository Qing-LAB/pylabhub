/**
 * @file actor_api.cpp
 * @brief ActorRoleAPI implementation.
 */
#include "actor_api.hpp"

#include "plh_datahub.hpp" // LOGGER_*

#include <algorithm>

namespace pylabhub::actor
{

// ── Internal ──────────────────────────────────────────────────────────────────

bool ActorRoleAPI::is_fz_accepted(std::span<const std::byte> current_fz) const noexcept
{
    if (!consumer_fz_has_accepted_)
        return false;
    if (consumer_fz_accepted_.size() != current_fz.size())
        return false;
    return std::equal(current_fz.begin(), current_fz.end(),
                      consumer_fz_accepted_.begin());
}

// ── Common ────────────────────────────────────────────────────────────────────

void ActorRoleAPI::log(const std::string &level, const std::string &msg)
{
    if      (level == "debug") LOGGER_DEBUG("[actor/{}] {}", role_name_, msg);
    else if (level == "warn")  LOGGER_WARN ("[actor/{}] {}", role_name_, msg);
    else if (level == "error") LOGGER_ERROR("[actor/{}] {}", role_name_, msg);
    else                       LOGGER_INFO ("[actor/{}] {}", role_name_, msg);
}

void ActorRoleAPI::stop()
{
    if (shutdown_flag_ != nullptr)
        shutdown_flag_->store(true, std::memory_order_relaxed);
}

void ActorRoleAPI::set_critical_error()
{
    critical_error_.store(true, std::memory_order_relaxed);
    stop();
}

py::object ActorRoleAPI::flexzone() const
{
    if (flexzone_obj_ != nullptr && !flexzone_obj_->is_none())
        return *flexzone_obj_;
    return py::none();
}

// ── Producer ──────────────────────────────────────────────────────────────────

bool ActorRoleAPI::broadcast(py::bytes data)
{
    if (producer_ == nullptr)
        return false;
    const auto s = data.cast<std::string>();
    return producer_->send(s.data(), s.size());
}

bool ActorRoleAPI::send(const std::string &identity, py::bytes data)
{
    if (producer_ == nullptr)
        return false;
    const auto s = data.cast<std::string>();
    return producer_->send_to(identity, s.data(), s.size());
}

py::list ActorRoleAPI::consumers()
{
    py::list result;
    if (producer_ != nullptr)
    {
        for (const auto &id : producer_->connected_consumers())
            result.append(id);
    }
    return result;
}

bool ActorRoleAPI::update_flexzone_checksum()
{
    if (producer_ == nullptr)
        return false;
    auto *shm = producer_->shm();
    if (shm == nullptr)
        return false;
    return shm->update_checksum_flexible_zone();
}

// ── Consumer ──────────────────────────────────────────────────────────────────

bool ActorRoleAPI::send_ctrl(py::bytes data)
{
    if (consumer_ == nullptr)
        return false;
    const auto s = data.cast<std::string>();
    return consumer_->send_ctrl("DATA", s.data(), s.size());
}

bool ActorRoleAPI::verify_flexzone_checksum()
{
    if (consumer_ == nullptr)
        return false;
    auto *shm = consumer_->shm();
    if (shm == nullptr)
        return false;
    return shm->verify_checksum_flexible_zone();
}

bool ActorRoleAPI::accept_flexzone_state()
{
    if (consumer_ == nullptr)
        return false;
    auto *shm = consumer_->shm();
    if (shm == nullptr)
        return false;
    const auto span = shm->flexible_zone_span();
    if (span.empty())
        return false;
    consumer_fz_accepted_.assign(span.begin(), span.end());
    consumer_fz_has_accepted_ = true;
    LOGGER_DEBUG("[actor/{}] flexzone state accepted ({} bytes)",
                 role_name_, span.size());
    return true;
}

// ── Shared spinlocks ──────────────────────────────────────────────────────────

py::object ActorRoleAPI::spinlock(std::size_t index)
{
    if (producer_ != nullptr && producer_->shm() != nullptr)
    {
        return py::cast(SharedSpinLockPy{producer_->shm()->get_spinlock(index)});
    }
    if (consumer_ != nullptr && consumer_->shm() != nullptr)
    {
        return py::cast(SharedSpinLockPy{consumer_->shm()->get_spinlock(index)});
    }
    throw py::value_error(
        "spinlock(): SHM not configured for role '" + role_name_ +
        "' (set shm.enabled = true in the role config)");
}

uint32_t ActorRoleAPI::spinlock_count() const noexcept
{
    if (producer_ != nullptr && producer_->shm() != nullptr)
    {
        return producer_->shm()->spinlock_count();
    }
    if (consumer_ != nullptr && consumer_->shm() != nullptr)
    {
        return consumer_->shm()->spinlock_count();
    }
    return 0;
}

// ── Metrics (HEP-CORE-0008) ───────────────────────────────────────────────────

py::dict ActorRoleAPI::metrics() const
{
    // Gather DataBlock Pimpl metrics (Domains 2+3) from producer or consumer SHM handle.
    const hub::ContextMetrics *cm = nullptr;
    hub::ContextMetrics cm_consumer_copy{};

    if (producer_ != nullptr && producer_->shm() != nullptr)
    {
        cm = &producer_->shm()->metrics();
    }
    else if (consumer_ != nullptr && consumer_->shm() != nullptr)
    {
        cm_consumer_copy = consumer_->shm()->metrics();
        cm = &cm_consumer_copy;
    }

    py::dict d;
    if (cm != nullptr)
    {
        d["context_elapsed_us"] = py::int_(cm->context_elapsed_us);
        d["iteration_count"]    = py::int_(cm->iteration_count);
        d["last_iteration_us"]  = py::int_(cm->last_iteration_us);
        d["max_iteration_us"]   = py::int_(cm->max_iteration_us);
        d["last_slot_wait_us"]  = py::int_(cm->last_slot_wait_us);
        d["overrun_count"]      = py::int_(cm->overrun_count);
        d["last_slot_work_us"]  = py::int_(cm->last_slot_work_us);
        d["period_ms"]          = py::int_(cm->period_ms);
    }
    else
    {
        // No SHM: return zeroes so Python code can always access keys unconditionally.
        d["context_elapsed_us"] = py::int_(0);
        d["iteration_count"]    = py::int_(0);
        d["last_iteration_us"]  = py::int_(0);
        d["max_iteration_us"]   = py::int_(0);
        d["last_slot_wait_us"]  = py::int_(0);
        d["overrun_count"]      = py::int_(0);
        d["last_slot_work_us"]  = py::int_(0);
        d["period_ms"]          = py::int_(0);
    }

    // Domain 4 — actor-layer RoleMetrics (always available)
    d["script_error_count"] = py::int_(metrics_.script_errors);

    return d;
}

} // namespace pylabhub::actor
