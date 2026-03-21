#pragma once
/**
 * @file auth_config.hpp
 * @brief AuthConfig — categorical config for CURVE authentication.
 *
 * Parsed from the role-specific "auth" JSON sub-object.
 * load_keypair() decrypts the vault file at runtime.
 */

#include "utils/json_fwd.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace pylabhub::config
{

struct AuthConfig
{
    std::string keyfile;       ///< Path to encrypted vault; empty = no CURVE auth.
    std::string client_pubkey; ///< Z85 CURVE25519 public key (40 chars); resolved at runtime.
    std::string client_seckey; ///< Z85 CURVE25519 secret key; resolved at runtime.

    /**
     * @brief Decrypt the vault at keyfile and populate client_pubkey / client_seckey.
     * No-op if keyfile is empty.
     * @param uid       Role UID — used as Argon2id KDF domain separator.
     * @param password  Vault password.
     * @param role_tag  Short role tag for log messages ("prod", "cons", "proc").
     * @return true if keys were loaded; false if keyfile absent.
     * @throws std::runtime_error if vault exists but decryption fails.
     */
    bool load_keypair(const std::string &uid, const std::string &password,
                      const char *role_tag);
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
