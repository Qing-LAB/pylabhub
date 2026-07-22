#pragma once
/**
 * @file auth_config.hpp
 * @brief AuthConfig — categorical config for CURVE authentication.
 *
 * Parsed from the role-specific (or "hub") `auth` JSON sub-object.
 * Vault operations (keypair loading/creation) are handled by RoleConfig
 * / HubConfig, not AuthConfig — see RoleConfig::load_keypair() and
 * create_keypair() (HEP-CORE-0024 §3.4; HEP-CORE-0033 §7.1).
 */

#include "utils/json_fwd.hpp"
#include "utils/security/secure_subsystem.hpp" // secure() surface

#include <stdexcept>
#include <string>
#include <string_view>

namespace pylabhub::config
{

struct AuthConfig
{
    /// Vault path (REQUIRED non-empty per HEP-CORE-0024 §3.4).
    /// After HEP-CORE-0040 §171: this is the ONLY field — keypair bytes
    /// live in `pylabhub::utils::security::secure().keys()` (LockedKey storage,
    /// mlock'd + zero-on-destruct), populated by `load_keypair(password)`
    /// via `secure().keys().add_identity_from_z85("hub_identity" |
    /// "role_identity", pub_z85, sec_z85)`.
    /// Sites that previously read `auth().client_pubkey` /
    /// `auth().client_seckey` now call `secure().keys().pubkey(name)` and
    /// `secure().keys().with_seckey(name, cb)` at the libzmq socket-option
    /// site (HEP-CORE-0040 §8.2 — use-not-export).
    std::string keyfile;
};

/// Parse auth from the role-specific (or "hub") JSON section.
///
/// **Required by HEP-CORE-0024 §3.4 + HEP-CORE-0033 §7.1 + HEP-CORE-0035
/// §4.6.2:** the `<section>.auth` object MUST be present and MUST contain
/// a `keyfile` field that is a non-empty string.  The path identifies
/// the on-disk encrypted vault file holding the role/hub CURVE keypair
/// (and, on the hub, the admin token).
///
/// All three errors below are config-load failures with operator-actionable
/// diagnostics:
///   - missing `auth` object
///   - missing `auth.keyfile` field
///   - empty `auth.keyfile` string
///
/// The empty-string case is rejected because pylabhub is a vault: the
/// security contract is "secret material on disk at a specified place."
/// A silent "empty means generate-in-memory" fallback would be
/// indistinguishable from a misconfigured deployment and would degrade
/// the security posture without operator awareness.  Operators who want
/// secret material elsewhere choose a different path; operators with a
/// genuine no-vault use case (an L4 test against a tmpdir) write a path
/// pointing into that tmpdir.
///
/// @param j         Root JSON object.
/// @param role_type  Section name: "producer" / "consumer" / "processor"
///                  for role configs; "hub" for hub configs.
inline AuthConfig parse_auth_config(const nlohmann::json &j, std::string_view role_type)
{
    AuthConfig ac;
    const auto sect_name = std::string(role_type);
    if (!j.contains(sect_name) || !j[sect_name].is_object())
        return ac; // outer-section absence is handled by the section's own parser.

    const auto &sect = j[sect_name];
    if (!sect.contains("auth"))
        throw std::runtime_error(sect_name + ": missing required '" + sect_name +
                                 ".auth' object. "
                                 "Add `\"auth\": { \"keyfile\": \"<path-to-vault>\" }` — the vault "
                                 "is required (see HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1).");
    if (!sect["auth"].is_object())
        throw std::runtime_error(sect_name + ": '" + sect_name + ".auth' must be a JSON object.");

    const auto &a = sect["auth"];
    for (auto it = a.begin(); it != a.end(); ++it)
    {
        if (it.key() != "keyfile")
            throw std::runtime_error(sect_name + ": unknown config key '" + sect_name + ".auth." +
                                     it.key() + "'");
    }
    if (!a.contains("keyfile"))
        throw std::runtime_error(sect_name + ": missing required '" + sect_name +
                                 ".auth.keyfile' "
                                 "field. Set it to the path of the on-disk vault file "
                                 "(see HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1).");
    if (!a["keyfile"].is_string())
        throw std::runtime_error(sect_name + ": '" + sect_name +
                                 ".auth.keyfile' must be a string.");
    ac.keyfile = a["keyfile"].get<std::string>();
    if (ac.keyfile.empty())
        throw std::runtime_error(sect_name + ": '" + sect_name +
                                 ".auth.keyfile' must be a "
                                 "non-empty path string. pylabhub does not support a no-vault "
                                 "operation mode — secret material is required (see HEP-CORE-0024 "
                                 "§3.4 / HEP-CORE-0033 §7.1).");
    return ac;
}

} // namespace pylabhub::config
