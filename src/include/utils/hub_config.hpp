#pragma once

/**
 * @file hub_config.hpp
 * @brief HubConfig — central hub configuration singleton lifecycle module.
 *
 * Reads `hub.json` from the hub instance directory (set via set_config_path() before
 * lifecycle startup), resolves all paths to absolute, and exposes typed getters.
 *
 * ## Lifecycle
 *
 * Register via `LifecycleGuard`:
 * @code
 *   HubConfig::set_config_path(hub_dir / "hub.json");  // call before lifecycle
 *   LifecycleGuard lifecycle(MakeModDefList(
 *       Logger::GetLifecycleModule(),
 *       pylabhub::crypto::GetLifecycleModule(),
 *       HubConfig::GetLifecycleModule(),
 *       ...));
 * @endcode
 *
 * Startup order: `Logger → CryptoUtils → HubConfig → ...`
 *
 * ## Config loading (priority low → high)
 *
 *  1. Built-in C++ defaults (hard-coded in Impl; always applied first)
 *  2. `hub.json` from hub instance directory — loaded when set_config_path() is called
 *  3. `PYLABHUB_CONFIG_FILE` env var — explicit single-file override (CI / scripted)
 *  4. `PYLABHUB_HUB_NAME` / `PYLABHUB_BROKER_ENDPOINT` / `PYLABHUB_ADMIN_ENDPOINT`
 *     — highest-priority env var overrides applied after file loading
 *
 * ## Environment overrides (applied after file load)
 *
 *  - `PYLABHUB_HUB_NAME`             — overrides hub.name
 *  - `PYLABHUB_BROKER_ENDPOINT`      — overrides hub.broker_endpoint
 *  - `PYLABHUB_ADMIN_ENDPOINT`       — overrides hub.admin_endpoint
 */

#include "utils/lifecycle.hpp"
#include "utils/channel_access_policy.hpp"
#include "utils/json_config.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace pylabhub
{

/**
 * @brief Configuration for a single hub federation peer (HEP-CORE-0022).
 *
 * Parsed from one entry in hub.json["peers"].  The broker uses this to
 * create an outbound DEALER connection and relay broadcasts.
 */
struct HubPeerConfig
{
    std::string                   hub_uid;          ///< Peer hub UID (e.g. "HUB-DEMOB-00000002")
    std::string                   broker_endpoint;  ///< Peer broker ROUTER endpoint
    std::string                   pubkey_z85;       ///< Z85 CURVE25519 public key (40 chars); empty = no auth
    std::vector<std::string>      channels;         ///< Channels this hub relays FROM itself TO the peer
};

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
     * @brief Supply the admin token from the vault before LifecycleGuard starts.
     *
     * The admin token is a secret and must NEVER be stored in hub.json (which is
     * world-readable at 0644).  The vault (hub.vault, 0600) is the sole source.
     * Call this after opening the vault, before the lifecycle starts.
     * hub.json must not contain an "admin.token" field; any such field is ignored
     * with an error log at runtime.
     */
    static void set_admin_token(const std::string& token);

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
     * @pre Must be called after the lifecycle module is started (Initializing or
     *      Initialized state).  Asserts in debug builds if called too early or
     *      after shutdown.
     */
    static HubConfig& get_instance();

    /// True once the lifecycle startup callback has completed (config loaded).
    [[nodiscard]] static bool lifecycle_initialized() noexcept;

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
     * Set via hub.json["hub"]["uid"].
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

    /** Grace period for graceful channel shutdown (CHANNEL_CLOSING_NOTIFY → FORCE_SHUTDOWN). */
    std::chrono::seconds channel_shutdown_grace() const noexcept;

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

    /**
     * @brief Hub script package directory (type-specific subdirectory of the script path).
     *
     * Resolved from `hub.json["script"]["path"]` + `hub.json["script"]["type"]`.
     * For example, `path = "./script"` + `type = "python"` → `<hub_dir>/script/python/`.
     * `HubScript` loads `<hub_script_dir>/__init__.py` as a Python package.
     *
     * Empty when not configured or no hub directory is in use (dev/test mode).
     *
     * Backward compat: old hubs with `"python": {"script": "./script"}` get
     * `hub_script_dir = <hub_dir>/script/` (flat, no type subdir appended).
     */
    const std::filesystem::path& hub_script_dir() const noexcept;

    /**
     * @brief The scripting language type configured for this hub.
     *
     * Set via `hub.json["script"]["type"]`. Empty when not configured.
     * Required when `hub.json["script"]["path"]` is also set; omitting it
     * leaves `hub_script_dir()` empty so the hub runs without a user script.
     * Valid values: `"python"`, `"lua"`.
     */
    const std::string& script_type() const noexcept;

    /** Path to requirements.txt for Python environment setup. */
    const std::filesystem::path& python_requirements() const noexcept;

    /**
     * @brief Tick interval for the hub's periodic tick thread (milliseconds).
     *
     * Set via `hub.json["python"]["tick_interval_ms"]`. Default: 1000 ms.
     */
    int tick_interval_ms() const noexcept;

    /**
     * @brief How often the C++ tick runner logs a channel health summary (milliseconds).
     *
     * Set via `hub.json["python"]["health_log_interval_ms"]`. Default: 60000 ms.
     * The actual log fires when `tick_count % (health_log_interval_ms / tick_interval_ms) == 0`.
     */
    int health_log_interval_ms() const noexcept;

    // -----------------------------------------------------------------------
    // Security settings
    // -----------------------------------------------------------------------

    /**
     * @brief Pre-shared token for the admin shell.
     *
     * Set exclusively via set_admin_token() (called by hubshell after vault.open()).
     * Never stored in hub.json. Empty = no auth (dev / ephemeral mode only).
     */
    const std::string& admin_token() const noexcept;

    // -----------------------------------------------------------------------
    // Connection policy (Phase 3)
    // -----------------------------------------------------------------------

    /** Hub-wide connection policy for channel registration. Default: Open. */
    broker::ConnectionPolicy connection_policy() const noexcept;

    /** Known actors allowlist. Non-empty only when policy is Verified (or pre-populated). */
    std::vector<broker::KnownActor> known_actors() const;

    /** Per-channel policy overrides (first matching glob wins over hub-wide policy). */
    std::vector<broker::ChannelPolicy> channel_policies() const;

    // -----------------------------------------------------------------------
    // Hub federation peers (HEP-CORE-0022)
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the list of configured federation peers.
     *
     * Each entry is parsed from `hub.json["peers"][]` and describes one outbound
     * DEALER connection the broker should establish on startup.
     */
    const std::vector<HubPeerConfig>& peers() const noexcept;

    // -----------------------------------------------------------------------
    // Directory model (Phase 5)
    // -----------------------------------------------------------------------

    /**
     * @brief Hub directory path.
     *
     * The hub instance directory (parent of hub.json). Set via set_config_path().
     * Empty when running without a hub directory (dev/test mode using built-in defaults).
     */
    const std::filesystem::path& hub_dir() const noexcept;

    /**
     * @brief Path to the hub's broker public key file (`<hub_dir>/hub.pubkey`).
     *
     * Written at startup by HubVault::publish_public_key(). Empty if hub_dir is not set.
     */
    std::filesystem::path hub_pubkey_path() const noexcept;

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
