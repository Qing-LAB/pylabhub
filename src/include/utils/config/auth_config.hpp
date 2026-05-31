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
    std::string keyfile;       ///< Vault path (vault mode) or "" (explicit ephemeral).
    std::string client_pubkey; ///< Z85 CURVE25519 public key (40 chars); populated by RoleConfig::load_keypair().
    std::string client_seckey; ///< Z85 CURVE25519 secret key; populated by RoleConfig::load_keypair().
};

/// Parse auth from the role-specific (or "hub") JSON section.
///
/// **Required by HEP-CORE-0024 §3.4 + HEP-CORE-0033 §7.1 + HEP-CORE-0035
/// §4.6.2:** the `<section>.auth` object MUST be present and MUST contain
/// a `keyfile` field.  The operator chooses one of two explicit modes:
///   - `keyfile = "<path>"`  — vault mode (file referenced is the
///                             encrypted CURVE key vault).
///   - `keyfile = ""`        — explicit ephemeral mode (no on-disk
///                             vault; CURVE keys generated in memory).
///
/// Missing `auth` object OR missing `keyfile` field is a config-load
/// error — pylabhub refuses to silently fall through to either mode
/// because the two have different security trade-offs and the choice
/// must be a deliberate operator decision.
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
            "Add `\"auth\": { \"keyfile\": \"<path-to-vault>\" }` for vault mode, "
            "or `\"auth\": { \"keyfile\": \"\" }` for explicit ephemeral mode "
            "(see HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1).");
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
            "field. Set it to a vault path for vault mode, or to \"\" "
            "(empty string) to opt explicitly into ephemeral mode "
            "(see HEP-CORE-0024 §3.4 / HEP-CORE-0033 §7.1).");
    if (!a["keyfile"].is_string())
        throw std::runtime_error(
            sect_name + ": '" + sect_name + ".auth.keyfile' must be a string.");
    ac.keyfile = a["keyfile"].get<std::string>();
    return ac;
}

} // namespace pylabhub::config
