#pragma once

/**
 * @file hub_config.hpp
 * @brief HubConfig — central hub configuration singleton lifecycle module.
 *
 * Reads `config/hub.json` (relative to the binary's location), resolves all
 * paths to absolute, and exposes typed getters to every module in the process.
 *
 * ## Lifecycle
 *
 * Register via `LifecycleGuard`:
 * @code
 *   LifecycleGuard lifecycle(MakeModDefList(
 *       Logger::GetLifecycleModule(),
 *       pylabhub::crypto::GetLifecycleModule(),
 *       HubConfig::GetLifecycleModule(),
 *       ...));
 * @endcode
 *
 * Startup order: `Logger → CryptoUtils → HubConfig → ...`
 *
 * ## Config loading — layered (priority low → high)
 *
 *  1. Built-in C++ defaults (always applied first)
 *  2. `config/hub.default.json` — canonical defaults staged by the build system;
 *     always updated on rebuild, **never** edited by users
 *  3. `config/hub.user.json` — user customisations merged on top of defaults;
 *     deployed once from a template and **never overwritten** by the build
 *  4. `PYLABHUB_CONFIG_FILE` env var — explicit single-file override; bypasses
 *     the default/user layering (useful for CI or scripted deployments)
 *  5. `PYLABHUB_HUB_NAME` / `PYLABHUB_BROKER_ENDPOINT` / `PYLABHUB_ADMIN_ENDPOINT`
 *     — highest-priority env var overrides applied after file loading
 *
 * The config directory is discovered (in order):
 *  - `<binary_dir>/../config/` (standard staged layout: bin/ + config/)
 *  - `<binary_dir>/config/`   (flat layout)
 *
 * If no config directory is found, HubConfig starts with built-in defaults.
 *
 * ## Environment overrides (applied after file load)
 *
 *  - `PYLABHUB_HUB_NAME`             — overrides hub.name
 *  - `PYLABHUB_BROKER_ENDPOINT`      — overrides hub.broker_endpoint
 *  - `PYLABHUB_ADMIN_ENDPOINT`       — overrides hub.admin_endpoint
 */

#include "plh_service.hpp"
#include "utils/json_config.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace pylabhub
{

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/**
 * @class HubConfig
 * @brief Singleton lifecycle module that owns the hub's JSON configuration.
 *
 * Thread-safe after startup; all getters are const and lock-free (values are
 * resolved once at startup and stored as value types).
 */
class PYLABHUB_UTILS_EXPORT HubConfig
{
  public:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Optional: call before LifecycleGuard to override the config path.
     *
     * If not called, path discovery runs automatically at startup.
     * Must be called before the lifecycle module starts.
     */
    static void set_config_path(const std::filesystem::path& path);

    /**
     * @brief Returns the ModuleDef for use with LifecycleGuard.
     * Dependencies: Logger, JsonConfig.
     */
    static utils::ModuleDef GetLifecycleModule();

    // -----------------------------------------------------------------------
    // Singleton accessor
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the global HubConfig instance.
     * @pre Must be called after the lifecycle module is started.
     */
    static HubConfig& get_instance();

    // -----------------------------------------------------------------------
    // Hub identity
    // -----------------------------------------------------------------------

    /** Hub name in reverse-domain format (e.g. "asu.lab.experiments.main"). */
    const std::string& hub_name() const noexcept;

    /** Human-readable description of this hub. */
    const std::string& hub_description() const noexcept;

    /**
     * @brief Stable unique identifier for this hub instance.
     *
     * Format: @c "HUB-{NAME}-{8HEX}" (e.g. "HUB-MYLABHUB-3A7F2B1C").
     * Auto-generated from hub_name at first startup if not set in config.
     * Can be overridden in hub.user.json["hub"]["uid"].
     */
    const std::string& hub_uid() const noexcept;

    // -----------------------------------------------------------------------
    // Network endpoints
    // -----------------------------------------------------------------------

    /** ZMQ endpoint for the BrokerService (e.g. "tcp://0.0.0.0:5570"). */
    const std::string& broker_endpoint() const noexcept;

    /** ZMQ endpoint for the admin shell (local only, e.g. "tcp://127.0.0.1:5600"). */
    const std::string& admin_endpoint() const noexcept;

    // -----------------------------------------------------------------------
    // Broker timing
    // -----------------------------------------------------------------------

    /** Channel heartbeat timeout — broker closes channel after this. */
    std::chrono::seconds channel_timeout() const noexcept;

    /** How often the broker checks consumer liveness (0 = disabled). */
    std::chrono::seconds consumer_liveness_check() const noexcept;

    // -----------------------------------------------------------------------
    // File-system paths (all absolute after startup)
    // -----------------------------------------------------------------------

    /** Root directory of the hub installation (e.g. the staged directory). */
    const std::filesystem::path& root_dir() const noexcept;

    /** Directory containing hub.json and key files. */
    const std::filesystem::path& config_dir() const noexcept;

    /** Directory for Python user scripts. */
    const std::filesystem::path& scripts_python_dir() const noexcept;

    /** Directory for Lua user scripts. */
    const std::filesystem::path& scripts_lua_dir() const noexcept;

    /** Default data output directory. */
    const std::filesystem::path& data_dir() const noexcept;

    /** Optional Python startup script (empty if not configured). */
    const std::filesystem::path& python_startup_script() const noexcept;

    /** Path to requirements.txt for Python environment setup. */
    const std::filesystem::path& python_requirements() const noexcept;

    // -----------------------------------------------------------------------
    // Security settings
    // -----------------------------------------------------------------------

    /**
     * @brief Optional pre-shared token for the admin shell.
     *
     * Read from `hub.user.json["admin"]["token"]`. Empty string means no auth
     * (connections from localhost are accepted without a token).
     */
    const std::string& admin_token() const noexcept;

    // -----------------------------------------------------------------------
    // Raw config access (for modules that need custom keys)
    // -----------------------------------------------------------------------

    /**
     * @brief Provides read-only access to the underlying JsonConfig.
     * @pre Lifecycle module must be started.
     */
    const utils::JsonConfig& json_config() const noexcept;

    // -----------------------------------------------------------------------
    // Non-copyable, non-movable singleton
    // -----------------------------------------------------------------------
    HubConfig(const HubConfig&) = delete;
    HubConfig& operator=(const HubConfig&) = delete;
    HubConfig(HubConfig&&) = delete;
    HubConfig& operator=(HubConfig&&) = delete;

    /// @internal Called by the lifecycle startup function (hub_config.cpp).
    /// Do NOT call directly from application code.
    void load_(const std::filesystem::path& override_path);

  private:
    HubConfig();
    ~HubConfig();

    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace pylabhub
