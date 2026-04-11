/**
 * @file channel_group_registry.cpp
 * @brief ChannelGroupRegistry implementation.
 */
#include "channel_group_registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace pylabhub::broker
{

bool ChannelGroupRegistry::join(const std::string &channel,
                                 const std::string &role_uid,
                                 const std::string &role_name,
                                 const std::string &zmq_identity)
{
    auto &group = groups_[channel]; // auto-creates if missing
    if (group.name.empty())
        group.name = channel;

    // Idempotent: if already a member, update identity (reconnect case).
    for (auto &m : group.members)
    {
        if (m.role_uid == role_uid)
        {
            m.zmq_identity = zmq_identity;
            m.role_name = role_name;
            return false; // already member
        }
    }

    group.members.push_back({role_uid, role_name, zmq_identity});
    return true; // new member added
}

bool ChannelGroupRegistry::leave(const std::string &channel,
                                  const std::string &role_uid)
{
    auto it = groups_.find(channel);
    if (it == groups_.end())
        return false;

    auto &members = it->second.members;
    auto mit = std::find_if(members.begin(), members.end(),
        [&](const ChannelMember &m) { return m.role_uid == role_uid; });

    if (mit == members.end())
        return false;

    members.erase(mit);

    // Delete channel if empty.
    if (members.empty())
        groups_.erase(it);

    return true;
}

std::vector<std::string>
ChannelGroupRegistry::remove_from_all(const std::string &role_uid)
{
    std::vector<std::string> affected_channels;

    for (auto it = groups_.begin(); it != groups_.end(); )
    {
        auto &members = it->second.members;
        auto mit = std::find_if(members.begin(), members.end(),
            [&](const ChannelMember &m) { return m.role_uid == role_uid; });

        if (mit != members.end())
        {
            affected_channels.push_back(it->first);
            members.erase(mit);

            if (members.empty())
            {
                it = groups_.erase(it);
                continue;
            }
        }
        ++it;
    }

    return affected_channels;
}

std::optional<nlohmann::json>
ChannelGroupRegistry::members_json(const std::string &channel) const
{
    auto it = groups_.find(channel);
    if (it == groups_.end())
        return std::nullopt;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto &m : it->second.members)
    {
        arr.push_back({
            {"role_uid", m.role_uid},
            {"role_name", m.role_name}
        });
    }
    return arr;
}

std::vector<std::string>
ChannelGroupRegistry::member_identities(const std::string &channel,
                                         const std::string &exclude_uid) const
{
    std::vector<std::string> ids;
    auto it = groups_.find(channel);
    if (it == groups_.end())
        return ids;

    for (const auto &m : it->second.members)
    {
        if (!exclude_uid.empty() && m.role_uid == exclude_uid)
            continue;
        ids.push_back(m.zmq_identity);
    }
    return ids;
}

bool ChannelGroupRegistry::exists(const std::string &channel) const
{
    return groups_.count(channel) > 0;
}

bool ChannelGroupRegistry::is_member(const std::string &channel,
                                      const std::string &role_uid) const
{
    auto it = groups_.find(channel);
    if (it == groups_.end())
        return false;

    return std::any_of(it->second.members.begin(), it->second.members.end(),
        [&](const ChannelMember &m) { return m.role_uid == role_uid; });
}

} // namespace pylabhub::broker
