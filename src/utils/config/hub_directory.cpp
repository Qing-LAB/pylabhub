/**
 * @file hub_directory.cpp
 * @brief HubDirectory — canonical hub directory layout (HEP-CORE-0033 §7).
 */
#include "utils/hub_directory.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/uid_utils.hpp"

#include "plh_platform.hpp" // PYLABHUB_IS_POSIX

#include <fmt/core.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>

#if defined(PYLABHUB_IS_POSIX)
#include <sys/stat.h>
#endif

namespace pylabhub::utils
{

// ── Special members ──────────────────────────────────────────────────────────

HubDirectory::HubDirectory(std::filesystem::path base) noexcept
    : base_(std::move(base))
{
}
HubDirectory::~HubDirectory() = default;
HubDirectory::HubDirectory(HubDirectory &&) noexcept = default;
HubDirectory &HubDirectory::operator=(HubDirectory &&) noexcept = default;

// ── Factory methods ──────────────────────────────────────────────────────────

HubDirectory HubDirectory::open(const std::filesystem::path &base)
{
    return HubDirectory(std::filesystem::weakly_canonical(base));
}

HubDirectory HubDirectory::from_config_file(const std::filesystem::path &config_path)
{
    return open(config_path.parent_path());
}

HubDirectory HubDirectory::create(const std::filesystem::path &base)
{
    namespace fs = std::filesystem;

    auto make_dir = [](const fs::path &p)
    {
        std::error_code ec;
        fs::create_directories(p, ec);
        if (ec)
            throw std::runtime_error(
                "HubDirectory: cannot create directory '" + p.string() +
                "': " + ec.message());
    };

    make_dir(base / "logs");
    make_dir(base / "run");
    make_dir(base / "vault");
    make_dir(base / "schemas");
    make_dir(base / "script" / "python");

#if defined(PYLABHUB_IS_POSIX)
    // Restrict vault/ to owner-only access for keypair security.
    const fs::path vp = base / "vault";
    if (::chmod(vp.c_str(), 0700) != 0)
    {
        // Non-fatal: vault was created; warn but don't abort.
        std::fprintf(stderr,
                     "[hub_dir] Warning: could not set 0700 on vault '%s'\n",
                     vp.c_str());
    }
#endif

    return open(base);
}

// ── Path helpers ─────────────────────────────────────────────────────────────

std::filesystem::path HubDirectory::script_entry(std::string_view script_path,
                                                  std::string_view type) const
{
    namespace fs = std::filesystem;

    const fs::path sp(script_path);
    const fs::path resolved = sp.is_absolute()
                              ? sp
                              : fs::weakly_canonical(base_ / sp);

    // Mirrors RoleDirectory::script_entry — Python: __init__.py, Lua: init.lua.
    const char *entry = (type == "lua") ? "init.lua" : "__init__.py";
    return resolved / "script" / type / entry;
}

// ── Layout inspection ────────────────────────────────────────────────────────

bool HubDirectory::has_standard_layout() const
{
    namespace fs = std::filesystem;
    return fs::is_directory(base_ / "logs") &&
           fs::is_directory(base_ / "run") &&
           fs::is_directory(base_ / "vault");
    // script/ and schemas/ are OPTIONAL per HEP-0033 §7 — not checked.
}

// ── init_directory ───────────────────────────────────────────────────────────

namespace
{

/// Build the default hub.json template (HEP-0033 §6.2 minus the
/// auth/access fields deferred to HEP-0035).
nlohmann::json build_hub_json_template(const std::string &uid,
                                        const std::string &name)
{
    return nlohmann::json{
        {"hub", {
            {"uid",       uid},
            {"name",      name},
            {"log_level", "info"},
            {"auth",      {{"keyfile", "vault/hub.vault"}}},
        }},
        {"script",               {{"type", "python"}, {"path", "."}}},
        {"python_venv",          ""},
        {"stop_on_script_error", false},
        // Hub script tick (HEP-CORE-0033 Phase 7).  Same shape and
        // semantics as the role-side data loop's pacing — see
        // `utils/loop_timing_policy.hpp`.  Default `fixed_rate` at 1 Hz
        // gives `on_tick(api)` once per second; switch to `max_rate`
        // for continuous polling, or omit/raise the period for slower
        // ticks.  `loop_timing` is REQUIRED at parse time.
        {"loop_timing",          "fixed_rate"},
        {"target_period_ms",     1000},
        {"logging", {
            {"file_path",    ""},
            {"max_size_mb",  10},
            {"backups",      5},
            {"timestamped",  true},
        }},
        {"network", {
            // Default to loopback so single-machine demos work out of
            // the box.  The same string is read by role-side
            // `HubRefConfig::parse_hub_ref_config` as the *connect*
            // target — `tcp://0.0.0.0:5570` would have been a correct
            // bind address but an unreachable connect target.
            // Operators deploying cross-host edit this to the hub's
            // externally-visible address.  See HEP-CORE-0033 §6.2.
            {"broker_endpoint", "tcp://127.0.0.1:5570"},
            {"broker_bind",     true},
            {"zmq_io_threads",  1},
        }},
        {"admin", {
            {"enabled",        true},
            {"endpoint",       "tcp://127.0.0.1:5600"},
            {"token_required", true},
        }},
        {"broker", {
            {"heartbeat_interval_ms",   ::pylabhub::kDefaultHeartbeatIntervalMs},
            {"ready_miss_heartbeats",   ::pylabhub::kDefaultReadyMissHeartbeats},
            {"pending_miss_heartbeats", ::pylabhub::kDefaultPendingMissHeartbeats},
            {"grace_heartbeats",        ::pylabhub::kDefaultGraceHeartbeats},
        }},
        {"federation", {
            {"enabled",            false},
            {"peers",              nlohmann::json::array()},
            {"forward_timeout_ms", 2000},
        }},
        {"state", {
            {"disconnected_grace_ms",    60000},
            {"max_disconnected_entries", 1000},
        }},
    };
}

} // namespace

int HubDirectory::init_directory(const std::filesystem::path &dir,
                                  const std::string &name,
                                  const LogInitOverrides &log)
{
    namespace fs = std::filesystem;

    if (name.empty())
    {
        fmt::print(stderr, "init_directory: error: hub name is required — "
                   "caller must resolve name before calling init_directory()\n");
        return 1;
    }

    // 1. Pre-check: hub.json must not already exist.
    const fs::path target_dir = dir.empty() ? fs::current_path() : dir;
    const fs::path json_path = target_dir / "hub.json";
    if (fs::exists(json_path))
    {
        fmt::print(stderr, "init_directory: error: hub.json already exists at "
                   "'{}'. Remove it first or choose a different directory.\n",
                   json_path.string());
        return 1;
    }

    // 2. Create directory structure.
    HubDirectory hub_dir = HubDirectory::open(target_dir);
    try
    {
        hub_dir = HubDirectory::create(target_dir);
    }
    catch (const std::exception &e)
    {
        fmt::print(stderr, "init_directory: error: {}\n", e.what());
        return 1;
    }

    // 3. Generate hub uid.
    const std::string uid = pylabhub::uid::generate_hub_uid(name);

    // 4. Write hub.json template.
    nlohmann::json j = build_hub_json_template(uid, name);

    if (log.max_size_mb.has_value())
        j["logging"]["max_size_mb"] = *log.max_size_mb;
    if (log.backups.has_value())
        j["logging"]["backups"] = *log.backups;

    {
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

    // 5. Print summary.
    fmt::print("\nHub directory initialised: {}\n"
               "  uid    : {}\n"
               "  name   : {}\n"
               "  config : {}\n",
               hub_dir.base().string(), uid, name, json_path.string());

    return 0;
}

} // namespace pylabhub::utils
