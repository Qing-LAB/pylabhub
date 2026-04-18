/**
 * @file producer_main.cpp
 * @brief pylabhub-producer — standalone data-producer host process.
 *
 * ## Usage
 *
 *     pylabhub-producer <prod_dir>                  # Run from directory
 *     pylabhub-producer --config <path.json>        # Run from explicit config file
 *     pylabhub-producer --config <path.json> --validate   # Validate config; exit 0/1
 *     pylabhub-producer --config <path.json> --keygen     # Generate producer keypair; exit 0
 *     pylabhub-producer --init [<prod_dir>]         # Create producer directory; exit 0
 *
 * ## Config format
 *
 *     {
 *       "producer": { "uid": "PROD-TEMPSENS-12345678", "name": "TempSensor" },
 *       "out_hub_dir":        "/var/pylabhub/my_hub",
 *       "out_channel":        "lab.sensors.temperature",
 *       "target_period_ms":   100,
 *       "out_transport":      "shm",
 *       "out_shm_enabled":    true,
 *       "out_shm_slot_count": 8,
 *       "out_slot_schema":    { "fields": [{"name": "value", "type": "float32"}] },
 *       "checksum": "enforced",
 *       "script": { "type": "python", "path": "." }
 *     }
 *
 * ## Python script interface
 *
 *     # script/python/__init__.py
 *     import pylabhub_producer as prod
 *
 *     def on_init(api: prod.ProducerAPI) -> None:
 *         api.log('info', f"Producer {api.uid()} starting")
 *
 *     def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:
 *         out_slot.value = 42.0
 *         return True
 *
 *     def on_stop(api: prod.ProducerAPI) -> None:
 *         api.log('info', "Producer stopping")
 */

#include "producer_init.hpp"
#include "producer_role_host.hpp"
#include "producer_fields.hpp"
#include "engine_factory.hpp"

#include "plh_datahub.hpp"
#include "utils/config/role_config.hpp"
#include "utils/role_cli.hpp"
#include "utils/role_directory.hpp"
#include "utils/uid_utils.hpp"
#include "utils/zmq_context.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/role_main_helpers.hpp"

#include "utils/json_fwd.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>


using namespace pylabhub::utils;
namespace scripting  = pylabhub::scripting;
namespace role_cli   = pylabhub::role_cli;

// ---------------------------------------------------------------------------
// do_init — create producer directory with producer.json template
// ---------------------------------------------------------------------------

// do_init: binary-side CLI wrapper around RoleDirectory::init_directory().
// - Binary handles user interaction (name prompt via role_cli).
// - Library (init_directory) does the pure work: create dir + write templates.
static int do_init(const role_cli::RoleArgs &args)
{
    namespace fs = std::filesystem;

    const fs::path prod_dir = args.role_dir.empty()
                              ? fs::current_path()
                              : fs::path(args.role_dir);

    const auto name_opt = role_cli::resolve_init_name(
        args.init_name, "Producer name (human-readable, e.g. 'TempSensor'): ");
    if (!name_opt)
        return 1;

    RoleDirectory::LogInitOverrides log_overrides;
    log_overrides.max_size_mb = args.log_max_size_mb;
    log_overrides.backups     = args.log_backups;

    return RoleDirectory::init_directory(
        prod_dir, "producer", *name_opt, log_overrides);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::atomic<bool> g_shutdown{false};

    // Register role-specific init content before parsing args.
    pylabhub::producer::register_producer_init();
    // Register runtime content (host factory + callbacks) for the
    // unified plh_role dispatch path. Harmless for the standalone
    // producer binary — plh_role is what consumes RoleRegistry.
    pylabhub::producer::register_producer_runtime();

    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "pylabhub-producer"}, &g_shutdown);
    signal_handler.install();

    auto parsed = role_cli::parse_role_args(argc, argv, "producer");
    if (parsed.exit_code >= 0)
        return parsed.exit_code;
    const role_cli::RoleArgs &args = parsed.args;

    if (args.init_only)
        return do_init(args);

    // ── Lifecycle init (required before JsonConfig/RoleConfig) ────────────────
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules());
    scripting::register_signal_handler_lifecycle(signal_handler, "[prod-main]");
    scripting::log_version_info("[prod-main]");

    // ── Load config via RoleConfig ───────────────────────────────────────────
    std::optional<pylabhub::config::RoleConfig> config;
    try
    {
        if (!args.role_dir.empty())
            config.emplace(pylabhub::config::RoleConfig::load_from_directory(
                args.role_dir, "producer", pylabhub::producer::parse_producer_fields));
        else
            config.emplace(pylabhub::config::RoleConfig::load(
                args.config_path, "producer", pylabhub::producer::parse_producer_fields));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }
    auto &c = *config;

    const std::string config_dir = c.base_dir().string();

    // ── Attach rotating log sink (path auto-composed from config) ─────────────
    {
        std::error_code ec;
        scripting::configure_logger_from_config(c, ec, "[prod-main]");
        // Non-fatal on failure — logger continues on stderr; message
        // already surfaced by configure_logger_from_config().
    }

    // ── Keygen mode ──────────────────────────────────────────────────────────
    if (args.keygen_only)
    {
        if (c.auth().keyfile.empty())
        {
            std::cerr << "Error: --keygen requires 'producer.auth.keyfile' in config\n";
            return 1;
        }

        const auto pw_opt = role_cli::get_new_role_password(
            "producer",
            "Producer vault password (empty = no encryption): ",
            "Confirm password: ");
        if (!pw_opt)
            return 1;

        try
        {
            const auto pubkey = c.create_keypair(*pw_opt);
            std::cout << "Producer vault written to: " << c.auth().keyfile << "\n"
                      << "  producer_uid : " << c.identity().uid << "\n"
                      << "  public_key   : " << pubkey << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── Auth ─────────────────────────────────────────────────────────────────
    if (!c.auth().keyfile.empty())
    {
        const auto vault_password = scripting::get_role_password("producer", "Producer vault password: ");
        if (!vault_password)
            return 1;
        c.load_keypair(*vault_password);
    }

    // ── Create engine based on script type ───────────────────────────────────
    auto engine = scripting::make_engine_from_script_config(c.script());

    // ── Run role host ────────────────────────────────────────────────────────
    pylabhub::producer::ProducerRoleHost host(
        std::move(*config), std::move(engine), &g_shutdown);
    host.set_validate_only(args.validate_only);

    try { host.startup_(); }
    catch (const std::exception &e)
    {
        std::cerr << "Startup failed: " << e.what() << "\n";
        return 1;
    }

    if (!host.script_load_ok())
    {
        std::cerr << "Script load failed.\n";
        host.shutdown_();
        return 1;
    }

    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        host.shutdown_();
        return 0;
    }

    if (!host.is_running())
    {
        std::cerr << "Failed to start producer — loop did not start.\n";
        host.shutdown_();
        return 1;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const auto &cfg = host.config();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        return fmt::format(
            "  pyLabHub {} (pylabhub-producer, {})\n"
            "  Config:    {}\n"
            "  UID:       {}\n"
            "  Channel:   {}\n"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            cfg.script().type,
            config_dir, cfg.identity().uid, cfg.out_channel(),
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, host, "[prod-main]");
    host.shutdown_();
    return 0;
}
