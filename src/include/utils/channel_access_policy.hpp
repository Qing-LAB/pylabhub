#pragma once
/**
 * @file channel_access_policy.hpp
 * @brief Hub registration access-control placeholder (legacy).
 *
 * 🚧 **Superseded by HEP-CORE-0035 (in design, not implemented).**
 * The `ConnectionPolicy` enum + `KnownRole` + `ChannelPolicy` types here
 * pre-date the CURVE-required role model and the HEP-0022 federation
 * model.  They operate on self-asserted JSON identity strings and never
 * consult the connecting socket's CURVE pubkey.  HEP-CORE-0035 replaces
 * them with a two-layer enforcement model (ZAP pubkey allowlist +
 * federation-trust gate).  Until HEP-0035 Phase 6 retires this code,
 * the types remain live: settable directly on `BrokerService::Config`
 * by tests and (eventually) Phase 9 wiring; **not** parsed from hub.json
 * by `pylabhub::config::HubConfig` (Phase 1 deliberately omits the
 * auth fields — see HEP-0033 §15 Phase 1 + HEP-0035 §3).
 *
 * Legacy semantics (do NOT treat as design):
 *   Open     — no identity required; any client connects.
 *   Tracked  — identity accepted and recorded if provided; not required.
 *   Required — role_name + role_uid must be present in REG_REQ / CONSUMER_REG_REQ.
 *   Verified — role_name + role_uid must string-match an entry in `known_roles`.
 */
#include "pylabhub_utils_export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::broker
{

/**
 * @enum ConnectionPolicy
 * @brief Hub-wide policy for validating incoming producer/consumer registrations.
 *
 * Levels escalate from fully permissive (Open) to allowlist-only (Verified).
 * A per-channel override via ChannelPolicy::policy can tighten (but not loosen)
 * the effective policy for specific channel glob patterns.
 *
 * **Where set (today):**
 *   - `BrokerService::Config::connection_policy` (`broker_service.hpp`).
 *     Set directly by tests (`test_datahub_channel_access_policy.cpp`).
 *     **Not** parsed from hub.json — `HubBrokerConfig` deliberately omits
 *     the field pending HEP-CORE-0035.
 * **Where checked:** BrokerServiceImpl::check_connection_policy() in
 *   broker_service.cpp — called on every REG_REQ (producer) and CONSUMER_REG_REQ
 *   (consumer). Returns error string on rejection; empty string on acceptance.
 * **Effective policy:** BrokerServiceImpl::effective_policy() picks the
 *   most-restrictive matching ChannelPolicy override, falling back to Config default.
 *
 * | Value    | role_name/uid required? | Must be in known_roles? | Suitable for   |
 * |----------|--------------------------|--------------------------|----------------|
 * | Open     | No                       | No                       | Dev/local hubs |
 * | Tracked  | Optional (if provided,   | No                       | Observability  |
 * |          | stored in registry)      |                          | and auditing   |
 * | Required | Yes (both fields)        | No                       | Deployment     |
 * | Verified | Yes (both fields)        | Yes (allowlist)          | Production     |
 *
 * JSON hub.json key: `"connection_policy": "open"` | `"tracked"` | `"required"` | `"verified"`.
 * **Design doc:** HEP-CORE-0010-Role-Thread-Model.md §5.3 (Phase 3)
 */
enum class ConnectionPolicy : uint8_t
{
    Open,     ///< No identity required. Any client connects. (default — suitable for dev)
    Tracked,  ///< Identity accepted and recorded in registry if provided; not required.
    Required, ///< role_name + role_uid must be present in REG_REQ / CONSUMER_REG_REQ.
    Verified, ///< role_name + role_uid must match an entry in known_roles allowlist.
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

/// One entry in the hub's known-roles allowlist (hub.json::known_roles).
struct PYLABHUB_UTILS_EXPORT KnownRole
{
    std::string name; ///< Role human name (e.g. "lab.daq.sensor1")
    std::string uid;  ///< Role UID string (e.g. "prod.sensor.uid12345678", "cons.logger.uid9e1d4c2a")
    std::string role; ///< "producer", "consumer", or "any" (empty = "any")
};

/// Per-channel connection policy override (first match wins).
struct PYLABHUB_UTILS_EXPORT ChannelPolicy
{
    std::string      channel_glob; ///< Glob pattern on channel name ('*' wildcard only)
    ConnectionPolicy policy;
};

} // namespace pylabhub::broker
