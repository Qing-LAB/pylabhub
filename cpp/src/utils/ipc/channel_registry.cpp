#include "channel_registry.hpp"

#include <algorithm>

namespace pylabhub::broker
{

bool ChannelRegistry::register_channel(const std::string& channel_name, ChannelEntry entry)
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        // New channel — insert unconditionally.
        // last_heartbeat is initialised to now() by the ChannelEntry default member initialiser.
        m_channels.emplace(channel_name, std::move(entry));
        return true;
    }

    // Existing channel: check schema hash
    if (pos->second.schema_hash != entry.schema_hash)
    {
        // Schema mismatch — reject registration
        return false;
    }

    // Same schema hash — allow re-registration (producer restart).
    // Preserve existing consumers so they are still notified on close.
    auto existing_consumers = std::move(pos->second.consumers);
    pos->second = std::move(entry);
    pos->second.consumers = std::move(existing_consumers);
    return true;
}

std::optional<ChannelEntry> ChannelRegistry::find_channel(const std::string& channel_name) const
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        return std::nullopt;
    }
    return pos->second;
}

bool ChannelRegistry::deregister_channel(const std::string& channel_name, uint64_t producer_pid)
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        return false;
    }
    if (pos->second.producer_pid != producer_pid)
    {
        return false;
    }
    m_channels.erase(pos);
    return true;
}

bool ChannelRegistry::register_consumer(const std::string& channel_name, ConsumerEntry entry)
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        return false;
    }
    pos->second.consumers.push_back(std::move(entry));
    return true;
}

bool ChannelRegistry::deregister_consumer(const std::string& channel_name, uint64_t consumer_pid)
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        return false;
    }
    auto& consumers = pos->second.consumers;
    auto it = std::find_if(consumers.begin(), consumers.end(),
                           [consumer_pid](const ConsumerEntry& e)
                           { return e.consumer_pid == consumer_pid; });
    if (it == consumers.end())
    {
        return false;
    }
    consumers.erase(it);
    return true;
}

std::vector<ConsumerEntry> ChannelRegistry::find_consumers(const std::string& channel_name) const
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        return {};
    }
    return pos->second.consumers;
}

bool ChannelRegistry::update_heartbeat(const std::string& channel_name)
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        return false;
    }
    pos->second.last_heartbeat = std::chrono::steady_clock::now();
    if (pos->second.status == ChannelStatus::PendingReady)
    {
        pos->second.status = ChannelStatus::Ready;
    }
    return true;
}

std::vector<std::string> ChannelRegistry::find_timed_out_channels(
    std::chrono::steady_clock::duration timeout) const
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> result;
    for (const auto& [name, entry] : m_channels)
    {
        // Time out all channels (PendingReady or Ready) that have not sent a heartbeat
        // within the timeout window. PendingReady channels use their registration time
        // (stored in last_heartbeat) as the baseline, so they get the same grace period.
        if (now - entry.last_heartbeat >= timeout)
        {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<std::string> ChannelRegistry::list_channels() const
{
    std::vector<std::string> names;
    names.reserve(m_channels.size());
    for (const auto& [name, _] : m_channels)
    {
        names.push_back(name);
    }
    return names;
}

size_t ChannelRegistry::size() const
{
    return m_channels.size();
}

ChannelEntry* ChannelRegistry::find_channel_mutable(const std::string& channel_name) noexcept
{
    auto it = m_channels.find(channel_name);
    return (it != m_channels.end()) ? &it->second : nullptr;
}

std::unordered_map<std::string, ChannelEntry>& ChannelRegistry::all_channels() noexcept
{
    return m_channels;
}

} // namespace pylabhub::broker
