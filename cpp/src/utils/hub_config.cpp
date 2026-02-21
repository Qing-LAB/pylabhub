/**
 * @file hub_config.cpp
 * @brief HubConfig singleton lifecycle module implementation.
 *
 * Config loading strategy (priority low → high):
 *  1. Built-in C++ defaults (hard-coded in Impl struct fields)
 *  2. hub.default.json  — the canonical defaults file; always updated by the build
 *  3. hub.user.json     — user customisations; merged on top of defaults
 *  4. PYLABHUB_CONFIG_FILE env var — if set, replaces both file sources with a
 *     single explicit file (useful for CI / scripted environments)
 *  5. PYLABHUB_HUB_NAME / PYLABHUB_BROKER_ENDPOINT / PYLABHUB_ADMIN_ENDPOINT
 *     — process-level overrides applied after file loading
 */
#include "plh_service.hpp"
#include "utils/hub_config.hpp"
#include "utils/json_config.hpp"
#include "uid_utils.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#if defined(PYLABHUB_IS_POSIX)
#include <unistd.h>
#include <climits>
#endif

#if defined(PYLABHUB_IS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pylabhub
{

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static std::atomic<bool> g_hub_config_initialized{false};

static std::mutex g_config_path_mu;
static fs::path   g_config_path_override; ///< Set by set_config_path() before startup.

// ---------------------------------------------------------------------------
// Helpers (anonymous namespace)
// ---------------------------------------------------------------------------

namespace
{

/// Returns the absolute path to the running executable's directory.
fs::path get_binary_dir() noexcept
{
    try
    {
#if defined(PYLABHUB_IS_LINUX)
        char buf[PATH_MAX];
        ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0)
        {
            buf[len] = '\0';
            return fs::path(buf).parent_path();
        }
#elif defined(PYLABHUB_IS_APPLE)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string path(size, '\0');
        if (_NSGetExecutablePath(path.data(), &size) == 0)
            return fs::canonical(fs::path(path)).parent_path();
#elif defined(PYLABHUB_IS_WINDOWS)
        wchar_t buf[32767];
        DWORD len = GetModuleFileNameW(nullptr, buf, 32767);
        if (len > 0)
            return fs::path(buf).parent_path();
#endif
    }
    catch (...) {}
    return {};
}

/// Returns the config directory discovered from the binary location.
fs::path discover_config_dir() noexcept
{
    try
    {
        fs::path bin = get_binary_dir();
        if (bin.empty())
            return {};

        // Standard staged layout: <root>/bin/ + <root>/config/
        fs::path candidate = bin / ".." / "config";
        if (fs::is_directory(candidate))
            return fs::weakly_canonical(candidate);

        // Flat layout: config/ next to binary
        candidate = bin / "config";
        if (fs::is_directory(candidate))
            return fs::weakly_canonical(candidate);
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

/// Recursively merges `overrides` into `base` (object keys override, arrays replace).
void json_merge(nlohmann::json& base, const nlohmann::json& overrides) noexcept
{
    if (!overrides.is_object())
        return;
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
    {
        if (it.value().is_object() &&
            base.contains(it.key()) && base.at(it.key()).is_object())
        {
            json_merge(base[it.key()], it.value());
        }
        else
        {
            base[it.key()] = it.value();
        }
    }
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
    std::string admin_token    {}; ///< Empty = no auth required.

    std::chrono::seconds channel_timeout         {10};
    std::chrono::seconds consumer_liveness_check {5};

    fs::path root_dir;
    fs::path config_dir;
    fs::path scripts_python_dir;
    fs::path scripts_lua_dir;
    fs::path data_dir;
    fs::path python_startup_script;
    fs::path python_requirements;

    utils::JsonConfig cfg; ///< Holds the merged JSON for raw access.

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
        }
        if (j.contains("admin"))
        {
            const auto& a = j.at("admin");
            if (a.contains("token") && a.at("token").is_string())
                admin_token = a.at("token").get<std::string>();
        }
        if (j.contains("broker"))
        {
            const auto& b = j.at("broker");
            if (b.contains("channel_timeout_s"))
                channel_timeout = std::chrono::seconds(b.at("channel_timeout_s").get<int>());
            if (b.contains("consumer_liveness_check_s"))
                consumer_liveness_check = std::chrono::seconds(b.at("consumer_liveness_check_s").get<int>());
        }
        if (j.contains("paths") && !config_dir.empty())
        {
            const auto& p = j.at("paths");
            auto r = [&](const char* key) -> std::string {
                return p.contains(key) ? p.at(key).get<std::string>() : std::string{};
            };
            auto sv = r("scripts_python");
            auto sl = r("scripts_lua");
            auto sd = r("data_dir");
            if (!sv.empty()) scripts_python_dir = resolve_path(config_dir, sv);
            if (!sl.empty()) scripts_lua_dir    = resolve_path(config_dir, sl);
            if (!sd.empty()) data_dir           = resolve_path(config_dir, sd);
        }
        if (j.contains("python") && !config_dir.empty())
        {
            const auto& py = j.at("python");
            if (py.contains("startup_script") && !py.at("startup_script").is_null())
                python_startup_script = resolve_path(config_dir,
                    py.at("startup_script").get<std::string>());
            if (py.contains("requirements") && !py.at("requirements").is_null())
                python_requirements = resolve_path(config_dir,
                    py.at("requirements").get<std::string>());
        }
    }

    // ------------------------------------------------------------------
    // Main startup: layered config load.
    // ------------------------------------------------------------------
    void load(const fs::path& override_path)
    {
        // --- Determine config directory ---
        if (!override_path.empty())
        {
            // Direct file override (PYLABHUB_CONFIG_FILE or set_config_path()).
            config_dir = override_path.parent_path();
            root_dir   = fs::weakly_canonical(config_dir / "..");

            nlohmann::json j = read_json_file(override_path);
            if (!j.empty())
            {
                LOGGER_INFO("HubConfig: loading override file '{}'", override_path.string());
                apply_json(j);
                std::error_code ec;
                cfg.init(override_path, /*createIfMissing=*/false, &ec);
            }
            else
            {
                LOGGER_WARN("HubConfig: override file '{}' not readable — using defaults",
                            override_path.string());
            }
        }
        else if (const char* env = std::getenv("PYLABHUB_CONFIG_FILE"))
        {
            // Env-var explicit file override.
            fs::path ep(env);
            config_dir = ep.parent_path();
            root_dir   = fs::weakly_canonical(config_dir / "..");

            nlohmann::json j = read_json_file(ep);
            if (!j.empty())
            {
                LOGGER_INFO("HubConfig: loading PYLABHUB_CONFIG_FILE '{}'", ep.string());
                apply_json(j);
                std::error_code ec;
                cfg.init(ep, /*createIfMissing=*/false, &ec);
            }
            else
            {
                LOGGER_WARN("HubConfig: PYLABHUB_CONFIG_FILE '{}' not readable — using defaults",
                            ep.string());
            }
        }
        else
        {
            // Standard layered load: hub.default.json → merge hub.user.json.
            fs::path cfg_dir = discover_config_dir();
            if (!cfg_dir.empty())
            {
                config_dir = cfg_dir;
                root_dir   = fs::weakly_canonical(cfg_dir / "..");

                nlohmann::json merged = nlohmann::json::object();

                // Layer 1: hub.default.json (canonical defaults; always present after staging)
                fs::path def_file = cfg_dir / "hub.default.json";
                if (fs::exists(def_file))
                {
                    nlohmann::json jdef = read_json_file(def_file);
                    if (!jdef.empty())
                    {
                        LOGGER_INFO("HubConfig: loading defaults from '{}'", def_file.string());
                        json_merge(merged, jdef);
                    }
                }
                else
                {
                    LOGGER_INFO("HubConfig: hub.default.json not found — using built-in defaults");
                }

                // Layer 2: hub.user.json (user overrides; optional)
                fs::path user_file = cfg_dir / "hub.user.json";
                if (fs::exists(user_file))
                {
                    nlohmann::json juser = read_json_file(user_file);
                    if (!juser.empty())
                    {
                        LOGGER_INFO("HubConfig: merging user overrides from '{}'", user_file.string());
                        json_merge(merged, juser);
                    }
                }
                else
                {
                    LOGGER_INFO("HubConfig: no hub.user.json found — using defaults only");
                }

                // Apply the merged JSON to member variables.
                if (!merged.empty())
                    apply_json(merged);

                // Store the merged result in the JsonConfig for raw access.
                fs::path merged_path = cfg_dir / "hub.default.json"; // use as backing file
                std::error_code ec;
                cfg.init(merged_path, /*createIfMissing=*/false, &ec);
                // Update the in-memory view with the fully merged JSON.
                cfg.transaction(utils::JsonConfig::AccessFlags::Default).write(
                    [&merged](nlohmann::json& j) { j = merged; });
            }
            else
            {
                // Fallback: derive root from binary dir, use built-in defaults.
                fs::path bin = get_binary_dir();
                if (!bin.empty())
                {
                    root_dir   = fs::weakly_canonical(bin / "..");
                    config_dir = root_dir / "config";
                }
                LOGGER_INFO("HubConfig: no config directory found — using built-in defaults");
            }
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
HubConfig& HubConfig::get_instance()
{
    static HubConfig instance;
    return instance;
}

// Called by the lifecycle startup function.
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

const fs::path& HubConfig::root_dir()              const noexcept { return pImpl->root_dir; }
const fs::path& HubConfig::config_dir()            const noexcept { return pImpl->config_dir; }
const fs::path& HubConfig::scripts_python_dir()    const noexcept { return pImpl->scripts_python_dir; }
const fs::path& HubConfig::scripts_lua_dir()       const noexcept { return pImpl->scripts_lua_dir; }
const fs::path& HubConfig::data_dir()              const noexcept { return pImpl->data_dir; }
const fs::path& HubConfig::python_startup_script() const noexcept { return pImpl->python_startup_script; }
const fs::path& HubConfig::python_requirements()   const noexcept { return pImpl->python_requirements; }

const utils::JsonConfig& HubConfig::json_config() const noexcept { return pImpl->cfg; }

// ---------------------------------------------------------------------------
// Lifecycle startup / shutdown
// ---------------------------------------------------------------------------

namespace
{
void do_hub_config_startup(const char* /*arg*/)
{
    fs::path override_path;
    {
        std::lock_guard lock(g_config_path_mu);
        override_path = g_config_path_override;
    }
    HubConfig::get_instance().load_(override_path);
    g_hub_config_initialized.store(true, std::memory_order_release);
}

void do_hub_config_shutdown(const char* /*arg*/)
{
    g_hub_config_initialized.store(false, std::memory_order_release);
}
} // namespace

// static
utils::ModuleDef HubConfig::GetLifecycleModule()
{
    utils::ModuleDef module("pylabhub::HubConfig");
    module.add_dependency("pylabhub::utils::Logger");
    module.add_dependency("pylabhub::utils::JsonConfig");
    module.set_startup(&do_hub_config_startup);
    module.set_shutdown(&do_hub_config_shutdown, std::chrono::milliseconds(500));
    return module;
}

} // namespace pylabhub
