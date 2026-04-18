/**
 * @file role_directory.cpp
 * @brief RoleDirectory — canonical role directory layout (HEP-CORE-0024).
 */
#include "utils/role_directory.hpp"
#include "utils/uid_utils.hpp"

#include "plh_platform.hpp" // PYLABHUB_IS_POSIX

#include <fmt/core.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#if defined(PYLABHUB_IS_POSIX)
#include <sys/stat.h>
#endif

namespace pylabhub::utils
{

// ── ConfigState (must precede defaulted special members) ──────────────────────

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

// ── Role registration — internal storage (HEP-0024 §10) ─────────────────────

/// Internal representation of registered role init content.
/// Stored inside the shared library — never crosses ABI boundary.
struct RoleInitEntry
{
    std::string config_filename;
    std::string uid_prefix;
    std::string role_label;
    std::function<nlohmann::json(const std::string &, const std::string &)> config_template;
    std::function<void(const RoleDirectory &, const std::string &)> on_init;
};

namespace
{

std::mutex &registry_mutex()
{
    static std::mutex mu;
    return mu;
}

std::unordered_map<std::string, RoleInitEntry> &registry()
{
    static std::unordered_map<std::string, RoleInitEntry> reg;
    return reg;
}

} // namespace

// ── RoleRegistrationBuilder — pimpl implementation ───────────────────────────

struct RoleDirectory::RoleRegistrationBuilder::Impl
{
    std::string  role_tag;
    RoleInitEntry entry;
    bool committed{false};
};

RoleDirectory::RoleRegistrationBuilder::RoleRegistrationBuilder(std::string role_tag)
    : impl_(std::make_unique<Impl>())
{
    impl_->role_tag = std::move(role_tag);
}

RoleDirectory::RoleRegistrationBuilder::~RoleRegistrationBuilder()
{
    if (impl_ && !impl_->committed)
        commit();
}

RoleDirectory::RoleRegistrationBuilder::RoleRegistrationBuilder(RoleRegistrationBuilder &&) noexcept = default;
RoleDirectory::RoleRegistrationBuilder &
RoleDirectory::RoleRegistrationBuilder::operator=(RoleRegistrationBuilder &&) noexcept = default;

RoleDirectory::RoleRegistrationBuilder &
RoleDirectory::RoleRegistrationBuilder::config_filename(std::string filename)
{
    impl_->entry.config_filename = std::move(filename);
    return *this;
}

RoleDirectory::RoleRegistrationBuilder &
RoleDirectory::RoleRegistrationBuilder::uid_prefix(std::string prefix)
{
    impl_->entry.uid_prefix = std::move(prefix);
    return *this;
}

RoleDirectory::RoleRegistrationBuilder &
RoleDirectory::RoleRegistrationBuilder::role_label(std::string label)
{
    impl_->entry.role_label = std::move(label);
    return *this;
}

RoleDirectory::RoleRegistrationBuilder &
RoleDirectory::RoleRegistrationBuilder::config_template(
    std::function<nlohmann::json(const std::string &, const std::string &)> fn)
{
    impl_->entry.config_template = std::move(fn);
    return *this;
}

RoleDirectory::RoleRegistrationBuilder &
RoleDirectory::RoleRegistrationBuilder::on_init(
    std::function<void(const RoleDirectory &, const std::string &)> fn)
{
    impl_->entry.on_init = std::move(fn);
    return *this;
}

void RoleDirectory::RoleRegistrationBuilder::commit()
{
    if (!impl_ || impl_->committed)
        return;
    impl_->committed = true;

    std::lock_guard lk(registry_mutex());
    registry()[impl_->role_tag] = std::move(impl_->entry);
}

// ── register_role — returns builder ──────────────────────────────────────────

RoleDirectory::RoleRegistrationBuilder
RoleDirectory::register_role(const std::string &role_tag)
{
    return RoleRegistrationBuilder(role_tag);
}

// ── init_directory ───────────────────────────────────────────────────────────

int RoleDirectory::init_directory(const std::filesystem::path &dir,
                                   const std::string &role_tag,
                                   const std::string &name,
                                   const LogInitOverrides &log)
{
    namespace fs = std::filesystem;

    // Copy registered entry under lock — no dangling pointer risk (H1 fix).
    RoleInitEntry info;
    {
        std::lock_guard lk(registry_mutex());
        auto it = registry().find(role_tag);
        if (it == registry().end())
        {
            fmt::print(stderr, "init_directory: error: unknown role '{}' — "
                       "not registered via RoleDirectory::register_role()\n",
                       role_tag);
            return 1;
        }
        info = it->second;
    }

    // 1. Pre-check: config file must not already exist (H2 fix — check before create).
    const fs::path target_dir = dir.empty() ? fs::current_path() : dir;
    const fs::path json_path = target_dir / info.config_filename;
    if (fs::exists(json_path))
    {
        fmt::print(stderr, "init_directory: error: {} already exists at '{}'. "
                   "Remove it first or choose a different directory.\n",
                   info.config_filename, json_path.string());
        return 1;
    }

    // 2. Create directory structure.
    RoleDirectory role_dir = RoleDirectory::open(target_dir);
    try
    {
        role_dir = RoleDirectory::create(target_dir);
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "init_directory: error: {}\n", e.what());
        return 1;
    }

    // 3. Validate name (caller must provide — no prompts from the lib).
    if (name.empty())
    {
        fmt::print(stderr, "init_directory: error: role name is required — "
                   "caller must resolve name before calling init_directory()\n");
        return 1;
    }

    // 4. Generate UID.
    const std::string uid = pylabhub::uid::generate_uid(info.uid_prefix, name);

    // 5. Write config template.
    if (info.config_template)
    {
        nlohmann::json j = info.config_template(uid, name);

        // Merge CLI log overrides into the generated JSON's logging section.
        // Keys follow config::LoggingConfig's JSON schema (see
        // logging_config.hpp). Unset overrides leave the template's value
        // (or absence) in place — LoggingConfig::parse_logging_config
        // applies its own defaults when keys are missing.
        if (log.max_size_mb.has_value())
            j["logging"]["max_size_mb"] = *log.max_size_mb;
        if (log.backups.has_value())
            j["logging"]["backups"] = *log.backups;

        std::ofstream out(json_path);
        if (!out)
        {
            fmt::print(stderr, "init_directory: error: cannot write '{}'\n",
                       json_path.string());
            return 1;
        }
        out << j.dump(2) << "\n";
        out.close();
        if (!out)
        {
            fmt::print(stderr, "init_directory: error: write failed for '{}'\n",
                       json_path.string());
            return 1;
        }
    }

    // 6. Role-specific post-init callback (catch exceptions).
    if (info.on_init)
    {
        try
        {
            info.on_init(role_dir, name);
        }
        catch (const std::exception &e)
        {
            fmt::print(stderr, "init_directory: error in post-init callback: {}\n",
                       e.what());
            return 1;
        }
    }

    // 7. Print summary.
    fmt::print("\n{} directory initialised: {}\n"
               "  uid    : {}\n"
               "  name   : {}\n"
               "  config : {}\n",
               info.role_label, role_dir.base().string(),
               uid, name, json_path.string());

    return 0;
}

} // namespace pylabhub::utils
