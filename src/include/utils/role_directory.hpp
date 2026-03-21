#pragma once
/**
 * @file role_directory.hpp
 * @brief RoleDirectory — formalised role directory layout and path resolution (HEP-CORE-0024).
 *
 * A role directory is the canonical on-disk layout for a pyLabHub role instance:
 *
 *   <base>/
 *     <role>.json          ← JSON config file
 *     script/<type>/       ← script entry point (e.g. script/python/__init__.py)
 *     logs/                ← log files
 *     run/                 ← runtime state (PID files, etc.)
 *     vault/               ← NaCl keypairs (0700 on POSIX)
 *
 * RoleDirectory provides:
 *   - Path accessors (base, logs, run, vault, config_file, script_entry)
 *   - Hub reference resolution (resolve_hub_dir, hub_broker_endpoint, hub_pubkey_path)
 *   - Directory creation (create — creates subdirectories with correct permissions)
 *   - Layout validation (has_standard_layout)
 *   - Default keyfile path (inside vault/, named <uid>.vault)
 *
 * See: docs/HEP/HEP-CORE-0024-Role-Directory-Service.md
 */

#include "pylabhub_utils_export.h"
#include "utils/json_fwd.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Forward declarations for categorical configs (avoid heavy includes).
namespace pylabhub::config
{
struct IdentityConfig;
struct AuthConfig;
struct ScriptConfig;
struct TimingConfig;
struct InboxConfig;
struct ValidationConfig;
struct StartupConfig;
struct MonitoringConfig;
} // namespace pylabhub::config

namespace pylabhub::utils
{

/**
 * @brief Canonical role directory layout with path resolution helpers.
 *
 * RoleDirectory is a thin, value-like wrapper around a base path.  It does NOT
 * manage the lifetime of the directory or its contents; it merely provides
 * strongly-typed, centralised path resolution.
 *
 * Thread-safety: RoleDirectory is immutable after construction.  All methods are
 * safe to call from any thread.
 */
class PYLABHUB_UTILS_EXPORT RoleDirectory
{
public:
    // ── Construction ───────────────────────────────────────────────────────────

    /**
     * @brief Open an existing role directory.
     *
     * Does not check whether the directory or any of its subdirectories exist;
     * use @ref has_standard_layout() to verify.
     *
     * @param base  Path to the role directory root (absolute or relative).
     *              Stored as weakly-canonical.
     */
    ~RoleDirectory();
    RoleDirectory(RoleDirectory &&) noexcept;
    RoleDirectory &operator=(RoleDirectory &&) noexcept;
    RoleDirectory(const RoleDirectory &) = delete;
    RoleDirectory &operator=(const RoleDirectory &) = delete;

    static RoleDirectory open(const std::filesystem::path &base);

    /**
     * @brief Open the role directory that owns the given config file.
     *
     * Equivalent to `open(config_path.parent_path())`.
     *
     * @param config_path  Path to the role JSON config file.
     */
    static RoleDirectory from_config_file(const std::filesystem::path &config_path);

    /**
     * @brief Create a new role directory with the standard subdirectory layout.
     *
     * Creates the following subdirectories under @p base (creating intermediate
     * directories as needed):
     *   - logs/
     *   - run/
     *   - vault/  (mode 0700 on POSIX — private keypair storage)
     *   - script/python/
     *
     * The function is idempotent: if subdirectories already exist it succeeds
     * silently (does not reset vault/ permissions on an existing directory).
     *
     * @param base  Directory to create / populate.
     * @return RoleDirectory pointing to @p base (weakly-canonical).
     * @throws std::runtime_error if any required subdirectory cannot be created.
     */
    static RoleDirectory create(const std::filesystem::path &base);

    // ── Path accessors ─────────────────────────────────────────────────────────

    /** @brief Absolute (weakly-canonical) path to the role directory root. */
    const std::filesystem::path &base() const noexcept { return base_; }

    /** @brief `<base>/logs` — role log-file directory. */
    std::filesystem::path logs() const { return base_ / "logs"; }

    /** @brief `<base>/run` — PID file and runtime state directory. */
    std::filesystem::path run() const { return base_ / "run"; }

    /** @brief `<base>/vault` — private keypair storage (0700 on POSIX). */
    std::filesystem::path vault() const { return base_ / "vault"; }

    /**
     * @brief `<base>/<filename>` — path to the role JSON config file.
     * @param filename  Config file name, e.g. "producer.json".
     */
    std::filesystem::path config_file(std::string_view filename) const
    {
        return base_ / std::filesystem::path(filename);
    }

    /**
     * @brief `<base>/<name>` — arbitrary named subdirectory under base.
     * @param name  Subdirectory name.
     */
    std::filesystem::path subdir(std::string_view name) const
    {
        return base_ / std::filesystem::path(name);
    }

    // ── Script entry point ─────────────────────────────────────────────────────

    /**
     * @brief Resolve the script entry-point file.
     *
     * Convention: `<resolved_script_path>/script/<type>/__init__.<ext>`
     *
     * The @p script_path from the config (`script.path`) is first resolved
     * relative to the role base directory (if relative).  This method does NOT
     * verify that the resulting path exists.
     *
     * @param script_path  Value of `script.path` from the role JSON config.
     *                     Relative paths are resolved relative to @ref base().
     * @param type         Script type: "python" (→ `__init__.py`) or
     *                     "lua" (→ `init.lua`).
     * @return Absolute path to the script entry-point file.
     */
    std::filesystem::path script_entry(std::string_view script_path,
                                       std::string_view type) const;

    // ── Security helpers ───────────────────────────────────────────────────────

    /**
     * @brief Default keyfile path inside vault/ for the given UID.
     *
     * Returns `<base>/vault/<uid>.vault`.  The `vault/` directory is created
     * with 0700 permissions by @ref create().
     *
     * @param uid  Role UID, e.g. "PROD-TEMPSENS-12345678".
     */
    std::filesystem::path default_keyfile(std::string_view uid) const
    {
        return vault() / (std::string(uid) + ".vault");
    }

    // ── Hub reference resolution ───────────────────────────────────────────────

    /**
     * @brief Resolve a `hub_dir` value from the role config.
     *
     * - Empty string → nullopt (no hub_dir configured).
     * - Absolute path → returned as weakly_canonical.
     * - Relative path → resolved relative to @ref base(), then weakly_canonical.
     *
     * @param hub_dir_value  Value of the `hub_dir` JSON field.
     * @return Absolute (weakly-canonical) path to the hub directory, or nullopt.
     */
    std::optional<std::filesystem::path> resolve_hub_dir(
        std::string_view hub_dir_value) const;

    /**
     * @brief Path to the hub's public key file.
     * @param hub_dir  Absolute path to the hub directory.
     * @return `<hub_dir>/hub.pubkey`
     */
    static std::filesystem::path hub_pubkey_path(const std::filesystem::path &hub_dir)
    {
        return hub_dir / "hub.pubkey";
    }

    /**
     * @brief Read the broker endpoint from a hub directory.
     *
     * Opens `<hub_dir>/hub.json` and extracts `hub.broker_endpoint`.
     *
     * @param hub_dir  Absolute path to the hub directory.
     * @return Broker endpoint string, e.g. "tcp://127.0.0.1:5570".
     * @throws std::runtime_error if hub.json cannot be opened or the field
     *         is missing.
     */
    static std::string hub_broker_endpoint(const std::filesystem::path &hub_dir);

    /**
     * @brief Read the broker public key from a hub directory (if present).
     *
     * Returns the first line of `<hub_dir>/hub.pubkey`, or empty string if the
     * file does not exist.
     *
     * @param hub_dir  Absolute path to the hub directory.
     */
    static std::string hub_broker_pubkey(const std::filesystem::path &hub_dir);

    // ── Security diagnostics ───────────────────────────────────────────────────

    /**
     * @brief Emit a security warning if the keyfile is inside the role directory.
     *
     * An encrypted keypair stored inside the role directory can be exfiltrated
     * by a user script (which has full filesystem access as the role process
     * user).  An attacker who obtains the `.vault` file can attempt offline
     * brute-force against the Argon2id password.
     *
     * The warning is printed unconditionally to stderr (pre-lifecycle, so the
     * Logger may not yet be initialised).
     *
     * No warning is emitted when @p keyfile is empty.
     *
     * @param role_base  Absolute (weakly-canonical) role directory base path.
     * @param keyfile    Value of `auth.keyfile` from the role JSON config
     *                   (absolute or relative; relative paths are resolved
     *                   relative to @p role_base for the comparison).
     */
    static void warn_if_keyfile_in_role_dir(const std::filesystem::path &role_base,
                                             const std::string           &keyfile);

    // ── Layout inspection ──────────────────────────────────────────────────────

    /**
     * @brief Return true when all standard subdirectories exist under base.
     *
     * Checks for: `logs/`, `run/`, `vault/`, `script/python/`.
     */
    bool has_standard_layout() const;

    // ── Config loading ────────────────────────────────────────────────────────

    /**
     * @brief Load and parse a role config file via categorical parsers.
     *
     * Reads the JSON file, runs all shared parsers (identity, auth, script,
     * timing, inbox, startup, monitoring, validation), integrates hub
     * resolution and path normalization. Also reads role-specific fields
     * (channel, transport, SHM, schemas) from the JSON.
     *
     * After this call, typed accessors (identity(), script(), etc.) are valid.
     *
     * @param filename   Config file name, e.g. "producer.json".
     * @param role_tag   Role type: "producer", "consumer", "processor".
     * @throws std::runtime_error on file-not-found, parse error, or validation failure.
     */
    void load_config(std::string_view filename, std::string_view role_tag);

    /** @brief True after load_config() succeeds. */
    bool config_loaded() const noexcept;

    // ── Typed config accessors (valid after load_config) ──────────────────────

    const config::IdentityConfig   &identity()   const;
    const config::AuthConfig       &auth()       const;
    const config::ScriptConfig     &script()     const;
    const config::TimingConfig     &timing()     const;
    const config::InboxConfig      &inbox()      const;
    const config::ValidationConfig &validation() const;
    const config::StartupConfig    &startup()    const;
    const config::MonitoringConfig &monitoring() const;

    /** @brief The raw parsed JSON. Valid after load_config(). */
    const nlohmann::json &raw_json() const;

    /** @brief The role tag passed to load_config(). */
    const std::string &role_tag() const;

private:
    explicit RoleDirectory(std::filesystem::path base) noexcept;

    std::filesystem::path base_;

    struct ConfigState;
    std::unique_ptr<ConfigState> config_;
};

} // namespace pylabhub::utils
