/**
 * @file plh_role_main.cpp
 * @brief plh_role — unified role binary that dispatches on --role tag.
 *
 * Replaces the three per-role executables (pylabhub-producer, -consumer,
 * -processor) with a single binary whose role is selected at runtime.
 *
 * Dispatch uses a static map from long-form role tag to a pair of
 * registration functions (init-time + runtime). **Only the role matched
 * by --role gets registered** — unused roles never touch RoleDirectory /
 * RoleRegistry, in line with HEP-0024 §12 (CLI↔Config separation).
 *
 * ## Usage
 *
 *     plh_role --init --role producer [<role_dir>] [--name <n>] \
 *                       [--log-maxsize <MB>] [--log-backups <N>]
 *     plh_role --role producer <role_dir>                   # Run from directory
 *     plh_role --role producer --config <path> [--validate | --keygen]
 *
 * ## Role types (registered on demand)
 *
 *   - producer
 *   - consumer
 *   - processor
 *
 * Custom roles: write a third-party `register_<role>_init()` +
 * `register_<role>_runtime()` pair and extend `kRegistrars` in your fork.
 */

#include "producer_init.hpp"       // src/producer/ (via CMake PRIVATE include)
#include "consumer_init.hpp"       // src/consumer/
#include "processor_init.hpp"      // src/processor/

#include "engine_factory.hpp"      // src/scripting/
#include "utils/config/role_config.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/role_cli.hpp"
#include "utils/role_directory.hpp"
#include "utils/role_host_base.hpp"
#include "utils/role_main_helpers.hpp"
#include "utils/role_registry.hpp"

#include "plh_datahub.hpp"   // LifecycleGuard + hub/utils prelude

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

using namespace pylabhub::utils;
namespace scripting = pylabhub::scripting;
namespace role_cli  = pylabhub::role_cli;

// ── Dispatch map: role tag → (init registrar, runtime registrar) ─────────────

namespace
{

using RegistrarPair = std::pair<void (*)(), void (*)()>;

const std::unordered_map<std::string_view, RegistrarPair> &kRegistrars()
{
    // NOTE on lazy init: static local map built at first call so
    // initialization order with other static objects is deterministic.
    static const std::unordered_map<std::string_view, RegistrarPair> m = {
        {"producer",
         {&pylabhub::producer::register_producer_init,
          &pylabhub::producer::register_producer_runtime}},
        {"consumer",
         {&pylabhub::consumer::register_consumer_init,
          &pylabhub::consumer::register_consumer_runtime}},
        {"processor",
         {&pylabhub::processor::register_processor_init,
          &pylabhub::processor::register_processor_runtime}},
    };
    return m;
}

void print_available_roles(std::ostream &os)
{
    os << "Available roles: ";
    bool first = true;
    for (const auto &[tag, _] : kRegistrars())
    {
        if (!first) os << ", ";
        os << tag;
        first = false;
    }
    os << "\n";
}

/// Resolve + register the role named by @p args.role. Returns the
/// pointer to the RoleRuntimeInfo on success, nullptr on failure
/// (prints diagnostic to stderr).
const RoleRuntimeInfo *register_and_lookup(const role_cli::RoleArgs &args)
{
    if (args.role.empty())
    {
        std::cerr << "Error: --role <tag> is required (plh_role has no default role).\n";
        print_available_roles(std::cerr);
        return nullptr;
    }

    const auto &map = kRegistrars();
    auto it = map.find(args.role);
    if (it == map.end())
    {
        std::cerr << "Error: unknown role '" << args.role << "'.\n";
        print_available_roles(std::cerr);
        return nullptr;
    }

    // Register BOTH the init content (for --init subcommand) and the
    // runtime content (for run/validate/keygen). Cheap; harmless if
    // only one is actually needed.
    (it->second.first)();   // register_<role>_init
    (it->second.second)();  // register_<role>_runtime

    const RoleRuntimeInfo *info = RoleRegistry::get_runtime(args.role);
    if (!info)
    {
        // Would only happen if register_<role>_runtime misbehaved.
        std::cerr << "Internal error: runtime registration failed for '"
                  << args.role << "'.\n";
        return nullptr;
    }
    return info;
}

// ── --init flow ──────────────────────────────────────────────────────────────

int do_init(const role_cli::RoleArgs &args)
{
    namespace fs = std::filesystem;
    const fs::path dir = args.role_dir.empty()
                             ? fs::current_path()
                             : fs::path(args.role_dir);

    const auto name_opt = role_cli::resolve_init_name(
        args.init_name,
        "Role name (human-readable, e.g. 'TempSensor'): ");
    if (!name_opt)
        return 1;

    RoleDirectory::LogInitOverrides log_overrides;
    log_overrides.max_size_mb = args.log_max_size_mb;
    log_overrides.backups     = args.log_backups;

    // args.role here is the long form and matches the RoleDirectory key
    // registered by register_<role>_init().
    return RoleDirectory::init_directory(
        dir, args.role, *name_opt, log_overrides);
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    std::atomic<bool> g_shutdown{false};

    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "plh_role"}, &g_shutdown);
    signal_handler.install();

    // Parse args. The role_name passed to parse_role_args is only used
    // in the usage text ("<role_dir>"); the real selector is args.role.
    auto parsed = role_cli::parse_role_args(argc, argv, "role");
    if (parsed.exit_code >= 0)
        return parsed.exit_code;
    const role_cli::RoleArgs &args = parsed.args;

    const RoleRuntimeInfo *info = register_and_lookup(args);
    if (!info)
        return 1;

    // ── Init mode ─────────────────────────────────────────────────────
    if (args.init_only)
        return do_init(args);

    // ── Lifecycle init (before any JsonConfig/RoleConfig use) ─────────
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules());
    scripting::register_signal_handler_lifecycle(signal_handler, "[plh_role]");
    scripting::log_version_info("[plh_role]");

    // ── Load config ───────────────────────────────────────────────────
    std::optional<pylabhub::config::RoleConfig> config;
    try
    {
        if (!args.role_dir.empty())
            config.emplace(pylabhub::config::RoleConfig::load_from_directory(
                args.role_dir, info->role_tag.c_str(), info->config_parser));
        else
            config.emplace(pylabhub::config::RoleConfig::load(
                args.config_path, info->role_tag.c_str(), info->config_parser));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }
    auto &c = *config;
    const std::string config_dir = c.base_dir().string();

    // ── Attach rotating log sink (path auto-composed from config) ─────
    {
        std::error_code ec;
        scripting::configure_logger_from_config(c, ec, "[plh_role]");
    }

    // ── Keygen mode ───────────────────────────────────────────────────
    if (args.keygen_only)
    {
        if (c.auth().keyfile.empty())
        {
            std::cerr << "Error: --keygen requires '" << info->role_tag
                      << ".auth.keyfile' in config\n";
            return 1;
        }

        const auto pw_opt = role_cli::get_new_role_password(
            info->role_tag.c_str(),
            "Role vault password (empty = no encryption): ",
            "Confirm password: ");
        if (!pw_opt)
            return 1;

        try
        {
            const auto pubkey = c.create_keypair(*pw_opt);
            std::cout << info->role_label << " vault written to: "
                      << c.auth().keyfile << "\n"
                      << "  role_uid   : " << c.identity().uid << "\n"
                      << "  public_key : " << pubkey << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── Auth: unlock vault if configured ──────────────────────────────
    if (!c.auth().keyfile.empty())
    {
        const auto vault_password = scripting::get_role_password(
            info->role_tag.c_str(), "Role vault password: ");
        if (!vault_password)
            return 1;
        c.load_keypair(*vault_password);
    }

    // ── Engine + host via registry factory ────────────────────────────
    auto engine = scripting::make_engine_from_script_config(c.script());
    auto host = info->host_factory(
        std::move(*config), std::move(engine), &g_shutdown);
    host->set_validate_only(args.validate_only);

    try { host->startup_(); }
    catch (const std::exception &e)
    {
        std::cerr << "Startup failed: " << e.what() << "\n";
        return 1;
    }

    if (!host->script_load_ok())
    {
        std::cerr << "Script load failed.\n";
        host->shutdown_();
        return 1;
    }

    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        host->shutdown_();
        return 0;
    }

    if (!host->is_running())
    {
        std::cerr << "Failed to start " << info->role_tag
                  << " — loop did not start.\n";
        host->shutdown_();
        return 1;
    }

    // ── Status callback (generic: shows role + channel info from config) ──
    const auto start_time = std::chrono::steady_clock::now();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        const auto &cfg = host->config();
        // Show input/output channels when set; cfg.in_channel() /
        // out_channel() return empty for sides the role doesn't use.
        std::string channels;
        if (!cfg.in_channel().empty())
            channels += fmt::format("  in_channel:  {}\n", cfg.in_channel());
        if (!cfg.out_channel().empty())
            channels += fmt::format("  out_channel: {}\n", cfg.out_channel());
        return fmt::format(
            "  pyLabHub {} (plh_role, role={}, script={})\n"
            "  Config:    {}\n"
            "  UID:       {}\n"
            "{}"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            info->role_tag, cfg.script().type,
            config_dir, cfg.identity().uid,
            channels,
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, *host, "[plh_role]");
    host->shutdown_();
    return 0;
}
