#pragma once
/**
 * @file auth_config.hpp
 * @brief AuthConfig — categorical config for CURVE authentication.
 *
 * Parsed from the role-specific "auth" JSON sub-object.
 * Vault operations (keypair loading/creation) are handled by RoleConfig,
 * not AuthConfig — see RoleConfig::load_keypair() and create_keypair().
 */

#include "utils/json_fwd.hpp"

#include <string>
#include <string_view>

namespace pylabhub::config
{

struct AuthConfig
{
    std::string keyfile;       ///< Path to encrypted vault; empty = no CURVE auth.
    std::string client_pubkey; ///< Z85 CURVE25519 public key (40 chars); populated by RoleConfig::load_keypair().
    std::string client_seckey; ///< Z85 CURVE25519 secret key; populated by RoleConfig::load_keypair().
};

/// Parse auth from the role-specific JSON section.
/// @param j         Root JSON object.
/// @param role_tag  Role type: "producer", "consumer", "processor".
inline AuthConfig parse_auth_config(const nlohmann::json &j, std::string_view role_tag)
{
    AuthConfig ac;
    const auto sect_name = std::string(role_tag);
    if (j.contains(sect_name) && j[sect_name].is_object())
    {
        const auto &sect = j[sect_name];
        if (sect.contains("auth") && sect["auth"].is_object())
            ac.keyfile = sect["auth"].value("keyfile", "");
    }
    return ac;
}

} // namespace pylabhub::config
