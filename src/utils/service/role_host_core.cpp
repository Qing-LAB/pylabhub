// src/scripting/role_host_core.cpp
/**
 * @file role_host_core.cpp
 * @brief RoleHostCore — engine-agnostic infrastructure implementation.
 */
#include "utils/role_host_core.hpp"

#include "utils/hub_inbox_queue.hpp"
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
    // Move elements individually + clear() rather than swap() so that
    // incoming_queue_ retains its allocated capacity across drain cycles.
    // With kMaxIncomingQueue=64, this avoids a heap allocation on every
    // drain-then-refill cycle (swap would reset capacity to zero).
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

// ============================================================================
// Inbox cache
// ============================================================================

std::shared_ptr<hub::InboxClient>
RoleHostCore::get_inbox_client(const std::string &target_uid) const
{
    std::lock_guard lk(inbox_cache_mu_);
    auto it = inbox_cache_.find(target_uid);
    if (it != inbox_cache_.end())
        return it->second.client;
    return nullptr;
}

std::optional<RoleHostCore::InboxCacheEntry>
RoleHostCore::get_inbox_entry(const std::string &target_uid) const
{
    std::lock_guard lk(inbox_cache_mu_);
    auto it = inbox_cache_.find(target_uid);
    if (it != inbox_cache_.end())
        return it->second;
    return std::nullopt;
}

void RoleHostCore::set_inbox_entry(const std::string &target_uid,
                                    InboxCacheEntry entry)
{
    std::lock_guard lk(inbox_cache_mu_);
    inbox_cache_[target_uid] = std::move(entry);
}

void RoleHostCore::clear_inbox_cache()
{
    std::lock_guard lk(inbox_cache_mu_);
    for (auto &[uid, entry] : inbox_cache_)
    {
        if (entry.client)
            entry.client->stop();
    }
    inbox_cache_.clear();
}

// ============================================================================
// Shared script data
// ============================================================================

std::optional<RoleHostCore::StateValue>
RoleHostCore::get_shared_data(const std::string &key) const
{
    std::shared_lock lk(shared_data_mu_);
    auto it = shared_data_.find(key);
    if (it != shared_data_.end())
        return it->second;
    return std::nullopt;
}

void RoleHostCore::set_shared_data(const std::string &key, StateValue value)
{
    std::unique_lock lk(shared_data_mu_);
    shared_data_[key] = std::move(value);
}

void RoleHostCore::remove_shared_data(const std::string &key)
{
    std::unique_lock lk(shared_data_mu_);
    shared_data_.erase(key);
}

void RoleHostCore::clear_shared_data()
{
    std::unique_lock lk(shared_data_mu_);
    shared_data_.clear();
}

} // namespace pylabhub::scripting
