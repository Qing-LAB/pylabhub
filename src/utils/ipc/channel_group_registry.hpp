#pragma once
/**
 * @file channel_group_registry.hpp
 * @brief ChannelGroupRegistry — broker-internal pub/sub channel member tracking.
 *
 * Manages named messaging groups where any role can join, leave, and send
 * JSON messages to all members. The broker does fan-out delivery.
 *
 * Separate from ChannelRegistry (which tracks data plane registrations).
 *
 * ACCESS DISCIPLINE: Same as ChannelRegistry — accessed only from the
 * BrokerService run() thread under m_query_mu. No internal locking.
 *
 * See HEP-CORE-0030 for the full protocol specification.
 */

#include "utils/json_fwd.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pylabhub::broker
{

struct ChannelMember
{
    std::string role_uid;
    std::string role_name;
    std::string zmq_identity;   ///< ROUTER identity for push notifications
    std::chrono::steady_clock::time_point joined_at{std::chrono::steady_clock::now()};
};

struct ChannelGroup
{
    std::string name;
    std::vector<ChannelMember> members;
    std::chrono::steady_clock::time_point created_at{std::chrono::steady_clock::now()};
};

class ChannelGroupRegistry
{
  public:
    /// Join a channel. Creates the channel if it doesn't exist.
    /// Returns true if the member was added (false if already a member).
    bool join(const std::string &channel,
              const std::string &role_uid,
              const std::string &role_name,
              const std::string &zmq_identity);

    /// Leave a channel. Returns true if the member was found and removed.
    /// Deletes the channel if it becomes empty.
    bool leave(const std::string &channel, const std::string &role_uid);

    /// Remove a role from ALL channels it belongs to.
    /// Returns list of (channel_name, role_uid) pairs for notification.
    std::vector<std::string> remove_from_all(const std::string &role_uid);

    /// Get member list for a channel as JSON array.
    /// Returns nullopt if channel doesn't exist.
    [[nodiscard]] std::optional<nlohmann::json>
    members_json(const std::string &channel) const;

    /// Get ZMQ identities of all members (optionally excluding one).
    [[nodiscard]] std::vector<std::string>
    member_identities(const std::string &channel,
                      const std::string &exclude_uid = {}) const;

    /// Check if a channel exists.
    [[nodiscard]] bool exists(const std::string &channel) const;

    /// Check if a role is a member of a channel.
    [[nodiscard]] bool is_member(const std::string &channel,
                                 const std::string &role_uid) const;

    /// Number of channels.
    [[nodiscard]] size_t channel_count() const { return groups_.size(); }

  private:
    std::unordered_map<std::string, ChannelGroup> groups_;
};

} // namespace pylabhub::broker
