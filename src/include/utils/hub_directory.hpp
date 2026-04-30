#pragma once
/**
 * @file hub_directory.hpp
 * @brief HubDirectory — canonical hub directory layout (HEP-CORE-0033 §7).
 *
 * A hub directory is the canonical on-disk layout for a pyLabHub hub
 * instance:
 *
 *   <base>/
 *     hub.json                ← composite hub config (HEP-CORE-0033 §6.1)
 *     script/<type>/          ← OPTIONAL — absent = hub runs without scripting
 *     schemas/                ← OPTIONAL — hub-global schema records
 *                               (HEP-CORE-0034 §12)
 *     vault/                  ← CURVE keypair + (optional) admin token
 *                               (0700 on POSIX)
 *     logs/                   ← log files
 *     run/                    ← runtime state (PID files, etc.)
 *
 * HubDirectory is a thin, value-like wrapper around a base path.  It
 * does NOT manage the lifetime of the directory or its contents; it
 * provides strongly-typed, centralised path resolution.
 *
 * No registration builder — the hub is single-kind (HEP-0033 §2.2),
 * so `init_directory` writes a fixed `hub.json` template directly.
 */

#include "pylabhub_utils_export.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::utils
{

/**
 * @brief Canonical hub directory layout with path resolution helpers.
 *
 * Thread-safety: HubDirectory is immutable after construction.  All methods
 * are safe to call from any thread.
 */
class PYLABHUB_UTILS_EXPORT HubDirectory
{
public:
    // ── Construction ───────────────────────────────────────────────────────────

    ~HubDirectory();
    HubDirectory(HubDirectory &&) noexcept;
    HubDirectory &operator=(HubDirectory &&) noexcept;
    HubDirectory(const HubDirectory &) = delete;
    HubDirectory &operator=(const HubDirectory &) = delete;

    /**
     * @brief Open an existing hub directory.
     *
     * Does not check whether the directory or any of its subdirectories
     * exist; use @ref has_standard_layout() to verify.
     *
     * @param base  Path to the hub directory root (absolute or relative).
     *              Stored as weakly-canonical.
     */
    static HubDirectory open(const std::filesystem::path &base);

    /**
     * @brief Open the hub directory that owns the given config file.
     *
     * Equivalent to `open(config_path.parent_path())`.
     *
     * @param config_path  Path to a hub.json file.
     */
    static HubDirectory from_config_file(const std::filesystem::path &config_path);

    /**
     * @brief Create a new hub directory with the standard subdirectory layout.
     *
     * Creates the following subdirectories under @p base (creating
     * intermediate directories as needed):
     *   - logs/
     *   - run/
     *   - vault/  (mode 0700 on POSIX — private keypair storage)
     *   - schemas/  (HEP-CORE-0034 §12 — hub-global schema records)
     *   - script/python/  (template language; can be replaced/empty)
     *
     * The function is idempotent: existing subdirectories are kept;
     * vault/ permissions are NOT reset on an existing directory.
     *
     * @throws std::runtime_error if any required subdirectory cannot be created.
     */
    static HubDirectory create(const std::filesystem::path &base);

    // ── Path accessors ─────────────────────────────────────────────────────────

    /** @brief Absolute (weakly-canonical) path to the hub directory root. */
    const std::filesystem::path &base() const noexcept { return base_; }

    /** @brief `<base>/logs` — hub log-file directory. */
    std::filesystem::path logs() const { return base_ / "logs"; }

    /** @brief `<base>/run` — PID file and runtime state directory. */
    std::filesystem::path run() const { return base_ / "run"; }

    /** @brief `<base>/vault` — private keypair storage (0700 on POSIX). */
    std::filesystem::path vault() const { return base_ / "vault"; }

    /**
     * @brief `<base>/schemas` — hub-global schema record directory.
     * HEP-CORE-0034 §12.  May or may not exist; absence is valid
     * (hub then serves only producer-private schema records).
     */
    std::filesystem::path schemas() const { return base_ / "schemas"; }

    /** @brief `<base>/hub.json` — fixed config file path. */
    std::filesystem::path config_file() const { return base_ / "hub.json"; }

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
     * (mirrors HEP-CORE-0024 §3.1 role-side resolution).
     *
     * The @p script_path from the config (`script.path`) is resolved
     * relative to the hub base directory if relative.  This method does
     * NOT verify that the resulting path exists.
     *
     * @param script_path  Value of `script.path` from hub.json.
     *                     Relative paths are resolved relative to @ref base().
     * @param type         Script type: "python" (→ `__init__.py`) or
     *                     "lua" (→ `init.lua`).
     * @return Absolute path to the script entry-point file.
     */
    std::filesystem::path script_entry(std::string_view script_path,
                                       std::string_view type) const;

    // ── Security helpers ───────────────────────────────────────────────────────

    /**
     * @brief Canonical hub vault path: `<base>/vault/hub.vault`.
     *
     * Per HEP-CORE-0033 §6.5, the hub vault has a fixed filename
     * (unlike role vaults, which key on uid).  HubVault::open()
     * reads from this exact path.
     */
    std::filesystem::path hub_vault_file() const
    {
        return vault() / "hub.vault";
    }

    // ── Layout inspection ──────────────────────────────────────────────────────

    /**
     * @brief Return true when the required subdirectories exist under base.
     *
     * Checks for: `logs/`, `run/`, `vault/`.  `script/` and `schemas/`
     * are optional per HEP-0033 §7 and are NOT checked here.
     */
    bool has_standard_layout() const;

    // ── --init scaffolding ─────────────────────────────────────────────────────

    struct LogInitOverrides
    {
        /// Written to @c logging.max_size_mb if set.
        std::optional<double> max_size_mb;
        /// Written to @c logging.backups if set (@c -1 = keep all).
        std::optional<int>    backups;
    };

    /**
     * @brief Scaffolding init for the hub directory.
     *
     * The library performs NO user interaction.  Callers (binary main())
     * must resolve the name before calling — e.g. via
     * pylabhub::cli::resolve_init_name() for CLI/interactive flow.
     *
     * Sequence:
     *   1. Validate preconditions (name non-empty, hub.json doesn't exist).
     *   2. create(dir) — directory structure.
     *   3. Generate hub uid via uid::generate_hub_uid(name).
     *   4. Write hub.json template (HEP-0033 §6.2 minus the auth/access
     *      fields deferred to HEP-0035).
     *   5. Print summary (directory, uid, config path).
     *
     * @param dir  Directory to initialize (pass empty for current path).
     * @param name Hub instance name. Must be non-empty.
     * @param log  Optional CLI overrides for the generated `logging`
     *             section.  Any set field is written into the JSON before
     *             dump; unset fields leave the template default in place.
     * @return 0 on success, non-zero on error.
     */
    static int init_directory(const std::filesystem::path &dir,
                              const std::string &name,
                              const LogInitOverrides &log = {});

private:
    explicit HubDirectory(std::filesystem::path base) noexcept;

    std::filesystem::path base_;
};

} // namespace pylabhub::utils
