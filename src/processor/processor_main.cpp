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
 * If processor.json contains a "hub_dir" key, broker endpoint and pubkey are
 * loaded from <hub_dir>/hub.json and <hub_dir>/hub.pubkey, overriding the inline values.
 *
 * ## Config format
 *
 *     {
 *       "processor": { "uid": "PROC-Doubler-12345678", "name": "Doubler", "log_level": "info" },
 *       "hub_dir": "/var/pylabhub/my_hub",
 *       "in_channel":  "lab.sensor.raw",
 *       "out_channel": "lab.sensor.processed",
 *       "slot_acquire_timeout_ms": -1,
 *       "overflow_policy": "drop",
 *       "shm": {
 *         "in":  { "enabled": true, "secret": 0 },
 *         "out": { "enabled": true, "secret": 0, "slot_count": 4 }
 *       },
 *       "in_slot_schema":   { "fields": [{"name": "value", "type": "float32"}] },
 *       "out_slot_schema":  { "fields": [{"name": "value", "type": "float32"}] },
 *       "flexzone_schema":  null,
 *       "script": { "path": "." }
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

#include "processor_config.hpp"
#include "processor_script_host.hpp"
#include "lua_processor_host.hpp"

#include "plh_datahub.hpp"
#include "utils/actor_vault.hpp"
#include "utils/role_cli.hpp"
#include "utils/role_directory.hpp"
#include "utils/uid_utils.hpp"
#include "utils/zmq_context.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/timeout_constants.hpp"
#include "role_main_helpers.hpp"

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

static int do_init(const std::string &proc_dir_str, const std::string &cli_name)
{
    namespace fs = std::filesystem;

    const fs::path proc_dir = proc_dir_str.empty()
                              ? fs::current_path()
                              : fs::path(proc_dir_str);

    try
    {
        RoleDirectory::create(proc_dir);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    const fs::path json_path = proc_dir / "processor.json";
    if (fs::exists(json_path))
    {
        std::cerr << "Error: processor.json already exists at '" << json_path.string()
                  << "'. Remove it first or choose a different directory.\n";
        return 1;
    }

    // ── Resolve processor name ────────────────────────────────────────────────
    const auto name_opt = role_cli::resolve_init_name(
        cli_name, "Processor name (human-readable, e.g. 'Doubler'): ");
    if (!name_opt)
        return 1;
    const std::string proc_name = *name_opt;

    const std::string proc_uid = pylabhub::uid::generate_processor_uid(proc_name);

    // ── Build processor.json template ─────────────────────────────────────────
    nlohmann::json j;
    j["hub_dir"] = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";

    j["processor"]["uid"]       = proc_uid;
    j["processor"]["name"]      = proc_name;
    j["processor"]["log_level"] = "info";
    // auth.keyfile: set to a valid path, then run --keygen to create the encrypted vault.
    j["processor"]["auth"]["keyfile"] = "";

    j["in_channel"]  = "lab.source.channel";
    j["out_channel"] = "lab.output.channel";
    j["slot_acquire_timeout_ms"] = -1;
    j["overflow_policy"] = "drop";

    j["shm"]["in"]["enabled"]  = true;
    j["shm"]["in"]["secret"]   = 0;
    j["shm"]["out"]["enabled"]     = true;
    j["shm"]["out"]["secret"]      = 0;
    j["shm"]["out"]["slot_count"]  = 4;

    j["in_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["flexzone_schema"] = nullptr;

    // script.path is the base directory containing the "script/" package.
    // "." means <proc_dir>/script/python/__init__.py (the default layout).
    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

    // ── Dual-broker (optional) ─────────────────────────────────────────────
    // Uncomment to use separate brokers for input and output channels:
    // j["in_hub_dir"]         = "/path/to/input_hub";
    // j["out_hub_dir"]        = "/path/to/output_hub";
    // j["in_broker"]          = "tcp://192.168.1.10:5570";
    // j["out_broker"]         = "tcp://192.168.1.20:5570";
    // j["in_broker_pubkey"]   = "";
    // j["out_broker_pubkey"]  = "";

    j["validation"]["update_checksum"]      = true;
    j["validation"]["stop_on_script_error"] = false;

    // ── Write processor.json ──────────────────────────────────────────────────
    std::ofstream out(json_path);
    if (!out)
    {
        std::cerr << "Error: cannot write '" << json_path.string() << "'\n";
        return 1;
    }
    out << j.dump(2) << "\n";
    out.close();

    // ── Write script/python/__init__.py template ──────────────────────────────
    const fs::path init_py = proc_dir / "script" / "python" / "__init__.py";
    std::ofstream py_out(init_py);
    if (!py_out)
    {
        std::cerr << "Error: cannot write '" << init_py.string() << "'\n";
        return 1;
    }
    py_out <<
        "\"\"\"Processor: " << proc_name << "\n"
        "\n"
        "Script callbacks for the pylabhub-processor host.\n"
        "Edit this file to implement your data-transformation logic.\n"
        "\n"
        "Directory layout:\n"
        "  script/python/            <- this Python package\n"
        "    __init__.py             <- entry point (this file)\n"
        "\n"
        "Import siblings with relative imports:\n"
        "  from . import helpers     <- loads ./helpers.py\n"
        "\"\"\"\n"
        "import pylabhub_processor as proc\n"
        "\n"
        "\n"
        "def on_init(api: proc.ProcessorAPI) -> None:\n"
        "    \"\"\"Called once before the processing loop starts.\"\"\"\n"
        "    api.log('info', f\"on_init: uid={api.uid()}\")\n"
        "\n"
        "\n"
        "def on_process(in_slot, out_slot, flexzone, messages, api: proc.ProcessorAPI) -> bool:\n"
        "    \"\"\"\n"
        "    Called for each input slot.\n"
        "\n"
        "    in_slot:  ctypes/numpy read-only view of the input SHM slot, or None on timeout.\n"
        "    out_slot: ctypes/numpy writable view of the output SHM slot, or None on timeout.\n"
        "    flexzone: persistent ctypes struct for the output flexzone, or None.\n"
        "    messages: list of (sender: str, data: bytes) from the ZMQ peer channel.\n"
        "    api:      ProcessorAPI — log, broadcast, stop, etc.\n"
        "\n"
        "    Return True or None to commit the output slot.\n"
        "    Return False to discard without publishing.\n"
        "    \"\"\"\n"
        "    if in_slot is None:\n"
        "        return False\n"
        "\n"
        "    for sender, data in messages:\n"
        "        api.log('debug', f\"msg from {sender}: {data!r}\")\n"
        "\n"
        "    # TODO: replace with real transformation\n"
        "    out_slot.value = in_slot.value\n"
        "    return True\n"
        "\n"
        "\n"
        "def on_stop(api: proc.ProcessorAPI) -> None:\n"
        "    \"\"\"Called once after the processing loop exits.\"\"\"\n"
        "    api.log('info', f\"on_stop: uid={api.uid()}\")\n";
    py_out.close();

    // ── Summary ───────────────────────────────────────────────────────────────
    std::cout << "\nProcessor directory initialised: " << proc_dir.string() << "\n"
              << "  proc_uid  : " << proc_uid << "\n"
              << "  proc_name : " << proc_name << "\n"
              << "  processor.json : " << json_path.string() << "\n"
              << "  script    : " << init_py.string() << "\n\n"
              << "Next steps:\n"
              << "  1. Edit processor.json — set 'hub_dir' to your hub directory path\n"
              << "  2. Edit processor.json — set 'in_channel' and 'out_channel'\n"
              << "  3. Edit processor.json — set 'processor.auth.keyfile' to a path,\n"
              << "     then generate the keypair:\n"
              << "       pylabhub-processor --config " << json_path.string() << " --keygen\n"
              << "  4. Edit script/python/__init__.py — implement on_process\n"
              << "  5. Run: pylabhub-processor " << proc_dir.string() << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::atomic<bool> g_shutdown{false};

    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "pylabhub-processor"}, &g_shutdown);
    signal_handler.install();

    // ── Parse arguments ───────────────────────────────────────────────────────
    const role_cli::RoleArgs args = role_cli::parse_role_args(argc, argv, "processor");

    // ── Init mode: create processor directory and exit ────────────────────────
    if (args.init_only)
    {
        return do_init(args.role_dir, args.init_name);
    }

    // ── Load config ───────────────────────────────────────────────────────────
    pylabhub::processor::ProcessorConfig config;
    try
    {
        if (!args.role_dir.empty())
            config = pylabhub::processor::ProcessorConfig::from_directory(args.role_dir);
        else
            config = pylabhub::processor::ProcessorConfig::from_json_file(args.config_path);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    // ── keygen mode: generate NaCl CURVE keypair and exit ────────────────────
    if (args.keygen_only)
    {
        if (config.auth.keyfile.empty())
        {
            std::cerr << "Error: --keygen requires 'processor.auth.keyfile' in config\n"
                      << "  Set \"processor\": { \"auth\": { \"keyfile\": \"proc.key\" } } "
                         "in processor.json\n";
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
            const auto vault = pylabhub::utils::ActorVault::create(
                config.auth.keyfile, config.processor_uid, *pw_opt);

            std::cout << "Processor vault written to: " << config.auth.keyfile << "\n"
                      << "  processor_uid : " << config.processor_uid << "\n"
                      << "  public_key    : " << vault.public_key() << "\n"
                      << "  (secret_key encrypted in vault — keep vault file private)\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── Lifecycle guard ───────────────────────────────────────────────────────
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules(args.log_file));
    scripting::register_signal_handler_lifecycle(signal_handler, "[proc-main]");

    // ── Load processor keypair (if keyfile configured) ────────────────────────
    if (!config.auth.keyfile.empty())
    {
        const auto vault_password = scripting::get_role_password("processor", "Processor vault password: ");
        if (!vault_password)
            return 1;
        config.auth.load_keypair(config.processor_uid, *vault_password);
    }

    // Resolve the config directory for status display.
    const std::string config_dir = !args.role_dir.empty()
        ? args.role_dir
        : std::filesystem::path(args.config_path).parent_path().string();

    // ── Dispatch based on script engine ────────────────────────────────────────
    if (config.script_type == "lua")
    {
        pylabhub::processor::LuaProcessorHost lua_host;
        lua_host.set_config(std::move(config));
        lua_host.set_validate_only(args.validate_only);
        lua_host.set_shutdown_flag(&g_shutdown);

        try { lua_host.startup_(); }
        catch (const std::exception &e)
        {
            std::cerr << "Lua startup failed: " << e.what() << "\n";
            return 1;
        }

        if (!lua_host.script_load_ok())
        {
            std::cerr << "Lua script load failed.\n";
            lua_host.shutdown_();
            return 1;
        }

        if (args.validate_only)
        {
            std::cout << "\nValidation passed.\n";
            lua_host.shutdown_();
            return 0;
        }

        if (!lua_host.is_running())
        {
            std::cerr << "Failed to start Lua processor — loop did not start.\n";
            lua_host.shutdown_();
            return 1;
        }

        const auto start_time = std::chrono::steady_clock::now();
        const auto &cfg = lua_host.config();
        signal_handler.set_status_callback([&]() -> std::string
        {
            const auto elapsed = std::chrono::steady_clock::now() - start_time;
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            return fmt::format(
                "  pyLabHub {} (pylabhub-processor, lua)\n"
                "  Config:      {}\n"
                "  UID:         {}\n"
                "  In channel:  {}\n"
                "  Out channel: {}\n"
                "  Uptime:      {}h {}m {}s",
                pylabhub::platform::get_version_string(),
                config_dir, cfg.processor_uid, cfg.in_channel, cfg.out_channel,
                secs / 3600, (secs % 3600) / 60, secs % 60);
        });

        scripting::run_role_main_loop(g_shutdown, lua_host, "[proc-main]");
        lua_host.shutdown_();
        return 0;
    }

    // ── Python path (default) ──────────────────────────────────────────────────
    // ProcessorScriptHost owns the Python interpreter lifetime via PythonScriptHost.
    pylabhub::processor::ProcessorScriptHost proc_script;
    proc_script.set_config(std::move(config));
    proc_script.set_validate_only(args.validate_only);
    proc_script.set_shutdown_flag(&g_shutdown);

    try
    {
        proc_script.startup_();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Interpreter startup failed: " << e.what() << "\n";
        return 1;
    }

    // ── Check script load result ───────────────────────────────────────────────
    if (!proc_script.script_load_ok())
    {
        std::cerr << "Script load failed.\n";
        proc_script.shutdown_();
        return 1;
    }

    // ── validate mode: print layout and exit ─────────────────────────────────
    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        proc_script.shutdown_();
        return 0;
    }

    // ── Run mode ──────────────────────────────────────────────────────────────
    if (!proc_script.is_running())
    {
        std::cerr << "Failed to start processor — loop did not start.\n";
        proc_script.shutdown_();
        return 1;
    }

    // Register status callback now that config and API are available.
    const auto start_time = std::chrono::steady_clock::now();
    const auto &api = proc_script.api();
    const auto &cfg = proc_script.config();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        return fmt::format(
            "  pyLabHub {} (pylabhub-processor)\n"
            "  Config:      {}\n"
            "  UID:         {}\n"
            "  In channel:  {}\n"
            "  Out channel: {}\n"
            "  Status:      running ({} in, {} out, {} drops, {} errors)\n"
            "  Uptime:      {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            config_dir, cfg.processor_uid, cfg.in_channel, cfg.out_channel,
            api.in_slots_received(), api.out_slots_written(),
            api.out_drop_count(), api.script_error_count(),
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, proc_script, "[proc-main]");

    // ── Tear down ─────────────────────────────────────────────────────────────
    proc_script.shutdown_();
    return 0;
    // runner_lifecycle destructor calls finalize():
    //   → SignalHandler (dynamic persistent) uninstalled first
    //   → Logger, ZMQContext, etc. stopped in reverse init order
}
