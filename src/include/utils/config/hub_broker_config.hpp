#pragma once
/**
 * @file hub_broker_config.hpp
 * @brief HubBrokerConfig — broker timing + policy + known-roles list.
 *
 * Parsed from the top-level `"broker"` JSON sub-object (HEP-CORE-0033 §6.2).
 * Owns the heartbeat timeout / multiplier, the default channel-policy when
 * a channel registers without an explicit one, and the list of pre-known
 * roles (uid + name + pubkey) the broker should recognise at registration
 * time.  Strict key whitelist per HEP-CORE-0033 §6.3.
 */

#include "utils/json_fwd.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pylabhub::config
{

struct KnownRoleEntry
{
    std::string uid;        ///< Role uid (HEP-0033 §G2.2.0a)
    std::string name;       ///< Human-readable name
    std::string pubkey;     ///< Z85 CURVE25519 public key (40 chars), or empty
};

struct HubBrokerConfig
{
    int32_t                       heartbeat_timeout_ms{15000};
    int32_t                       heartbeat_multiplier{5};
    std::string                   default_channel_policy{"open"};
    std::vector<KnownRoleEntry>   known_roles;
};

inline HubBrokerConfig parse_hub_broker_config(const nlohmann::json &j)
{
    HubBrokerConfig bc;
    if (!j.contains("broker"))
        return bc;
    if (!j["broker"].is_object())
        throw std::runtime_error("hub: 'broker' must be an object");

    const auto &sect = j["broker"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "heartbeat_timeout_ms" && k != "heartbeat_multiplier" &&
            k != "default_channel_policy" && k != "known_roles")
            throw std::runtime_error("hub: unknown config key 'broker." + k + "'");
    }

    bc.heartbeat_timeout_ms = sect.value("heartbeat_timeout_ms",
                                          bc.heartbeat_timeout_ms);
    bc.heartbeat_multiplier = sect.value("heartbeat_multiplier",
                                          bc.heartbeat_multiplier);
    bc.default_channel_policy = sect.value("default_channel_policy",
                                            bc.default_channel_policy);

    if (bc.heartbeat_timeout_ms < 0)
        throw std::runtime_error(
            "hub: 'broker.heartbeat_timeout_ms' must be >= 0 (got " +
            std::to_string(bc.heartbeat_timeout_ms) + ")");
    if (bc.heartbeat_multiplier < 1)
        throw std::runtime_error(
            "hub: 'broker.heartbeat_multiplier' must be >= 1 (got " +
            std::to_string(bc.heartbeat_multiplier) + ")");
    if (bc.default_channel_policy != "open" &&
        bc.default_channel_policy != "named_only" &&
        bc.default_channel_policy != "closed")
        throw std::runtime_error(
            "hub: 'broker.default_channel_policy' must be 'open', "
            "'named_only', or 'closed' (got '" + bc.default_channel_policy + "')");

    if (sect.contains("known_roles"))
    {
        const auto &arr = sect["known_roles"];
        if (!arr.is_array())
            throw std::runtime_error("hub: 'broker.known_roles' must be an array");
        for (const auto &e : arr)
        {
            if (!e.is_object())
                throw std::runtime_error(
                    "hub: 'broker.known_roles[]' entry must be an object");
            for (auto it = e.begin(); it != e.end(); ++it)
            {
                const auto &k = it.key();
                if (k != "uid" && k != "name" && k != "pubkey")
                    throw std::runtime_error(
                        "hub: unknown config key 'broker.known_roles[]." + k + "'");
            }
            KnownRoleEntry kr;
            kr.uid    = e.value("uid",    std::string{});
            kr.name   = e.value("name",   std::string{});
            kr.pubkey = e.value("pubkey", std::string{});
            if (kr.uid.empty())
                throw std::runtime_error(
                    "hub: 'broker.known_roles[].uid' is required");
            bc.known_roles.push_back(std::move(kr));
        }
    }
    return bc;
}

} // namespace pylabhub::config
