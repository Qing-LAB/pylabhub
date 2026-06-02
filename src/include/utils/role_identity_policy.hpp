#pragma once
/**
 * @file role_identity_policy.hpp
 * @brief Hub-side gate for verifying a connecting role's claimed
 *        identity at REG_REQ / CONSUMER_REG_REQ time (legacy
 *        placeholder).
 *
 * 🚧 **Superseded by HEP-CORE-0035 (in design, not implemented).**
 * The `RoleIdentityPolicy` enum + `KnownRole` + `ChannelPolicyOverride`
 * types here pre-date the CURVE-required role model and the HEP-0022
 * federation model.  They operate on self-asserted JSON identity
 * strings and never consult the connecting socket's CURVE pubkey.
 * HEP-CORE-0035 replaces them with a two-layer enforcement model
 * (ZAP pubkey allowlist at the socket layer + federation-trust gate
 * at the registration layer).  Until HEP-0035 Phase 6 retires this
 * code, the types remain live: settable directly on
 * `BrokerService::Config` by tests and (eventually) Phase 9 wiring;
 * **not** parsed from hub.json by `pylabhub::config::HubConfig` (Phase
 * 1 deliberately omits the auth fields — see HEP-0033 §15 Phase 1 +
 * HEP-0035 §3).
 *
 * What this gate actually does: at REG_REQ (producer) and
 * CONSUMER_REG_REQ (consumer), the broker reads `role_name` +
 * `role_uid` from the request body — strings the connecting role put
 * there itself — and applies one of four strictness modes based on
 * `RoleIdentityPolicy` (with optional per-channel-glob override via
 * `ChannelPolicyOverride`).  The "channel" in the legacy name
 * referred only to the glob-based override selector, NOT to what is
 * being verified.  The subject of verification is the **role's
 * identity**; "registration identity policy" is what it really is.
 *
 * Legacy semantics (do NOT treat as design):
 *   Open     — no identity required; any client connects.
 *   Tracked  — identity accepted and logged if provided; not required.
 *   Required — role_name + role_uid must be present in REG_REQ /
 *              CONSUMER_REG_REQ (non-empty strings).
 *   Verified — role_name + role_uid must string-match an entry in
 *              `known_roles[]`.
 *
 * Renamed 2026-05-13 from `channel_access_policy.hpp` /
 * `ConnectionPolicy` / `ChannelPolicy` to reflect what the mechanism
 * actually verifies.  Rename is cosmetic; the placeholder remains on
 * its HEP-0035 §8 Phase 6 deletion path.
 */
#include "pylabhub_utils_export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pylabhub::broker
{

/**
 * @enum RoleIdentityPolicy
 * @brief Strictness mode for verifying a connecting role's
 *        self-asserted identity (`role_name` + `role_uid`) at
 *        REG_REQ / CONSUMER_REG_REQ.
 *
 * Levels escalate from fully permissive (Open) to allowlist-only
 * (Verified).  A per-channel override via
 * `ChannelPolicyOverride::policy` can tighten (but not loosen) the
 * effective policy for specific channel glob patterns.
 *
 * **Where set (today):**
 *   - `BrokerService::Config::role_identity_policy` (`broker_service.hpp`).
 *     Set directly by tests (`test_datahub_role_identity_policy.cpp`).
 *     **Not** parsed from hub.json — `HubBrokerConfig` deliberately
 *     omits the field pending HEP-CORE-0035.
 * **Where checked:** `BrokerServiceImpl::check_role_identity()` in
 *   `broker_service.cpp` — called on every REG_REQ (producer) and
 *   CONSUMER_REG_REQ (consumer).  Returns an error response on
 *   rejection; std::nullopt on acceptance.
 * **Effective policy:** `BrokerServiceImpl::effective_role_identity_policy()`
 *   picks the most-restrictive matching `ChannelPolicyOverride`,
 *   falling back to the Config default.
 *
 * | Value    | role_name/uid required? | Must be in known_roles? | Suitable for   |
 * |----------|--------------------------|--------------------------|----------------|
 * | Open     | No                       | No                       | Dev/local hubs |
 * | Tracked  | Optional (logged if      | No                       | Observability  |
 * |          | provided)                |                          | and auditing   |
 * | Required | Yes (both fields)        | No                       | Deployment     |
 * | Verified | Yes (both fields)        | Yes (allowlist)          | Production     |
 *
 * **Design doc:** HEP-CORE-0035 (replacement design — supersedes this
 * placeholder).  Legacy reference: HEP-CORE-0009 §2.7.
 */
enum class RoleIdentityPolicy : uint8_t
{
    Open,     ///< No identity required. Any client connects. (default — suitable for dev)
    Tracked,  ///< Identity accepted and recorded in registry if provided; not required.
    Required, ///< role_name + role_uid must be present in REG_REQ / CONSUMER_REG_REQ.
    Verified, ///< role_name + role_uid must match an entry in known_roles allowlist.
};

/// Convert `RoleIdentityPolicy` to its JSON/config string representation.
inline constexpr const char* role_identity_policy_to_str(RoleIdentityPolicy p) noexcept
{
    switch (p)
    {
    case RoleIdentityPolicy::Tracked:  return "tracked";
    case RoleIdentityPolicy::Required: return "required";
    case RoleIdentityPolicy::Verified: return "verified";
    default:                           return "open";
    }
}

/// Parse `RoleIdentityPolicy` from a JSON/config string.  Returns Open
/// on unknown values.
inline RoleIdentityPolicy role_identity_policy_from_str(const std::string& s) noexcept
{
    if (s == "tracked")  { return RoleIdentityPolicy::Tracked; }
    if (s == "required") { return RoleIdentityPolicy::Required; }
    if (s == "verified") { return RoleIdentityPolicy::Verified; }
    return RoleIdentityPolicy::Open;
}

/// One entry in the hub's known-roles allowlist.
///
/// **Pre-2026-06-02 contract:** name + uid + role were string identity
/// claims matched against the role's self-asserted JSON body at
/// REG_REQ / CONSUMER_REG_REQ time (legacy `RoleIdentityPolicy::Verified`).
///
/// **2026-06-02 PeerAdmission Phase B contract:** `pubkey_z85` is added
/// as the **cryptographic identity anchor** — the value the broker's
/// ZAP handler will match the connecting CURVE handshake against
/// (HEP-CORE-0035 §4.5 / PeerAdmission design §6.2).  The string
/// fields name/uid/role are retained for human-readable bookkeeping
/// (used by `--list-known-roles` output and operator logs) but the
/// ZAP gate keys on `pubkey_z85` alone.
///
/// The legacy `RoleIdentityPolicy::Verified` string match (in
/// `BrokerServiceImpl::check_role_identity`) is unaffected by this
/// addition; it simply ignores `pubkey_z85`.  Phase 6 of the HEP-0035
/// retirement plan removes the legacy check entirely.
struct PYLABHUB_UTILS_EXPORT KnownRole
{
    std::string name;        ///< Role human name (e.g. "lab.daq.sensor1")
    std::string uid;         ///< Role UID string (e.g. "prod.sensor.uid12345678")
    std::string role;        ///< "producer", "consumer", or "any" (empty = "any")
    std::string pubkey_z85;  ///< CURVE public key (Z85, 40 chars).  Empty
                             ///< only during pre-Phase-B migration; new
                             ///< entries written via `--add-known-role`
                             ///< always populate this.
};

/// Per-channel override of the hub-wide `RoleIdentityPolicy` (first
/// matching glob wins).  Tightens — but does not loosen — verification
/// strictness for channel names matching the glob.
struct PYLABHUB_UTILS_EXPORT ChannelPolicyOverride
{
    std::string         channel_glob; ///< Glob pattern on channel name ('*' wildcard only)
    RoleIdentityPolicy  policy;
};

} // namespace pylabhub::broker
