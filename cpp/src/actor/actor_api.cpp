/**
 * @file actor_api.cpp
 * @brief ActorAPI implementation.
 */
#include "actor_api.hpp"

#include "plh_service.hpp"

#include <algorithm>

namespace pylabhub::actor
{

// ── Common ────────────────────────────────────────────────────────────────────

void ActorAPI::log(const std::string &level, const std::string &msg)
{
    if (level == "debug")
        LOGGER_DEBUG("[actor] {}", msg);
    else if (level == "warn" || level == "warning")
        LOGGER_WARN("[actor] {}", msg);
    else if (level == "error")
        LOGGER_ERROR("[actor] {}", msg);
    else
        LOGGER_INFO("[actor] {}", msg);
}

py::list ActorAPI::consumers()
{
    py::list result;
    if (producer_)
    {
        for (const auto &id : producer_->connected_consumers())
        {
            result.append(id);
        }
    }
    return result;
}

// ── Producer-side ─────────────────────────────────────────────────────────────

bool ActorAPI::broadcast(py::bytes data)
{
    if (!producer_)
    {
        return false;
    }
    const auto s = data.cast<std::string>();
    return producer_->send(s.data(), s.size());
}

bool ActorAPI::send(const std::string &identity, py::bytes data)
{
    if (!producer_)
    {
        return false;
    }
    const auto s = data.cast<std::string>();
    return producer_->send_to(identity, s.data(), s.size());
}

bool ActorAPI::update_flexzone_checksum()
{
    // ── Producer: update SHM flexzone checksum ────────────────────────────────
    if (producer_)
    {
        if (auto *shm = producer_->shm())
        {
            return shm->update_checksum_flexible_zone();
        }
        return false;
    }

    // ── Consumer: accept current SHM flexzone content as valid ───────────────
    if (consumer_)
    {
        if (auto *shm = consumer_->shm())
        {
            const auto span = shm->flexible_zone_span();
            if (span.empty())
            {
                LOGGER_WARN("[actor] update_flexzone_checksum: flexzone size is 0");
                return false;
            }
            consumer_fz_accepted_.assign(span.begin(), span.end());
            consumer_fz_has_accepted_ = true;
            LOGGER_DEBUG("[actor] consumer: flexzone state accepted ({} bytes)",
                         span.size());
            return true;
        }
    }
    return false;
}

// ── Consumer-side ─────────────────────────────────────────────────────────────

bool ActorAPI::send_ctrl(py::bytes data)
{
    if (!consumer_)
    {
        return false;
    }
    const auto s = data.cast<std::string>();
    return consumer_->send_ctrl("DATA", s.data(), s.size());
}

bool ActorAPI::verify_flexzone_checksum()
{
    if (consumer_)
    {
        if (auto *shm = consumer_->shm())
        {
            return shm->verify_checksum_flexible_zone();
        }
    }
    return false;
}

// ── Internal ──────────────────────────────────────────────────────────────────

bool ActorAPI::is_fz_accepted(std::span<const std::byte> current_fz) const noexcept
{
    if (!consumer_fz_has_accepted_)
    {
        return false;
    }
    if (current_fz.size() != consumer_fz_accepted_.size())
    {
        return false;
    }
    return std::equal(current_fz.begin(), current_fz.end(),
                      consumer_fz_accepted_.begin());
}

} // namespace pylabhub::actor
