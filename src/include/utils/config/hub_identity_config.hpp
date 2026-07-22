#pragma once
/**
 * @file hub_identity_config.hpp
 * @brief HubIdentityConfig — categorical config for hub identity.
 *
 * Parsed from the `"hub"` JSON sub-object (HEP-CORE-0033 §6.2).  Mirrors
 * the role-side `IdentityConfig` (`identity_config.hpp`) but reads the
 * `"hub"` section instead of a role-tag section, and auto-generates the
 * UID with prefix `HUB-` per HEP-CORE-0033 §G2.2.0a.
 *
 * The sibling `"hub.auth"` sub-object is parsed by the existing
 * role-side `AuthConfig` (`auth_config.hpp`); this header handles only
 * identity fields.
 */

#include "utils/json_fwd.hpp"
#include "utils/naming.hpp"
#include "utils/uid_utils.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct HubIdentityConfig
{
    std::string uid;               ///< Hub UID, e.g. "hub.main.uid12345678"
    std::string name;              ///< Human-readable name
    std::string log_level{"info"}; ///< "debug" | "info" | "warn" | "error"
};

/// Parse hub identity from the "hub" JSON sub-object.
///
/// **Strict key whitelist (HEP-0033 §6.3):** unknown keys inside `hub`
/// throw `std::runtime_error("hub: unknown config key 'hub.<name>'")`.
/// Allowed sub-keys: `uid`, `name`, `log_level`, `auth`.  The `auth`
/// key is consumed by `parse_auth_config` (separate parser); this
/// function ignores it but accepts it as known.
///
/// **UID auto-generation (HEP-0033 §G2.2.0a):** absent `hub.uid`
/// triggers `hub.<name>.uid<8hex>` generation via `uid::generate_hub_uid`
/// with a stderr diagnostic so the operator can paste the value back
/// for stability (HEP-0033 §6.3).  Note: the HEP-0033 §6.2 worked
/// example shows the legacy "HUB-NAME-HEX" form for documentation
/// continuity; the canonical wire form is the §G2.2.0a `hub.<name>.uid<8hex>`
/// form that `IdentifierKind::PeerUid` validates against.
inline HubIdentityConfig parse_hub_identity_config(const nlohmann::json &j)
{
    if (!j.contains("hub") || !j["hub"].is_object())
        throw std::runtime_error("hub: missing required 'hub' object");

    const auto &sect = j["hub"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "uid" && k != "name" && k != "log_level" && k != "auth")
            throw std::runtime_error("hub: unknown config key 'hub." + k + "'");
    }

    HubIdentityConfig hc;
    hc.name = sect.value("name", std::string{});
    hc.log_level = sect.value("log_level", std::string{"info"});
    hc.uid = sect.value("uid", std::string{});

    // HEP-CORE-0033 §6.3 (revised 2026-06-04): empty or absent
    // `hub.uid` is a hard config-load error.  The silent-auto-gen
    // fallback was retired alongside the §6.5 gatekeeper model —
    // a hub whose identity was minted by a silent fallback is the
    // exact failure mode `--skeleton` / `--init` are designed to
    // prevent.  Auto-generation survives only as a UX feature in
    // the `--init` interactive prompt helper (visible inline as
    // `[default: ...]`).
    if (hc.uid.empty())
    {
        throw std::runtime_error("hub: 'hub.uid' is empty or missing.  Run `plh_hub --init "
                                 "--uid <uid>` (one-shot) or `plh_hub --skeleton` + edit "
                                 "hub.json + `plh_hub --keygen` (manual) — see "
                                 "HEP-CORE-0033 §6.5.  Required format: HEP-CORE-0033 "
                                 "§G2.2.0b PeerUid grammar 'hub.<name>.uid<8hex>', "
                                 "e.g. 'hub.main.uid3a7f2b1c'.");
    }
    if (!pylabhub::hub::is_valid_identifier(hc.uid, pylabhub::hub::IdentifierKind::PeerUid))
    {
        throw std::runtime_error("hub: invalid 'hub.uid' = '" + hc.uid +
                                 "'.  Must follow HEP-CORE-0033 §G2.2.0b PeerUid grammar "
                                 "'hub.<name>.uid<8hex>', e.g. 'hub.main.uid3a7f2b1c'.");
    }

    return hc;
}

} // namespace pylabhub::config
