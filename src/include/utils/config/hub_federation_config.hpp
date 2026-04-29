#pragma once
/**
 * @file hub_federation_config.hpp
 * @brief HubFederationConfig — peer-hub list for federation broadcast.
 *
 * Parsed from the top-level `"federation"` JSON sub-object (HEP-CORE-0033 §6.2,
 * HEP-CORE-0022).  Strict key whitelist per HEP-CORE-0033 §6.3.
 */

#include "utils/json_fwd.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pylabhub::config
{

struct FederationPeerEntry
{
    std::string uid;       ///< Peer hub uid (HEP-0033 §G2.2.0a)
    std::string endpoint;  ///< ZMQ endpoint
    std::string pubkey;    ///< Z85 CURVE25519 public key (40 chars), or empty
};

struct HubFederationConfig
{
    bool                              enabled{false};
    std::vector<FederationPeerEntry>  peers;
    int32_t                           forward_timeout_ms{2000};
};

inline HubFederationConfig parse_hub_federation_config(const nlohmann::json &j)
{
    HubFederationConfig fc;
    if (!j.contains("federation"))
        return fc;
    if (!j["federation"].is_object())
        throw std::runtime_error("hub: 'federation' must be an object");

    const auto &sect = j["federation"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "enabled" && k != "peers" && k != "forward_timeout_ms")
            throw std::runtime_error("hub: unknown config key 'federation." + k + "'");
    }

    fc.enabled            = sect.value("enabled",            fc.enabled);
    fc.forward_timeout_ms = sect.value("forward_timeout_ms", fc.forward_timeout_ms);

    if (fc.forward_timeout_ms < 0)
        throw std::runtime_error(
            "hub: 'federation.forward_timeout_ms' must be >= 0 (got " +
            std::to_string(fc.forward_timeout_ms) + ")");

    if (sect.contains("peers"))
    {
        const auto &arr = sect["peers"];
        if (!arr.is_array())
            throw std::runtime_error("hub: 'federation.peers' must be an array");
        for (const auto &e : arr)
        {
            if (!e.is_object())
                throw std::runtime_error(
                    "hub: 'federation.peers[]' entry must be an object");
            for (auto it = e.begin(); it != e.end(); ++it)
            {
                const auto &k = it.key();
                if (k != "uid" && k != "endpoint" && k != "pubkey")
                    throw std::runtime_error(
                        "hub: unknown config key 'federation.peers[]." + k + "'");
            }
            FederationPeerEntry pe;
            pe.uid      = e.value("uid",      std::string{});
            pe.endpoint = e.value("endpoint", std::string{});
            pe.pubkey   = e.value("pubkey",   std::string{});
            if (pe.uid.empty() || pe.endpoint.empty())
                throw std::runtime_error(
                    "hub: 'federation.peers[]' requires 'uid' + 'endpoint'");
            fc.peers.push_back(std::move(pe));
        }
    }
    return fc;
}

} // namespace pylabhub::config
