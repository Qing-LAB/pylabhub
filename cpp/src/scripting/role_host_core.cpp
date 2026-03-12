// src/scripting/role_host_core.cpp
/**
 * @file role_host_core.cpp
 * @brief RoleHostCore — engine-agnostic infrastructure implementation.
 */
#include "role_host_core.hpp"

#include "utils/logger.hpp"

#include <chrono>
#include <utility>

namespace pylabhub::scripting
{

void RoleHostCore::enqueue_message(IncomingMessage msg)
{
    {
        std::unique_lock<std::mutex> lk(incoming_mu_);
        if (incoming_queue_.size() >= kMaxIncomingQueue)
        {
            LOGGER_WARN("[RoleHostCore] Incoming queue full — dropping message");
            return;
        }
        incoming_queue_.push_back(std::move(msg));
    }
    incoming_cv_.notify_one(); // wake any wait_for_incoming() waiter
}

std::vector<IncomingMessage> RoleHostCore::drain_messages()
{
    std::vector<IncomingMessage> msgs;
    std::unique_lock<std::mutex> lk(incoming_mu_);
    if (!incoming_queue_.empty())
    {
        msgs.reserve(incoming_queue_.size());
        for (auto &m : incoming_queue_)
            msgs.push_back(std::move(m));
        incoming_queue_.clear();
    }
    return msgs;
}


void RoleHostCore::notify_incoming() noexcept
{
    incoming_cv_.notify_all();
}

void RoleHostCore::wait_for_incoming(int timeout_ms) noexcept
{
    std::unique_lock<std::mutex> lk(incoming_mu_);
    incoming_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms));
}

} // namespace pylabhub::scripting
