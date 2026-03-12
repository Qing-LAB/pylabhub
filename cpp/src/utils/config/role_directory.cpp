/**
 * @file role_directory.cpp
 * @brief RoleDirectory — canonical role directory layout (HEP-CORE-0024).
 */
#include "utils/role_directory.hpp"

#include "plh_platform.hpp" // PYLABHUB_IS_POSIX

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>

#if defined(PYLABHUB_IS_POSIX)
#include <sys/stat.h>
#endif

namespace pylabhub::utils
{

// ── Factory methods ────────────────────────────────────────────────────────────

RoleDirectory RoleDirectory::open(const std::filesystem::path &base)
{
    return RoleDirectory(std::filesystem::weakly_canonical(base));
}

RoleDirectory RoleDirectory::from_config_file(const std::filesystem::path &config_path)
{
    return open(config_path.parent_path());
}

RoleDirectory RoleDirectory::create(const std::filesystem::path &base,
                                     std::string_view /*config_filename*/)
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

    const std::string ext = (type == "python") ? ".py" : ".lua";
    return resolved / "script" / type / ("__init__" + ext);
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

// ── Layout inspection ──────────────────────────────────────────────────────────

bool RoleDirectory::has_standard_layout() const
{
    namespace fs = std::filesystem;
    return fs::is_directory(base_ / "logs") &&
           fs::is_directory(base_ / "run") &&
           fs::is_directory(base_ / "vault") &&
           fs::is_directory(base_ / "script" / "python");
}

} // namespace pylabhub::utils
