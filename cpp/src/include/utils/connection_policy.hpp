#pragma once
/**
 * @file connection_policy.hpp
 * @brief Hub connection policy types shared by BrokerService and HubConfig.
 *
 * Defined once here (analogous to channel_pattern.hpp) to avoid coupling between
 * hub_config.hpp and broker_service.hpp.
 *
 * Policy levels escalate from open (no checks) to verified (allowlist enforcement):
 *
 *   Open     — no identity required; any client connects (default for dev)
 *   Tracked  — identity accepted and recorded if provided; not required
 *   Required — actor_name + actor_uid must be present in every REG_REQ / CONSUMER_REG_REQ
 *   Verified — actor_name + actor_uid must match an entry in the hub's known_actors list
 */
#include "pylabhub_utils_export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::broker
{

/// How the broker validates incoming producer/consumer registrations.
enum class ConnectionPolicy : uint8_t
{
    Open,     ///< No identity required. Any client connects. (default)
    Tracked,  ///< Identity accepted and recorded if provided; not required.
    Required, ///< actor_name + actor_uid must be present in the request.
    Verified, ///< actor_name + actor_uid must match a known_actors entry.
};

/// Convert ConnectionPolicy to its JSON/config string representation.
inline constexpr const char* connection_policy_to_str(ConnectionPolicy p) noexcept
{
    switch (p)
    {
    case ConnectionPolicy::Tracked:  return "tracked";
    case ConnectionPolicy::Required: return "required";
    case ConnectionPolicy::Verified: return "verified";
    default:                         return "open";
    }
}

/// Parse ConnectionPolicy from a JSON/config string. Returns Open on unknown values.
inline ConnectionPolicy connection_policy_from_str(const std::string& s) noexcept
{
    if (s == "tracked")  { return ConnectionPolicy::Tracked; }
    if (s == "required") { return ConnectionPolicy::Required; }
    if (s == "verified") { return ConnectionPolicy::Verified; }
    return ConnectionPolicy::Open;
}

/// One entry in the hub's known-actors allowlist (hub.json::known_actors).
struct PYLABHUB_UTILS_EXPORT KnownActor
{
    std::string name; ///< Actor human name (e.g. "lab.daq.sensor1")
    std::string uid;  ///< Actor UID string (e.g. "ACTOR-Sensor-12345678")
    std::string role; ///< "producer", "consumer", or "any" (empty = "any")
};

/// Per-channel connection policy override (first match wins).
struct PYLABHUB_UTILS_EXPORT ChannelPolicy
{
    std::string      channel_glob; ///< Glob pattern on channel name ('*' wildcard only)
    ConnectionPolicy policy;
};

} // namespace pylabhub::broker
