#pragma once
/**
 * @file hub_broker_config.hpp
 * @brief HubBrokerConfig — broker timing parameters.
 *
 * Parsed from the top-level `"broker"` JSON sub-object (HEP-CORE-0033 §6.2).
 * Owns the heartbeat timeout / multiplier.  Strict key whitelist per
 * HEP-CORE-0033 §6.3.
 *
 * **Auth/access fields deliberately omitted** (`default_channel_policy`,
 * `known_roles`, `channel_policies`).  The legacy `pylabhub::HubConfig`
 * carried these, but the implementation pre-dates the current CURVE-required
 * role model and the HEP-0022 federation-trust model.  See
 * `docs/tech_draft/hub_role_auth_design.md` for the design that must land
 * before these fields return — it covers ZAP-level pubkey allowlisting
 * (`known_roles[].pubkey`) and federation-delegated trust over
 * `federation.peers[]`.  Until that design exists, the broker-service
 * `BrokerService::Config` continues to carry the legacy `connection_policy`
 * + `known_roles` fields directly (set by Phase 9 wiring or by tests),
 * unchanged.
 */

#include "utils/json_fwd.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct HubBrokerConfig
{
    int32_t heartbeat_timeout_ms{15000};
    int32_t heartbeat_multiplier{5};
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
        if (k != "heartbeat_timeout_ms" && k != "heartbeat_multiplier")
            throw std::runtime_error("hub: unknown config key 'broker." + k + "'");
    }

    bc.heartbeat_timeout_ms = sect.value("heartbeat_timeout_ms",
                                          bc.heartbeat_timeout_ms);
    bc.heartbeat_multiplier = sect.value("heartbeat_multiplier",
                                          bc.heartbeat_multiplier);

    if (bc.heartbeat_timeout_ms < 0)
        throw std::runtime_error(
            "hub: 'broker.heartbeat_timeout_ms' must be >= 0 (got " +
            std::to_string(bc.heartbeat_timeout_ms) + ")");
    if (bc.heartbeat_multiplier < 1)
        throw std::runtime_error(
            "hub: 'broker.heartbeat_multiplier' must be >= 1 (got " +
            std::to_string(bc.heartbeat_multiplier) + ")");
    return bc;
}

} // namespace pylabhub::config
