/**
 * @file processor_main.cpp
 * @brief pylabhub-processor — standalone data-processor host process.
 *
 * ## Usage
 *
 *     pylabhub-processor <proc_dir>                  # Run from directory
 *     pylabhub-processor --config <path.json>        # Run from explicit config file
 *     pylabhub-processor --config <path.json> --validate   # Validate config; exit 0/1
 *     pylabhub-processor --config <path.json> --keygen     # Generate processor keypair; exit 0
 *     pylabhub-processor --init [<proc_dir>]         # Create processor directory; exit 0
 *
 * When <proc_dir> is given, processor.json is read from <proc_dir>/processor.json.
 * Hub broker endpoint and pubkey are resolved from <in/out_hub_dir>/hub.json.
 *
 * ## Config format
 *
 *     {
 *       "processor": { "uid": "PROC-Doubler-12345678", "name": "Doubler" },
 *       "in_hub_dir":         "/var/pylabhub/hub_a",
 *       "out_hub_dir":        "/var/pylabhub/hub_b",
 *       "in_channel":         "lab.sensor.raw",
 *       "out_channel":        "lab.sensor.processed",
 *       "in_transport":       "shm",
 *       "out_transport":      "shm",
 *       "out_shm_slot_count": 4,
 *       "in_slot_schema":     { "fields": [{"name": "value", "type": "float32"}] },
 *       "out_slot_schema":    { "fields": [{"name": "value", "type": "float32"}] },
 *       "checksum": "enforced",
 *       "script": { "type": "python", "path": "." }
 *     }
 *
 * ## Python script interface
 *
 *     # script/python/__init__.py
 *     import pylabhub_processor as proc
 *
 *     def on_init(api: proc.ProcessorAPI) -> None:
 *         api.log('info', f"Processor {api.uid()} starting")
 *
 *     def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:
 *         # in_slot   — ctypes/numpy read-only view, or None on timeout
 *         # out_slot  — ctypes/numpy writable view, or None on timeout
 *         # flexzone  — persistent output flexzone ctypes struct, or None
 *         # messages  — list of (sender: str, data: bytes)
 *         # api       — ProcessorAPI proxy
 *         if in_slot is None:
 *             return False
 *         out_slot.value = in_slot.value * 2.0
 *         return True  # True/None=commit; False=discard
 *
 *     def on_stop(api: proc.ProcessorAPI) -> None:
 *         api.log('info', "Processor stopping")
 */

#include "processor_init.hpp"
#include "processor_role_host.hpp"
#include "processor_fields.hpp"
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
// do_init — create processor directory with processor.json template
// ---------------------------------------------------------------------------

// do_init: binary-side CLI wrapper around RoleDirectory::init_directory().
static int do_init(const std::string &proc_dir_str, const std::string &cli_name)
{
    namespace fs = std::filesystem;

    const fs::path proc_dir = proc_dir_str.empty()
                              ? fs::current_path()
                              : fs::path(proc_dir_str);

    const auto name_opt = role_cli::resolve_init_name(
        cli_name, "Processor name (human-readable, e.g. 'Doubler'): ");
    if (!name_opt)
        return 1;

    return RoleDirectory::init_directory(proc_dir, "processor", *name_opt);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::atomic<bool> g_shutdown{false};

    // Register role-specific init content before parsing args.
    pylabhub::processor::register_processor_init();
    // Register runtime content (host factory + callbacks) for the
    // unified plh_role dispatch path. Harmless for the standalone
    // processor binary — plh_role is what consumes RoleRegistry.
    pylabhub::processor::register_processor_runtime();

    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "pylabhub-processor"}, &g_shutdown);
    signal_handler.install();

    // ── Parse arguments ───────────────────────────────────────────────────────
    const role_cli::RoleArgs args = role_cli::parse_role_args(argc, argv, "processor");

    // ── Init mode: create processor directory and exit ────────────────────────
    if (args.init_only)
        return do_init(args.role_dir, args.init_name);

    // ── Lifecycle init (required before JsonConfig/RoleConfig) ────────────────
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules(args.log_file));
    scripting::register_signal_handler_lifecycle(signal_handler, "[proc-main]");
    scripting::log_version_info("[proc-main]");

    // ── Load config via RoleConfig ───────────────────────────────────────────
    std::optional<pylabhub::config::RoleConfig> config;
    try
    {
        if (!args.role_dir.empty())
            config.emplace(pylabhub::config::RoleConfig::load_from_directory(
                args.role_dir, "processor", pylabhub::processor::parse_processor_fields));
        else
            config.emplace(pylabhub::config::RoleConfig::load(
                args.config_path, "processor", pylabhub::processor::parse_processor_fields));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }
    auto &c = *config;

    const std::string config_dir = c.base_dir().string();

    // ── Keygen mode ──────────────────────────────────────────────────────────
    if (args.keygen_only)
    {
        if (c.auth().keyfile.empty())
        {
            std::cerr << "Error: --keygen requires 'processor.auth.keyfile' in config\n";
            return 1;
        }

        const auto pw_opt = role_cli::get_new_role_password(
            "processor",
            "Processor vault password (empty = no encryption): ",
            "Confirm password: ");
        if (!pw_opt)
            return 1;

        try
        {
            const auto pubkey = c.create_keypair(*pw_opt);
            std::cout << "Processor vault written to: " << c.auth().keyfile << "\n"
                      << "  processor_uid : " << c.identity().uid << "\n"
                      << "  public_key    : " << pubkey << "\n";
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
        const auto vault_password = scripting::get_role_password("processor", "Processor vault password: ");
        if (!vault_password)
            return 1;
        c.load_keypair(*vault_password);
    }

    // ── Create engine based on script type ───────────────────────────────────
    auto engine = scripting::make_engine_from_script_config(c.script());

    // ── Run role host ────────────────────────────────────────────────────────
    pylabhub::processor::ProcessorRoleHost host(
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
        std::cerr << "Failed to start processor — loop did not start.\n";
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
            "  pyLabHub {} (pylabhub-processor, {})\n"
            "  Config:      {}\n"
            "  UID:         {}\n"
            "  In channel:  {}\n"
            "  Out channel: {}\n"
            "  Uptime:      {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            cfg.script().type,
            config_dir, cfg.identity().uid, cfg.in_channel(), cfg.out_channel(),
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, host, "[proc-main]");
    host.shutdown_();
    return 0;
}
