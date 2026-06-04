#pragma once
/**
 * @file hub_config.hpp
 * @brief HubConfig — hub-side composite config (HEP-CORE-0033 §6.1).
 *
 * Mirrors `RoleConfig` shape: pImpl, `JsonConfig` backend (thread-safe,
 * process-safe file I/O + `reload_if_changed()`), strict-key whitelist
 * parsing.  Owns the identity / auth / script / logging / network /
 * admin / broker / federation / state sub-configs that together form
 * a single `hub.json`.
 *
 * Intentionally *not* directional — the hub has no `in_`/`out_` sides.
 *
 * The role-side reference (`<direction>_hub_dir` from a role config →
 * resolved broker endpoint + pubkey) lives in `hub_ref_config.hpp`
 * and uses the unrelated `HubRefConfig` symbol.
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include "utils/config/auth_config.hpp"
#include "utils/config/hub_admin_config.hpp"
#include "utils/config/hub_broker_config.hpp"
#include "utils/config/hub_federation_config.hpp"
#include "utils/config/hub_identity_config.hpp"
#include "utils/config/hub_network_config.hpp"
#include "utils/config/hub_state_config.hpp"
#include "utils/config/logging_config.hpp"
#include "utils/config/script_config.hpp"
#include "utils/config/timing_config.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace pylabhub::config
{

class PYLABHUB_UTILS_EXPORT HubConfig
{
  public:
    // ── Factory methods ──────────────────────────────────────────────

    /// Load from an explicit hub.json path.
    /// @throws std::runtime_error on file/parse/validation error.
    static HubConfig load(const std::string &path);

    /// Load from a hub directory (reads `<dir>/hub.json`).
    /// @throws std::runtime_error on file/parse/validation error.
    static HubConfig load_from_directory(const std::string &dir);

    // ── Accessors ────────────────────────────────────────────────────

    const HubIdentityConfig   &identity()   const;
    const AuthConfig          &auth()       const;
    const ScriptConfig        &script()     const;
    const TimingConfig        &timing()     const;
    const LoggingConfig       &logging()    const;
    const HubNetworkConfig    &network()    const;
    const HubAdminConfig      &admin()      const;
    const HubBrokerConfig     &broker()     const;
    const HubFederationConfig &federation() const;
    const HubStateConfig      &state()      const;

    // ── Vault operations ────────────────────────────────────────────

    /// Decrypt the vault file and load keypair into auth config.
    /// Uses identity().uid as the KDF domain separator.
    /// @return true if keys were loaded; false if no keyfile configured.
    /// @throws std::runtime_error if vault exists but decryption fails.
    bool load_keypair(const std::string &password);

    /// Create a new vault file with a generated keypair.
    /// @return The public key string.
    /// @throws std::runtime_error if vault creation fails.
    std::string create_keypair(const std::string &password);

    // ── Raw JSON / JsonConfig operations ────────────────────────────

    const nlohmann::json         &raw() const;
    bool                          reload_if_changed();
    const std::filesystem::path  &base_dir() const;

    // ── Special members (pImpl) ──────────────────────────────────────

    ~HubConfig();
    HubConfig(HubConfig &&) noexcept;
    HubConfig &operator=(HubConfig &&) noexcept;

  private:
    HubConfig(); // private — use factory methods

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::config
