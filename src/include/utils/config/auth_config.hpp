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

#include <stdexcept>
#include <string>
#include <string_view>

namespace pylabhub::config
{

struct AuthConfig
{
    std::string keyfile;       ///< Vault path (REQUIRED non-empty per HEP-CORE-0024 §3.4).
    std::string client_pubkey; ///< Z85 CURVE25519 public key (40 chars); populated by RoleConfig::load_keypair().
    std::string client_seckey; ///< Z85 CURVE25519 secret key; populated by RoleConfig::load_keypair().
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
/// @param role_tag  Section name: "producer" / "consumer" / "processor"
///                  for role configs; "hub" for hub configs.
inline AuthConfig parse_auth_config(const nlohmann::json &j, std::string_view role_tag)
{
    AuthConfig ac;
    const auto sect_name = std::string(role_tag);
    if (!j.contains(sect_name) || !j[sect_name].is_object())
        return ac;  // outer-section absence is handled by the section's own parser.

    const auto &sect = j[sect_name];
    if (!sect.contains("auth"))
        throw std::runtime_error(
            sect_name + ": missing required '" + sect_name + ".auth' object. "
            "Add `\"auth\": { \"keyfile\": \"<path-to-vault>\" }` — the vault "
            "is required (see HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1).");
    if (!sect["auth"].is_object())
        throw std::runtime_error(
            sect_name + ": '" + sect_name + ".auth' must be a JSON object.");

    const auto &a = sect["auth"];
    for (auto it = a.begin(); it != a.end(); ++it)
    {
        if (it.key() != "keyfile")
            throw std::runtime_error(
                sect_name + ": unknown config key '" + sect_name +
                ".auth." + it.key() + "'");
    }
    if (!a.contains("keyfile"))
        throw std::runtime_error(
            sect_name + ": missing required '" + sect_name + ".auth.keyfile' "
            "field. Set it to the path of the on-disk vault file "
            "(see HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1).");
    if (!a["keyfile"].is_string())
        throw std::runtime_error(
            sect_name + ": '" + sect_name + ".auth.keyfile' must be a string.");
    ac.keyfile = a["keyfile"].get<std::string>();
    if (ac.keyfile.empty())
        throw std::runtime_error(
            sect_name + ": '" + sect_name + ".auth.keyfile' must be a "
            "non-empty path string. pylabhub does not support a no-vault "
            "operation mode — secret material is required (see HEP-CORE-0024 "
            "§3.4 / HEP-CORE-0033 §7.1).");
    return ac;
}

} // namespace pylabhub::config
