#pragma once
/**
 * @file hub_admin_config.hpp
 * @brief HubAdminConfig — AdminService endpoint configuration.
 *
 * Parsed from the top-level `"admin"` JSON sub-object (HEP-CORE-0033 §6.2 / §11).
 * Controls whether the structured admin RPC endpoint is enabled, where it
 * binds, and whether it requires a token.  Dev-mode skips token gating but
 * is enforced bind-localhost-only by the AdminService.
 */

#include "utils/json_fwd.hpp"

#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct HubAdminConfig
{
    bool        enabled{true};                                  ///< AdminService on/off
    std::string endpoint{"tcp://127.0.0.1:5600"};               ///< ZMQ endpoint
    bool        dev_mode{false};                                ///< Skip vault + token (localhost-only)
    bool        token_required{true};                           ///< Require admin token in non-dev mode

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
        if (k != "enabled" && k != "endpoint" && k != "dev_mode" &&
            k != "token_required")
            throw std::runtime_error("hub: unknown config key 'admin." + k + "'");
    }

    ac.enabled        = sect.value("enabled",        ac.enabled);
    ac.endpoint       = sect.value("endpoint",       ac.endpoint);
    ac.dev_mode       = sect.value("dev_mode",       ac.dev_mode);
    ac.token_required = sect.value("token_required", ac.token_required);
    return ac;
}

} // namespace pylabhub::config
