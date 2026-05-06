/**
 * @file plh_hub_main.cpp
 * @brief plh_hub — single-kind hub binary (HEP-CORE-0033 §15).
 *
 * Replaces the deleted `pylabhub-hubshell`.  Mirrors `plh_role`'s shape
 * minus the `--role` dispatch (the hub is single-kind per HEP-0033 §2.2;
 * `hub_cli::HubArgs` has no role field).
 *
 * ## Usage
 *
 *     plh_hub --init [<hub_dir>] [--name <n>] [--log-maxsize <MB>] [--log-backups <N>]
 *     plh_hub <hub_dir>                                  # Run from directory
 *     plh_hub --config <hub.json> [--validate | --keygen]
 *
 * ## Run flow
 *
 *     ABI check
 *       → signal handler (flips g_shutdown atomic on SIGINT/SIGTERM)
 *       → parse hub_cli::HubArgs
 *       → LifecycleGuard (Logger, FileLock, Crypto, JsonConfig, ZMQContext)
 *       → mode dispatch:
 *           --init                : HubDirectory::init_directory
 *           load HubConfig (load / load_from_directory)
 *           --keygen              : HubConfig::create_keypair + print
 *           --validate            : HubHost::startup() + immediate shutdown
 *           run                   : HubHost::startup() + run_main_loop +
 *                                   shutdown bridge thread that translates
 *                                   the signal-handler atomic into
 *                                   host.request_shutdown() (HubHost has
 *                                   no external shutdown-flag ctor)
 */

#include "engine_factory.hpp"               // src/scripting/

#include "utils/cli_helpers.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_cli.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/role_main_helpers.hpp"      // role_lifecycle_modules,
                                              // register_signal_handler_lifecycle,
                                              // log_version_info

#include "plh_datahub.hpp"                  // LifecycleGuard + utils prelude
#include "plh_version_registry.hpp"          // HEP-CORE-0032 ABI check

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

using namespace pylabhub::utils;
namespace cli       = pylabhub::cli;
namespace hub_cli   = pylabhub::hub_cli;
namespace scripting = pylabhub::scripting;

namespace
{

constexpr const char *kLogTag = "[plh_hub]";

// ─────────────────────────────────────────────────────────────────────────────
// Configure rotating log sink from HubConfig.
//
// Parallel to scripting::configure_logger_from_config but typed on HubConfig
// (the role-side helper takes RoleConfig).  Both configs expose the same
// `logging()` + `identity().uid` + `base_dir()` accessors, but we don't
// template the helper here to avoid touching the role-side header for a
// hub-only binary — duplication is ~25 lines.
// ─────────────────────────────────────────────────────────────────────────────
bool configure_logger_from_hub_config(const pylabhub::config::HubConfig &cfg)
{
    namespace fs = std::filesystem;
    const auto &lc = cfg.logging();

    fs::path base = lc.file_path.empty()
                        ? (cfg.base_dir() / "logs" / (cfg.identity().uid + ".log"))
                        : fs::path(lc.file_path);
    if (base.is_relative())
        base = cfg.base_dir() / base;

    std::error_code mk_ec;
    fs::create_directories(base.parent_path(), mk_ec);
    // (mk_ec swallowed — set_rotating_logfile pre-flight catches real errors.)

    pylabhub::utils::Logger::RotatingLogConfig rcfg{};
    rcfg.max_file_size_bytes = lc.max_size_bytes;
    rcfg.max_backup_files    = lc.max_backup_files;
    rcfg.timestamped_names   = lc.timestamped;
    rcfg.use_flock           = true;

    std::error_code ec;
    if (!pylabhub::utils::Logger::instance().set_rotating_logfile(base, rcfg, ec))
    {
        LOGGER_ERROR("{} Failed to attach rotating log sink at '{}': {}",
                     kLogTag, base.string(), ec.message());
        return false;
    }
    LOGGER_INFO("{} Log sink: {} (max_size={:.1f} MiB, backups={}, timestamped={})",
                kLogTag, base.string(),
                lc.max_size_bytes / (1024.0 * 1024.0),
                (lc.max_backup_files ==
                 pylabhub::config::LoggingConfig::kKeepAllBackups)
                    ? std::string("all")
                    : std::to_string(lc.max_backup_files),
                lc.timestamped);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// --init flow
// ─────────────────────────────────────────────────────────────────────────────
int do_init(const hub_cli::HubArgs &args)
{
    namespace fs = std::filesystem;
    const fs::path dir = args.hub_dir.empty()
                             ? fs::current_path()
                             : fs::path(args.hub_dir);

    const auto name_opt = cli::resolve_init_name(
        args.init_name,
        "Hub name (human-readable, e.g. 'lab1'): ");
    if (!name_opt)
        return 1;

    HubDirectory::LogInitOverrides log_overrides;
    log_overrides.max_size_mb = args.log_max_size_mb;
    log_overrides.backups     = args.log_backups;

    return HubDirectory::init_directory(dir, *name_opt, log_overrides);
}

// ─────────────────────────────────────────────────────────────────────────────
// --keygen flow
// ─────────────────────────────────────────────────────────────────────────────
int do_keygen(pylabhub::config::HubConfig &cfg)
{
    if (cfg.auth().keyfile.empty())
    {
        std::cerr << "Error: --keygen requires 'auth.keyfile' in config\n";
        return 1;
    }

    const auto pw_opt = cli::get_new_password(
        "hub",
        "PYLABHUB_HUB_PASSWORD",
        "Hub vault password (empty = no encryption): ",
        "Confirm password: ");
    if (!pw_opt)
        return 1;

    try
    {
        const auto pubkey = cfg.create_keypair(*pw_opt);
        std::cout << "Hub vault written to: " << cfg.auth().keyfile << "\n"
                  << "  hub_uid    : " << cfg.identity().uid << "\n"
                  << "  public_key : " << pubkey << "\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    // 1. ABI compatibility check (HEP-CORE-0032) — runs BEFORE LifecycleGuard
    //    or any library call that could exercise a mismatched vtable / protocol
    //    field.  Same shape plh_role uses.
    {
        constexpr auto kAbiExpected = pylabhub::version::abi_expected_here();
        const auto abi = pylabhub::version::check_abi(
            kAbiExpected.versions, kAbiExpected.build_id);
        if (!abi.compatible)
        {
            std::fprintf(stderr,
                "[plh_hub] ABI mismatch — refusing to run.\n"
                "  %s\n"
                "  Rebuild this binary against the installed library, "
                "or reinstall a matching library.\n",
                abi.message.c_str());
            return 2;
        }
    }

    // 2. Signal handler (atomic-flag style; bridge thread translates to
    //    host.request_shutdown() once we have a HubHost).
    std::atomic<bool> g_shutdown{false};
    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "plh_hub"}, &g_shutdown);
    signal_handler.install();

    // 3. Parse CLI.  hub_cli::parse_hub_args writes usage / errors to
    //    out_stream / err_stream (defaults to stdout / stderr) and returns
    //    a ParseResult; it does NOT call std::exit.
    auto parsed = hub_cli::parse_hub_args(argc, argv);
    if (parsed.exit_code >= 0)
        return parsed.exit_code;
    const auto &args = parsed.args;

    // 4. LifecycleGuard.  We reuse role_lifecycle_modules() — the included
    //    DataBlock module is unused on the hub side but its lifecycle
    //    init/shutdown is a cheap no-op and adding a separate
    //    hub_lifecycle_modules() helper would just duplicate 5 of 6 lines.
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules());
    scripting::register_signal_handler_lifecycle(signal_handler, kLogTag);
    scripting::log_version_info(kLogTag);

    // 5. --init mode (no config load — we're creating the directory).
    if (args.init_only)
        return do_init(args);

    // 6. Load config.  HubConfig::load throws on any parse / validation error;
    //    load_from_directory wraps load(<dir>/hub.json).
    std::optional<pylabhub::config::HubConfig> config_opt;
    try
    {
        if (!args.hub_dir.empty())
            config_opt.emplace(
                pylabhub::config::HubConfig::load_from_directory(args.hub_dir));
        else
            config_opt.emplace(
                pylabhub::config::HubConfig::load(args.config_path));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }
    auto &cfg = *config_opt;

    // 7. Attach rotating log sink (path auto-composed from config).
    (void) configure_logger_from_hub_config(cfg);

    // 8. --keygen mode.  Operates on the loaded config; doesn't enter the
    //    HubHost lifecycle.
    if (args.keygen_only)
        return do_keygen(cfg);

    // 9. Unlock vault if configured.  HubHost reads the unlocked
    //    auth().client_pubkey/seckey at startup() to decide whether to
    //    enable CURVE on the broker (HEP-0033 §4.1 step 2).
    if (!cfg.auth().keyfile.empty())
    {
        const auto vault_password = cli::get_password(
            "hub",
            "PYLABHUB_HUB_PASSWORD",
            "Hub vault password: ");
        if (!vault_password)
            return 1;
        try
        {
            cfg.load_keypair(*vault_password);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Vault unlock failed: " << e.what() << "\n";
            return 1;
        }
    }

    // 10. Build engine if a script is configured.  HubHost(cfg, engine)
    //     accepts a null engine (script-disabled run); cfg.script().path
    //     non-empty + engine non-null is the script-enabled path
    //     (HEP-0033 §12).
    std::unique_ptr<scripting::ScriptEngine> engine;
    if (!cfg.script().path.empty())
        engine = scripting::make_engine_from_script_config(cfg.script());

    // 11. --validate mode: do a full startup → shutdown round-trip.  This
    //     exercises broker bind, admin bind (if enabled), engine init +
    //     load_script, on_init dispatch, and the orderly shutdown.  Stronger
    //     than a config-only check; equivalent to the role-side
    //     `host->set_validate_only(true) + startup_()` semantic.
    if (args.validate_only)
    {
        try
        {
            pylabhub::hub_host::HubHost host(std::move(cfg), std::move(engine));
            host.startup();
            host.shutdown();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Validation failed: " << e.what() << "\n";
            return 1;
        }
        std::cout << "Validation passed.\n";
        return 0;
    }

    // 12. Run mode.
    pylabhub::hub_host::HubHost host(std::move(cfg), std::move(engine));
    try
    {
        host.startup();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Startup failed: " << e.what() << "\n";
        return 1;
    }

    // 13. Bridge thread — translates the signal-handler atomic into
    //     host.request_shutdown().  HubHost has no external shutdown-flag
    //     ctor; we own the wiring here.  The bridge polls at 100 ms; on
    //     g_shutdown it requests shutdown and exits.  Also exits if the
    //     host stops on its own (e.g., admin-RPC `request_shutdown`) so
    //     the join below is bounded.
    std::thread bridge([&host, &g_shutdown] {
        using namespace std::chrono_literals;
        while (!g_shutdown.load(std::memory_order_acquire))
        {
            if (!host.is_running())
                return;
            std::this_thread::sleep_for(100ms);
        }
        host.request_shutdown();
    });

    // 14. Status callback — printed on demand by the signal-handler watcher
    //     thread (e.g., on a query signal).  Mirrors the plh_role status
    //     shape but with hub-specific fields (broker endpoint instead of
    //     in/out channels).
    const auto start_time = std::chrono::steady_clock::now();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs    = std::chrono::duration_cast<std::chrono::seconds>(
                                 elapsed).count();
        const auto &c = host.config();
        return fmt::format(
            "  pyLabHub {} (plh_hub, script={})\n"
            "  Endpoint:  {}\n"
            "  UID:       {}\n"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            c.script().type,
            host.broker_endpoint(),
            c.identity().uid,
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    // 15. Block until shutdown.  HubHost::run_main_loop returns when
    //     request_shutdown() flips the internal flag (signal handler →
    //     bridge thread → host.request_shutdown(), or admin RPC →
    //     host.request_shutdown() directly).
    host.run_main_loop();

    // 16. Cleanup.  Bridge thread either already exited (host stopped on
    //     its own) or will exit on its next poll once g_shutdown is set
    //     (signal path).  Make sure g_shutdown is set so an admin-driven
    //     stop also unblocks the bridge.
    g_shutdown.store(true, std::memory_order_release);
    if (bridge.joinable())
        bridge.join();

    // 17. Synchronous, ordered teardown (HEP-0033 §4.2 step 2).
    host.shutdown();
    return 0;
}
