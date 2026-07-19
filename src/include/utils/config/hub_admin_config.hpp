#pragma once
/**
 * @file hub_admin_config.hpp
 * @brief HubAdminConfig — AdminService endpoint configuration.
 *
 * Parsed from the top-level `"admin"` JSON sub-object (HEP-CORE-0033 §6.2 / §11).
 * Controls whether the structured admin RPC endpoint is enabled and where it
 * binds.  The admin plane is CURVE-secured (§11.1) and always token-gated
 * (§11.3) — there is no token-less path.  Loopback is the default bind
 * (defense-in-depth); a network bind is a safe operator opt-in because the
 * transport is encrypted.
 */

#include "utils/json_fwd.hpp"

#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct HubAdminConfig
{
    bool        enabled{true};                                  ///< AdminService on/off
    std::string endpoint{"tcp://127.0.0.1:5600"};               ///< ZMQ endpoint (CURVE-server, §11.1)

    /// Runtime-only: 64-char hex admin token.  Populated by
    /// `HubConfig::load_keypair()` from the unlocked `HubVault`; NOT
    /// parsed from JSON (the token is a vault secret, never on disk
    /// in plaintext).  Empty until the vault is unlocked.  Mirrors
    /// the `AuthConfig::client_pubkey/seckey` runtime-only pattern.
    std::string admin_token;
};

inline HubAdminConfig parse_hub_admin_config(const nlohmann::json &j)
{
    HubAdminConfig ac;
    if (!j.contains("admin"))
        return ac;
    if (!j["admin"].is_object())
        throw std::runtime_error("hub: 'admin' must be an object");

    const auto &sect = j["admin"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "enabled" && k != "endpoint")
            throw std::runtime_error("hub: unknown config key 'admin." + k + "'");
    }

    ac.enabled        = sect.value("enabled",        ac.enabled);
    ac.endpoint       = sect.value("endpoint",       ac.endpoint);
    return ac;
}

} // namespace pylabhub::config
