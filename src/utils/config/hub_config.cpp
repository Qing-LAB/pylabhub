/**
 * @file hub_config.cpp
 * @brief HubConfig singleton lifecycle module implementation.
 *
 * Config loading strategy (priority low → high):
 *  1. Built-in C++ defaults (hard-coded in Impl struct fields)
 *  2. hub.json from hub instance directory — loaded when set_config_path() is called
 *     (i.e. when pylabhub-hubshell is launched with a hub_dir argument)
 *  3. PYLABHUB_CONFIG_FILE env var — explicit single-file override (useful for CI)
 *  4. PYLABHUB_HUB_NAME / PYLABHUB_BROKER_ENDPOINT / PYLABHUB_ADMIN_ENDPOINT
 *     — process-level overrides applied after file loading
 */
#include "plh_platform.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/channel_access_policy.hpp"
#include "utils/hub_config.hpp"
#include "utils/json_config.hpp"
#include "uid_utils.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace pylabhub
{

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

/// Lifecycle state for HubConfig — mirrors the Logger's state-machine pattern.
///
///   Uninitialized ──► Initializing ──► Initialized ──► ShuttingDown
///
/// get_instance() is allowed in Initializing (the startup callback loads the
/// config via get_instance().load_()) and Initialized (normal runtime).
/// Calls during Uninitialized or ShuttingDown trigger an assertion failure,
/// catching modules that forget to declare a dependency on pylabhub::HubConfig.
enum class HubConfigState : int { Uninitialized, Initializing, Initialized, ShuttingDown };
static std::atomic<HubConfigState> g_hub_config_state{HubConfigState::Uninitialized};

static std::mutex g_config_path_mu;
static fs::path   g_config_path_override; ///< Set by set_config_path() before startup.

static std::mutex  g_admin_token_mu;
static std::string g_admin_token; ///< Set by set_admin_token() before startup; vault is sole source.

// ---------------------------------------------------------------------------
// Helpers (anonymous namespace)
// ---------------------------------------------------------------------------

namespace
{

/// Returns the absolute path to the running executable's directory.
/// Delegates to platform::get_executable_name() which correctly handles
/// Linux (/proc/self/exe), macOS (_NSGetExecutablePath), and Windows
/// (GetModuleFileNameW).
fs::path get_binary_dir() noexcept
{
    try
    {
        const std::string exe = platform::get_executable_name(/*include_path=*/true);
        if (exe != "unknown" && exe != "unknown_win")
            return fs::path(exe).parent_path();
    }
    catch (...) {}
    return {};
}

/// Resolves a relative path in the config against the config directory.
fs::path resolve_path(const fs::path& config_dir, const std::string& raw) noexcept
{
    if (raw.empty())
        return {};
    try
    {
        fs::path p(raw);
        if (p.is_absolute())
            return fs::weakly_canonical(p);
        return fs::weakly_canonical(config_dir / p);
    }
    catch (...) { return fs::path(raw); }
}

/// Reads a JSON file from disk into an nlohmann::json object.
/// Returns a null JSON value on failure.
nlohmann::json read_json_file(const fs::path& path) noexcept
{
    try
    {
        std::ifstream f(path);
        if (!f.is_open())
            return nlohmann::json{};
        nlohmann::json j;
        f >> j;
        return j;
    }
    catch (...) {}
    return nlohmann::json{};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HubConfig::Impl
// ---------------------------------------------------------------------------

struct HubConfig::Impl
{
    // Resolved values — populated once at startup, read-only afterwards.
    std::string hub_name       {"local.hub.default"};
    std::string hub_description{"pyLabHub instance"};
    std::string hub_uid        {};  ///< Auto-generated if not in config: "HUB-{NAME}-{8HEX}"
    std::string broker_endpoint{"tcp://0.0.0.0:5570"};
    std::string admin_endpoint {"tcp://127.0.0.1:5600"};
    std::string admin_token    {}; ///< From vault only; vault is sole source (never from hub.json).

    std::chrono::seconds channel_timeout         {10};
    std::chrono::seconds consumer_liveness_check {5};
    std::chrono::seconds channel_shutdown_grace  {5};

    fs::path root_dir;
    fs::path config_dir;
    fs::path scripts_python_dir;
    fs::path scripts_lua_dir;
    fs::path data_dir;
    fs::path    hub_script_dir;     ///< Resolved from hub.json["script"]["path"] + type subdir.
    std::string script_type_;       ///< hub.json["script"]["type"]. Empty = not configured.
    fs::path python_requirements;
    int tick_interval_ms_    {1000};   ///< hub.json["script"]["tick_interval_ms"]
    int health_log_interval_ms_{60000}; ///< hub.json["script"]["health_log_interval_ms"]

    // ── Connection policy (Phase 3) ──────────────────────────────────────────
    broker::ConnectionPolicy             connection_policy{broker::ConnectionPolicy::Open};
    std::vector<broker::KnownRole>      known_roles;
    std::vector<broker::ChannelPolicy>   channel_policies;

    // ── Federation peers (HEP-CORE-0022) ────────────────────────────────────
    std::vector<HubPeerConfig>           peers_;

    utils::JsonConfig cfg; ///< Holds the merged JSON for raw access.

    // ------------------------------------------------------------------
    // Reset all fields to built-in defaults (for lifecycle re-init).
    // ------------------------------------------------------------------
    void reset_to_defaults()
    {
        hub_name           = "local.hub.default";
        hub_description    = "pyLabHub instance";
        hub_uid            = {};
        broker_endpoint    = "tcp://0.0.0.0:5570";
        admin_endpoint     = "tcp://127.0.0.1:5600";
        admin_token        = {};
        channel_timeout         = std::chrono::seconds{10};
        consumer_liveness_check = std::chrono::seconds{5};
        channel_shutdown_grace  = std::chrono::seconds{5};
        root_dir.clear();
        config_dir.clear();
        scripts_python_dir.clear();
        scripts_lua_dir.clear();
        data_dir.clear();
        hub_script_dir.clear();
        script_type_.clear();
        python_requirements.clear();
        tick_interval_ms_       = 1000;
        health_log_interval_ms_ = 60000;
        connection_policy  = broker::ConnectionPolicy::Open;
        known_roles.clear();
        channel_policies.clear();
        peers_.clear();
        cfg = utils::JsonConfig{};
    }

    // ------------------------------------------------------------------
    // Parse a JSON object and extract all known fields into member vars.
    // ------------------------------------------------------------------
    void apply_json(const nlohmann::json& j)
    {
        if (j.contains("hub"))
        {
            const auto& h = j.at("hub");
            if (h.contains("name"))             hub_name        = h.at("name").get<std::string>();
            if (h.contains("description"))      hub_description = h.at("description").get<std::string>();
            if (h.contains("uid"))              hub_uid         = h.at("uid").get<std::string>();
            if (h.contains("broker_endpoint"))  broker_endpoint = h.at("broker_endpoint").get<std::string>();
            if (h.contains("admin_endpoint"))   admin_endpoint  = h.at("admin_endpoint").get<std::string>();

            // ── Connection policy (Phase 3) ──────────────────────────────────
            if (h.contains("connection_policy") && h.at("connection_policy").is_string())
            {
                const std::string policy_str = h.at("connection_policy").get<std::string>();
                connection_policy = broker::connection_policy_from_str(policy_str);
                if (connection_policy == broker::ConnectionPolicy::Open &&
                    policy_str != "open" && !policy_str.empty())
                {
                    LOGGER_WARN(
                        "HubConfig: unknown connection_policy '{}' — defaulting to 'open'. "
                        "Valid values: open, tracked, required, verified.",
                        policy_str);
                }
            }
            if (h.contains("known_roles") && h.at("known_roles").is_array())
            {
                known_roles.clear();
                for (const auto& a : h.at("known_roles"))
                {
                    broker::KnownRole ka;
                    ka.name = a.value("name", "");
                    ka.uid  = a.value("uid", "");
                    ka.role = a.value("role", "any");
                    if (!ka.name.empty() && !ka.uid.empty())
                    {
                        known_roles.push_back(std::move(ka));
                    }
                }
            }
            if (h.contains("channel_policies") && h.at("channel_policies").is_array())
            {
                channel_policies.clear();
                for (const auto& cp : h.at("channel_policies"))
                {
                    const std::string glob = cp.value("channel", "");
                    const std::string pstr = cp.value("connection_policy", "open");
                    if (!glob.empty())
                    {
                        const auto pol = broker::connection_policy_from_str(pstr);
                        if (pol == broker::ConnectionPolicy::Open &&
                            pstr != "open" && !pstr.empty())
                        {
                            LOGGER_WARN(
                                "HubConfig: unknown channel_policy '{}' for glob '{}' — "
                                "defaulting to 'open'.", pstr, glob);
                        }
                        channel_policies.push_back({glob, pol});
                    }
                }
            }
        }
        if (j.contains("admin"))
        {
            const auto& a = j.at("admin");
            if (a.contains("token") && a.at("token").is_string() &&
                !a.at("token").get<std::string>().empty())
            {
                LOGGER_ERROR("HubConfig: 'admin.token' found in hub.json — "
                             "this is a SECURITY VIOLATION. hub.json is world-readable (0644). "
                             "Remove 'admin.token' from hub.json. The admin token is stored "
                             "exclusively in the encrypted vault (hub.vault, 0600) and injected "
                             "at runtime via HubConfig::set_admin_token(). The value is IGNORED.");
            }
        }
        if (j.contains("broker"))
        {
            const auto& b = j.at("broker");
            if (b.contains("channel_timeout_s"))
                channel_timeout = std::chrono::seconds(b.at("channel_timeout_s").get<int>());
            if (b.contains("consumer_liveness_check_s"))
                consumer_liveness_check = std::chrono::seconds(b.at("consumer_liveness_check_s").get<int>());
            if (b.contains("channel_shutdown_grace_s"))
                channel_shutdown_grace = std::chrono::seconds(b.at("channel_shutdown_grace_s").get<int>());
        }
        if (j.contains("paths") && !config_dir.empty())
        {
            const auto& p = j.at("paths");
            auto get_str = [&](const char* key) -> std::string {
                return p.contains(key) ? p.at(key).get<std::string>() : std::string{};
            };
            auto python_path = get_str("scripts_python");
            auto lua_path    = get_str("scripts_lua");
            auto data_path   = get_str("data_dir");
            if (!python_path.empty()) scripts_python_dir = resolve_path(config_dir, python_path);
            if (!lua_path.empty())    scripts_lua_dir    = resolve_path(config_dir, lua_path);
            if (!data_path.empty())   data_dir           = resolve_path(config_dir, data_path);
        }
        // Language-neutral "script" block (current format).
        // Tick timing, script path, and type belong here regardless of scripting language.
        if (j.contains("script") && !config_dir.empty())
        {
            const auto& sc = j.at("script");
            // Parse type (required when path is also set).
            if (sc.contains("type") && sc.at("type").is_string())
                script_type_ = sc.at("type").get<std::string>();
            // Resolve base path and append the type-specific subdirectory.
            // Both "type" AND "path" must be present to form a valid hub_script_dir.
            // e.g. path="./script", type="python" → hub_script_dir = <hub_dir>/script/python/
            //      path="./script", type="lua"    → hub_script_dir = <hub_dir>/script/lua/
            // If either is absent, hub_script_dir stays empty (no script loaded).
            if (sc.contains("path") && !sc.at("path").is_null() && !script_type_.empty())
            {
                const fs::path base = resolve_path(config_dir,
                    sc.at("path").get<std::string>());
                hub_script_dir = base / script_type_;
            }
            if (sc.contains("tick_interval_ms") && sc.at("tick_interval_ms").is_number_integer())
                tick_interval_ms_ = sc.at("tick_interval_ms").get<int>();
            if (sc.contains("health_log_interval_ms") &&
                sc.at("health_log_interval_ms").is_number_integer())
                health_log_interval_ms_ = sc.at("health_log_interval_ms").get<int>();
        }

        // ── Federation peers (HEP-CORE-0022) ────────────────────────────────
        if (j.contains("peers") && j.at("peers").is_array())
        {
            peers_.clear();
            for (const auto& p : j.at("peers"))
            {
                HubPeerConfig pc;
                pc.hub_uid         = p.value("hub_uid",         "");
                pc.broker_endpoint = p.value("broker_endpoint", "");
                pc.pubkey_z85      = p.value("pubkey_z85",      "");
                if (p.contains("channels") && p.at("channels").is_array())
                {
                    for (const auto& ch : p.at("channels"))
                        if (ch.is_string())
                            pc.channels.push_back(ch.get<std::string>());
                }
                if (!pc.broker_endpoint.empty())
                    peers_.push_back(std::move(pc));
                else
                    LOGGER_WARN("HubConfig: peers[] entry missing 'broker_endpoint' — skipped");
            }
        }

        // Python-specific settings ("python" block).
        // Backward compat: also accept old "python"."script" + tick keys from pre-unified hubs.
        if (j.contains("python") && !config_dir.empty())
        {
            const auto& py = j.at("python");
            // python-specific: requirements file path.
            if (py.contains("requirements") && !py.at("requirements").is_null())
                python_requirements = resolve_path(config_dir,
                    py.at("requirements").get<std::string>());
            // backward compat: old hubs stored script path + tick settings under "python".
            if (hub_script_dir.empty() && py.contains("script") && !py.at("script").is_null())
                hub_script_dir = resolve_path(config_dir,
                    py.at("script").get<std::string>());
            if (tick_interval_ms_ == 1000 &&
                py.contains("tick_interval_ms") && py.at("tick_interval_ms").is_number_integer())
                tick_interval_ms_ = py.at("tick_interval_ms").get<int>();
            if (health_log_interval_ms_ == 60000 &&
                py.contains("health_log_interval_ms") &&
                py.at("health_log_interval_ms").is_number_integer())
                health_log_interval_ms_ = py.at("health_log_interval_ms").get<int>();
        }
    }

    // ------------------------------------------------------------------
    // Main startup: load hub.json from hub instance directory.
    // ------------------------------------------------------------------
    void load(const fs::path& override_path)
    {
        // Reset all fields so lifecycle re-init starts fresh (critical for tests
        // that create multiple LifecycleGuard instances in the same process).
        reset_to_defaults();

        // Determine which config file to load (if any).
        fs::path target;
        if (!override_path.empty())
        {
            // Hub instance directory mode: set_config_path(hub_dir / "hub.json").
            target = override_path;
        }
        else if (const char* env = std::getenv("PYLABHUB_CONFIG_FILE"))
        {
            // CI / scripted deployment: explicit single-file override.
            target = fs::path(env);
        }

        if (!target.empty())
        {
            config_dir = target.parent_path();
            root_dir   = fs::weakly_canonical(config_dir / "..");

            nlohmann::json j = read_json_file(target);
            if (!j.empty())
            {
                LOGGER_INFO("HubConfig: loading '{}'", target.string());
                apply_json(j);
                std::error_code ec;
                cfg.init(target, /*createIfMissing=*/false, &ec);
            }
            else
            {
                LOGGER_WARN("HubConfig: '{}' not readable — using built-in defaults",
                            target.string());
            }
        }
        else
        {
            // No config file: use built-in defaults.
            // Derive root_dir from the binary location for path resolution.
            fs::path bin = get_binary_dir();
            if (!bin.empty())
            {
                root_dir   = fs::weakly_canonical(bin / "..");
                config_dir = root_dir;
            }
            LOGGER_INFO("HubConfig: no hub.json specified — using built-in defaults");
        }

        // --- Fill path defaults for anything not set by the JSON ---
        if (scripts_python_dir.empty() && !root_dir.empty())
            scripts_python_dir = root_dir / "share" / "scripts" / "python";
        if (scripts_lua_dir.empty() && !root_dir.empty())
            scripts_lua_dir = root_dir / "share" / "scripts" / "lua";
        if (data_dir.empty() && !root_dir.empty())
            data_dir = root_dir / "data";
        if (python_requirements.empty() && !scripts_python_dir.empty())
            python_requirements = scripts_python_dir / "requirements.txt";

        // --- Process-level env var overrides (highest priority) ---
        if (const char* env = std::getenv("PYLABHUB_HUB_NAME"))
            hub_name = env;
        if (const char* env = std::getenv("PYLABHUB_BROKER_ENDPOINT"))
            broker_endpoint = env;
        if (const char* env = std::getenv("PYLABHUB_ADMIN_ENDPOINT"))
            admin_endpoint = env;

        // --- Admin token: from vault only (set before lifecycle via set_admin_token()) ---
        {
            std::lock_guard lock(g_admin_token_mu);
            admin_token = g_admin_token;
        }


        // --- UID: auto-generate if not provided in config ---
        if (hub_uid.empty())
        {
            hub_uid = pylabhub::uid::generate_hub_uid(hub_name);
        }
        else if (!pylabhub::uid::has_hub_prefix(hub_uid))
        {
            LOGGER_WARN("HubConfig: hub.uid '{}' does not start with 'HUB-'; "
                        "recommend using generate_hub_uid() format.", hub_uid);
        }

        LOGGER_INFO("HubConfig: hub_name          = {}", hub_name);
        LOGGER_INFO("HubConfig: hub_uid           = {}", hub_uid);
        LOGGER_INFO("HubConfig: broker_endpoint   = {}", broker_endpoint);
        LOGGER_INFO("HubConfig: admin_endpoint    = {}", admin_endpoint);
        LOGGER_INFO("HubConfig: root_dir          = {}", root_dir.string());
        LOGGER_INFO("HubConfig: config_dir        = {}", config_dir.string());
        LOGGER_INFO("HubConfig: scripts_python    = {}", scripts_python_dir.string());
        LOGGER_INFO("HubConfig: data_dir          = {}", data_dir.string());
    }
};

// ---------------------------------------------------------------------------
// HubConfig public interface
// ---------------------------------------------------------------------------

HubConfig::HubConfig() : pImpl(std::make_unique<Impl>()) {}
HubConfig::~HubConfig() = default;

// static
void HubConfig::set_config_path(const fs::path& path)
{
    std::lock_guard lock(g_config_path_mu);
    g_config_path_override = path;
}

// static
void HubConfig::set_admin_token(const std::string& token)
{
    std::lock_guard lock(g_admin_token_mu);
    g_admin_token = token;
}

// static
bool HubConfig::lifecycle_initialized() noexcept
{
    return g_hub_config_state.load(std::memory_order_acquire) == HubConfigState::Initialized;
}

// static
HubConfig& HubConfig::get_instance()
{
    const auto st = g_hub_config_state.load(std::memory_order_acquire);
    assert((st == HubConfigState::Initializing || st == HubConfigState::Initialized) &&
           "HubConfig::get_instance() called outside valid lifecycle state — "
           "ensure your module declares a dependency on pylabhub::HubConfig");
    static HubConfig instance;
    return instance;
}

// Called once during lifecycle init, before any worker threads are started.
// After init completes the config is read-only; no locking is needed on accessors.
void HubConfig::load_(const fs::path& override_path)
{
    pImpl->load(override_path);
}

const std::string& HubConfig::hub_name()        const noexcept { return pImpl->hub_name; }
const std::string& HubConfig::hub_description() const noexcept { return pImpl->hub_description; }
const std::string& HubConfig::hub_uid()         const noexcept { return pImpl->hub_uid; }
const std::string& HubConfig::broker_endpoint() const noexcept { return pImpl->broker_endpoint; }
const std::string& HubConfig::admin_endpoint()  const noexcept { return pImpl->admin_endpoint; }
const std::string& HubConfig::admin_token()     const noexcept { return pImpl->admin_token; }

std::chrono::seconds HubConfig::channel_timeout()         const noexcept { return pImpl->channel_timeout; }
std::chrono::seconds HubConfig::consumer_liveness_check() const noexcept { return pImpl->consumer_liveness_check; }
std::chrono::seconds HubConfig::channel_shutdown_grace()  const noexcept { return pImpl->channel_shutdown_grace; }

const fs::path& HubConfig::root_dir()           const noexcept { return pImpl->root_dir; }
const fs::path& HubConfig::config_dir()         const noexcept { return pImpl->config_dir; }
const fs::path& HubConfig::scripts_python_dir() const noexcept { return pImpl->scripts_python_dir; }
const fs::path& HubConfig::scripts_lua_dir()    const noexcept { return pImpl->scripts_lua_dir; }
const fs::path& HubConfig::data_dir()           const noexcept { return pImpl->data_dir; }
const fs::path& HubConfig::hub_script_dir()     const noexcept { return pImpl->hub_script_dir; }
const std::string& HubConfig::script_type()     const noexcept { return pImpl->script_type_; }
const fs::path& HubConfig::python_requirements() const noexcept { return pImpl->python_requirements; }
int HubConfig::tick_interval_ms()     const noexcept { return pImpl->tick_interval_ms_; }
int HubConfig::health_log_interval_ms() const noexcept { return pImpl->health_log_interval_ms_; }

const utils::JsonConfig& HubConfig::json_config() const noexcept { return pImpl->cfg; }

// ── Connection policy (Phase 3) ─────────────────────────────────────────────
broker::ConnectionPolicy HubConfig::connection_policy() const noexcept
{
    return pImpl->connection_policy;
}
std::vector<broker::KnownRole> HubConfig::known_roles() const
{
    return pImpl->known_roles;
}
std::vector<broker::ChannelPolicy> HubConfig::channel_policies() const
{
    return pImpl->channel_policies;
}

// ── Federation peers (HEP-CORE-0022) ────────────────────────────────────────
const std::vector<HubPeerConfig>& HubConfig::peers() const noexcept { return pImpl->peers_; }

// ── Directory model (Phase 5) ───────────────────────────────────────────────
const fs::path& HubConfig::hub_dir() const noexcept { return pImpl->config_dir; }

std::filesystem::path HubConfig::hub_pubkey_path() const noexcept
{
    if (pImpl->config_dir.empty())
    {
        return {};
    }
    return pImpl->config_dir / "hub.pubkey";
}

// ---------------------------------------------------------------------------
// Lifecycle startup / shutdown
// ---------------------------------------------------------------------------

namespace
{
void do_hub_config_startup(const char* /*arg*/)
{
    g_hub_config_state.store(HubConfigState::Initializing, std::memory_order_release);
    fs::path override_path;
    {
        std::lock_guard lock(g_config_path_mu);
        override_path = g_config_path_override;
    }
    HubConfig::get_instance().load_(override_path);
    g_hub_config_state.store(HubConfigState::Initialized, std::memory_order_release);
}

void do_hub_config_shutdown(const char* /*arg*/)
{
    g_hub_config_state.store(HubConfigState::ShuttingDown, std::memory_order_release);
    std::lock_guard lock(g_admin_token_mu);
    g_admin_token.clear();
}
} // namespace

// static
utils::ModuleDef HubConfig::GetLifecycleModule()
{
    utils::ModuleDef module("pylabhub::HubConfig");
    module.add_dependency("pylabhub::utils::Logger");
    module.add_dependency("pylabhub::utils::JsonConfig");
    module.set_startup(&do_hub_config_startup);
    module.set_shutdown(&do_hub_config_shutdown,
                        std::chrono::milliseconds(pylabhub::kShortTimeoutMs));
    return module;
}

} // namespace pylabhub
