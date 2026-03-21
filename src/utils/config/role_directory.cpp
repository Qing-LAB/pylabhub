/**
 * @file role_directory.cpp
 * @brief RoleDirectory — canonical role directory layout (HEP-CORE-0024).
 */
#include "utils/role_directory.hpp"

#include "utils/config/auth_config.hpp"
#include "utils/config/identity_config.hpp"
#include "utils/config/inbox_config.hpp"
#include "utils/config/monitoring_config.hpp"
#include "utils/config/script_config.hpp"
#include "utils/config/startup_config.hpp"
#include "utils/config/timing_config.hpp"
#include "utils/config/validation_config.hpp"
#include "plh_platform.hpp" // PYLABHUB_IS_POSIX

#include <cassert>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>

#if defined(PYLABHUB_IS_POSIX)
#include <sys/stat.h>
#endif

namespace pylabhub::utils
{

namespace cfg = pylabhub::config;

// ── ConfigState (must precede defaulted special members) ──────────────────────

struct RoleDirectory::ConfigState
{
    nlohmann::json raw;
    std::string    role_tag;

    cfg::IdentityConfig   identity;
    cfg::AuthConfig       auth;
    cfg::ScriptConfig     script;
    cfg::TimingConfig     timing;
    cfg::InboxConfig      inbox;
    cfg::ValidationConfig validation;
    cfg::StartupConfig    startup;
    cfg::MonitoringConfig monitoring;
};

RoleDirectory::RoleDirectory(std::filesystem::path base) noexcept
    : base_(std::move(base))
{
}
RoleDirectory::~RoleDirectory() = default;
RoleDirectory::RoleDirectory(RoleDirectory &&) noexcept = default;
RoleDirectory &RoleDirectory::operator=(RoleDirectory &&) noexcept = default;

// ── Factory methods ────────────────────────────────────────────────────────────

RoleDirectory RoleDirectory::open(const std::filesystem::path &base)
{
    return RoleDirectory(std::filesystem::weakly_canonical(base));
}

RoleDirectory RoleDirectory::from_config_file(const std::filesystem::path &config_path)
{
    return open(config_path.parent_path());
}

RoleDirectory RoleDirectory::create(const std::filesystem::path &base)
{
    namespace fs = std::filesystem;

    auto make_dir = [](const fs::path &p)
    {
        std::error_code ec;
        fs::create_directories(p, ec);
        if (ec)
            throw std::runtime_error(
                "RoleDirectory: cannot create directory '" + p.string() +
                "': " + ec.message());
    };

    make_dir(base / "logs");
    make_dir(base / "run");
    make_dir(base / "vault");
    make_dir(base / "script" / "python");

#if defined(PYLABHUB_IS_POSIX)
    // Restrict vault/ to owner-only access for keypair security.
    const fs::path vp = base / "vault";
    if (::chmod(vp.c_str(), 0700) != 0)
    {
        // Non-fatal: vault was created; warn but don't abort.
        std::fprintf(stderr,
                     "[role_dir] Warning: could not set 0700 on vault '%s'\n",
                     vp.c_str());
    }
#endif

    return open(base);
}

// ── Path helpers ───────────────────────────────────────────────────────────────

std::filesystem::path RoleDirectory::script_entry(std::string_view script_path,
                                                   std::string_view type) const
{
    namespace fs = std::filesystem;

    const fs::path sp(script_path);
    const fs::path resolved = sp.is_absolute()
                              ? sp
                              : fs::weakly_canonical(base_ / sp);

    // Python: __init__.py (package convention)
    // Lua:    init.lua   (LuaRocks/LÖVE community convention)
    const char* entry = (type == "lua") ? "init.lua" : "__init__.py";
    return resolved / "script" / type / entry;
}

// ── Hub reference resolution ───────────────────────────────────────────────────

std::optional<std::filesystem::path> RoleDirectory::resolve_hub_dir(
    std::string_view hub_dir_value) const
{
    if (hub_dir_value.empty())
        return std::nullopt;

    namespace fs = std::filesystem;
    const fs::path p(hub_dir_value);
    return p.is_absolute()
           ? fs::weakly_canonical(p)
           : fs::weakly_canonical(base_ / p);
}

std::string RoleDirectory::hub_broker_endpoint(const std::filesystem::path &hub_dir)
{
    const auto hub_json = hub_dir / "hub.json";
    std::ifstream f(hub_json);
    if (!f.is_open())
        throw std::runtime_error(
            "RoleDirectory: cannot open hub.json at '" + hub_json.string() + "'");

    const auto hj = nlohmann::json::parse(f);
    return hj.at("hub").at("broker_endpoint").get<std::string>();
}

std::string RoleDirectory::hub_broker_pubkey(const std::filesystem::path &hub_dir)
{
    const auto pubkey_path = hub_dir / "hub.pubkey";
    if (!std::filesystem::exists(pubkey_path))
        return {};

    std::ifstream pk(pubkey_path);
    std::string key;
    std::getline(pk, key);
    return key;
}

// ── Security diagnostics ──────────────────────────────────────────────────────

void RoleDirectory::warn_if_keyfile_in_role_dir(const std::filesystem::path &role_base,
                                                  const std::string           &keyfile)
{
    if (keyfile.empty())
        return;

    namespace fs = std::filesystem;

    // Resolve keyfile: relative paths are resolved relative to role_base so that
    // the comparison is meaningful regardless of the process CWD.
    const fs::path kf_raw(keyfile);
    const fs::path kf = fs::weakly_canonical(
        kf_raw.is_absolute() ? kf_raw : (role_base / kf_raw));

    const fs::path base = fs::weakly_canonical(role_base);

    // Check whether kf is a sub-path of base by comparing path component-by-component.
    auto [base_end, kf_it] = std::mismatch(base.begin(), base.end(), kf.begin(), kf.end());
    if (base_end != base.end())
        return; // keyfile is NOT inside role_base — no warning

    std::fprintf(stderr,
                 "\n"
                 "  *** PYLABHUB SECURITY WARNING ***\n"
                 "  auth.keyfile '%s'\n"
                 "  is located inside the role directory '%s'.\n"
                 "\n"
                 "  Scripts running in this process have full filesystem access\n"
                 "  as the process owner and can read this file.  A leaked vault\n"
                 "  file enables offline brute-force of the Argon2id password.\n"
                 "\n"
                 "  RECOMMENDED: move the vault file outside the role directory\n"
                 "  and update 'auth.keyfile' to its absolute path, e.g.:\n"
                 "    /etc/pylabhub/vault/<uid>.vault   (system-managed service)\n"
                 "    ~/.pylabhub/vault/<uid>.vault      (single-user deployment)\n"
                 "\n",
                 keyfile.c_str(), role_base.string().c_str());
}

// ── Layout inspection ──────────────────────────────────────────────────────────

bool RoleDirectory::has_standard_layout() const
{
    namespace fs = std::filesystem;
    return fs::is_directory(base_ / "logs") &&
           fs::is_directory(base_ / "run") &&
           fs::is_directory(base_ / "vault") &&
           fs::is_directory(base_ / "script" / "python");
}

// ============================================================================
// load_config and accessors
// ============================================================================

void RoleDirectory::load_config(std::string_view filename, std::string_view role_tag)
{
    namespace fs = std::filesystem;

    const fs::path cfg_path = config_file(filename);
    std::ifstream ifs(cfg_path);
    if (!ifs)
        throw std::runtime_error(
            "RoleDirectory: cannot open config '" + cfg_path.string() + "'");

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(ifs);
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error(
            "RoleDirectory: JSON parse error in '" + cfg_path.string() + "': " + e.what());
    }

    const std::string tag_str(role_tag);
    const char *tag = tag_str.c_str();

    // Determine default period: producer=100ms, consumer/processor=0.
    const double default_period = (role_tag == "producer") ? 100.0 : 0.0;

    auto state = std::make_unique<ConfigState>();
    state->raw      = j;
    state->role_tag = tag_str;

    // ── Shared categorical parsers ────────────────────────────────────────────
    state->identity   = cfg::parse_identity_config(j, role_tag);
    state->auth       = cfg::parse_auth_config(j, role_tag);
    state->script     = cfg::parse_script_config(j, base_, tag);
    state->timing     = cfg::parse_timing_config(j, tag, default_period);
    state->inbox      = cfg::parse_inbox_config(j, tag);
    state->validation = cfg::parse_validation_config(j);
    state->startup    = cfg::parse_startup_config(j, tag);
    state->monitoring = cfg::parse_monitoring_config(j);

    // ── Hub resolution (integrated) ──────────────────────────────────────────
    // Hub dir is resolved relative to the role directory.
    // Broker and pubkey are read from the hub directory if present.
    // This is currently still done by the per-role from_directory() methods
    // and will be migrated here in Phase 3.

    // ── Security check ───────────────────────────────────────────────────────
    warn_if_keyfile_in_role_dir(base_, state->auth.keyfile);

    config_ = std::move(state);
}

bool RoleDirectory::config_loaded() const noexcept
{
    return config_ != nullptr;
}

const cfg::IdentityConfig &RoleDirectory::identity() const
{
    assert(config_ && "load_config() not called");
    return config_->identity;
}

const cfg::AuthConfig &RoleDirectory::auth() const
{
    assert(config_ && "load_config() not called");
    return config_->auth;
}

const cfg::ScriptConfig &RoleDirectory::script() const
{
    assert(config_ && "load_config() not called");
    return config_->script;
}

const cfg::TimingConfig &RoleDirectory::timing() const
{
    assert(config_ && "load_config() not called");
    return config_->timing;
}

const cfg::InboxConfig &RoleDirectory::inbox() const
{
    assert(config_ && "load_config() not called");
    return config_->inbox;
}

const cfg::ValidationConfig &RoleDirectory::validation() const
{
    assert(config_ && "load_config() not called");
    return config_->validation;
}

const cfg::StartupConfig &RoleDirectory::startup() const
{
    assert(config_ && "load_config() not called");
    return config_->startup;
}

const cfg::MonitoringConfig &RoleDirectory::monitoring() const
{
    assert(config_ && "load_config() not called");
    return config_->monitoring;
}

const nlohmann::json &RoleDirectory::raw_json() const
{
    assert(config_ && "load_config() not called");
    return config_->raw;
}

const std::string &RoleDirectory::role_tag() const
{
    assert(config_ && "load_config() not called");
    return config_->role_tag;
}

} // namespace pylabhub::utils
