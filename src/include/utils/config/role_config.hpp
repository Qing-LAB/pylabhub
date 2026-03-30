#pragma once
/**
 * @file role_config.hpp
 * @brief RoleConfig — unified config class for all pyLabHub roles.
 *
 * RoleConfig is the single config class used by producer, consumer, and processor.
 * It uses pImpl for ABI stability and JsonConfig as the file backend for thread-safe,
 * process-safe config I/O.
 *
 * ## Architecture
 *
 * - **Common fields** are parsed by categorical parsers and stored in the pImpl.
 *   Accessed via typed const accessors: identity(), timing(), in_hub(), out_transport(), etc.
 *
 * - **Directional fields** (hub, transport, SHM, channel) always have
 *   two slots: in_ and out_. Producer populates out_ only, consumer populates in_ only,
 *   processor populates both. No role-specific branching in the loader.
 *
 * - **Role-specific fields** are stored as type-erased std::any inside the pImpl.
 *   Each role defines a fields struct and a parser callback. Typed access via
 *   role_data<T>() template (header-only, instantiated in the binary — ABI-safe).
 *
 * ## JSON naming convention
 *
 * All directional fields use in_/out_ prefixes consistently:
 *   out_hub_dir, out_transport, out_zmq_endpoint, etc.
 * Non-directional fields are unprefixed:
 *   script.type, target_period_ms, stop_on_script_error, etc.
 *
 * See docs/tech_draft/config_module_design.md for full design.
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include "utils/config/auth_config.hpp"
#include "utils/config/checksum_config.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/config/identity_config.hpp"
#include "utils/config/inbox_config.hpp"
#include "utils/config/monitoring_config.hpp"
#include "utils/config/script_config.hpp"
#include "utils/config/shm_config.hpp"
#include "utils/config/startup_config.hpp"
#include "utils/config/timing_config.hpp"
#include "utils/config/transport_config.hpp"

#include <any>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace pylabhub::config
{

class PYLABHUB_UTILS_EXPORT RoleConfig
{
  public:
    /// Role-specific parser callback.
    /// Receives the raw JSON and a const reference to the partially-loaded
    /// RoleConfig (common fields already populated). Returns role-specific
    /// data as std::any.
    using RoleParser = std::function<std::any(const nlohmann::json &,
                                              const RoleConfig &)>;

    // ── Factory methods ──────────────────────────────────────────────

    /// Load from an explicit config file path.
    /// @param path        Absolute or relative path to the JSON config file.
    /// @param role_tag    Role type: "producer", "consumer", "processor".
    /// @param role_parser Optional callback to parse role-specific fields.
    /// @throws std::runtime_error on file/parse/validation error.
    static RoleConfig load(const std::string &path,
                           const char *role_tag,
                           RoleParser role_parser = nullptr);

    /// Load from a role directory (reads <dir>/<role_tag>.json).
    /// @param dir         Role directory path.
    /// @param role_tag    Role type: "producer", "consumer", "processor".
    /// @param role_parser Optional callback to parse role-specific fields.
    /// @throws std::runtime_error on file/parse/validation error.
    static RoleConfig load_from_directory(const std::string &dir,
                                          const char *role_tag,
                                          RoleParser role_parser = nullptr);

    // ── Non-directional accessors ────────────────────────────────────

    const IdentityConfig   &identity()   const;
    const AuthConfig       &auth()       const;
    const ScriptConfig     &script()     const;
    const TimingConfig     &timing()     const;
    const InboxConfig      &inbox()      const;
    const StartupConfig    &startup()    const;
    const MonitoringConfig &monitoring() const;
    const ChecksumConfig   &checksum()   const;

    // ── Directional accessors (two slots each) ───────────────────────

    const HubConfig                    &in_hub()        const;
    const HubConfig                    &out_hub()       const;
    const TransportConfig              &in_transport()  const;
    const TransportConfig              &out_transport() const;
    const ShmConfig                    &in_shm()        const;
    const ShmConfig                    &out_shm()       const;
    const std::string                  &in_channel()    const;
    const std::string                  &out_channel()   const;

    // ── Vault operations ────────────────────────────────────────────

    /// Decrypt the vault file and load keypair into auth config.
    /// Uses identity().uid as the KDF domain separator.
    /// @param password  Vault password.
    /// @return true if keys were loaded; false if no keyfile configured.
    /// @throws std::runtime_error if vault exists but decryption fails.
    bool load_keypair(const std::string &password);

    /// Create a new vault file with a generated keypair.
    /// Uses identity().uid as the KDF domain separator.
    /// @param password  Vault password (empty = no encryption).
    /// @return The public key string.
    /// @throws std::runtime_error if vault creation fails.
    std::string create_keypair(const std::string &password);

    // ── Raw JSON / JsonConfig operations ──────────────────────────────

    /// Access the raw JSON as loaded from file.
    const nlohmann::json &raw() const;

    /// Re-read the file if it changed on disk. Returns true if updated.
    bool reload_if_changed();

    // ── Role-specific typed access ────────────────────────────────────

    /// Returns true if role-specific data was provided by a RoleParser.
    bool has_role_data() const;

    /// Typed access to role-specific data.
    /// Throws std::bad_any_cast if T does not match the stored type.
    template <typename T>
    const T &role_data() const
    {
        return std::any_cast<const T &>(role_data_any_());
    }

    template <typename T>
    T &mutable_role_data()
    {
        return std::any_cast<T &>(mutable_role_data_any_());
    }

    // ── Metadata ──────────────────────────────────────────────────────

    const std::string              &role_tag()  const;
    const std::filesystem::path    &base_dir()  const;

    // ── Special members (pImpl) ──────────────────────────────────────

    ~RoleConfig();
    RoleConfig(RoleConfig &&) noexcept;
    RoleConfig &operator=(RoleConfig &&) noexcept;

  private:
    RoleConfig(); // private — use factory methods

    // Non-template bridges into pImpl (compiled in .cpp).
    const std::any &role_data_any_() const;
    std::any       &mutable_role_data_any_();

    struct Impl;
    std::unique_ptr<Impl> impl_; // ONLY member — pure pImpl, ABI-safe.
};

} // namespace pylabhub::config
