#include "channel_registry.hpp"

namespace pylabhub::broker
{

bool ChannelRegistry::register_channel(const std::string& channel_name, ChannelEntry entry)
{
    auto pos = m_channels.find(channel_name);
    if (pos == m_channels.end())
    {
        // New channel — insert unconditionally
        m_channels.emplace(channel_name, std::move(entry));
        return true;
    }

    // Existing channel: check schema hash
    if (pos->second.schema_hash != entry.schema_hash)
    {
        // Schema mismatch — reject registration
        return false;
    }

    // Same schema hash — allow re-registration (producer restart)
    pos->second = std::move(entry);
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

} // namespace pylabhub::broker
