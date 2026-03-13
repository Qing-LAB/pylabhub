/**
 * @file consumer_main.cpp
 * @brief pylabhub-consumer — standalone data-consumer host process.
 *
 * ## Usage
 *
 *     pylabhub-consumer <cons_dir>                  # Run from directory
 *     pylabhub-consumer --config <path.json>        # Run from explicit config file
 *     pylabhub-consumer --config <path.json> --validate   # Validate config; exit 0/1
 *     pylabhub-consumer --config <path.json> --keygen     # Generate consumer keypair; exit 0
 *     pylabhub-consumer --init [<cons_dir>]         # Create consumer directory; exit 0
 *
 * ## Config format
 *
 *     {
 *       "consumer": { "uid": "CONS-LOGGER-12345678", "name": "Logger" },
 *       "hub_dir": "/var/pylabhub/my_hub",
 *       "channel":    "lab.sensors.temperature",
 *       "slot_acquire_timeout_ms": -1,
 *       "shm": { "enabled": true, "secret": 0 },
 *       "slot_schema": { "fields": [{"name": "value", "type": "float32"}] },
 *       "script": { "path": "." }
 *     }
 *
 * ## Python script interface
 *
 *     # script/python/__init__.py
 *     import pylabhub_consumer as cons
 *
 *     def on_init(api: cons.ConsumerAPI) -> None:
 *         api.log('info', f"Consumer {api.uid()} starting")
 *
 *     def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:
 *         if in_slot is None:
 *             return  # timeout
 *         api.log('info', f"value={in_slot.value}")
 *
 *     def on_stop(api: cons.ConsumerAPI) -> None:
 *         api.log('info', "Consumer stopping")
 */

#include "consumer_config.hpp"
#include "consumer_script_host.hpp"

#include "plh_datahub.hpp"
#include "utils/actor_vault.hpp"
#include "utils/role_cli.hpp"
#include "utils/role_directory.hpp"
#include "utils/uid_utils.hpp"
#include "utils/zmq_context.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/timeout_constants.hpp"
#include "role_main_helpers.hpp"

#include <nlohmann/json.hpp>

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
// do_init — create consumer directory with consumer.json template
// ---------------------------------------------------------------------------

static int do_init(const std::string &cons_dir_str, const std::string &cli_name)
{
    namespace fs = std::filesystem;

    const fs::path cons_dir = cons_dir_str.empty()
                              ? fs::current_path()
                              : fs::path(cons_dir_str);

    try
    {
        RoleDirectory::create(cons_dir, "consumer.json");
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    const fs::path json_path = cons_dir / "consumer.json";
    if (fs::exists(json_path))
    {
        std::cerr << "Error: consumer.json already exists at '" << json_path.string()
                  << "'. Remove it first or choose a different directory.\n";
        return 1;
    }

    const auto name_opt = role_cli::resolve_init_name(
        cli_name, "Consumer name (human-readable, e.g. 'Logger'): ");
    if (!name_opt)
        return 1;
    const std::string cons_name = *name_opt;

    const std::string cons_uid = pylabhub::uid::generate_consumer_uid(cons_name);

    nlohmann::json j;
    j["hub_dir"] = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";

    j["consumer"]["uid"]       = cons_uid;
    j["consumer"]["name"]      = cons_name;
    j["consumer"]["log_level"] = "info";
    j["consumer"]["auth"]["keyfile"] = "";

    j["channel"]    = "lab.my.channel";
    j["slot_acquire_timeout_ms"] = -1;

    j["shm"]["enabled"] = true;
    j["shm"]["secret"]  = 0;

    j["slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["flexzone_schema"] = nullptr;

    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

    j["validation"]["stop_on_script_error"] = false;

    std::ofstream out(json_path);
    if (!out)
    {
        std::cerr << "Error: cannot write '" << json_path.string() << "'\n";
        return 1;
    }
    out << j.dump(2) << "\n";
    out.close();

    const fs::path init_py = cons_dir / "script" / "python" / "__init__.py";
    std::ofstream py_out(init_py);
    if (!py_out)
    {
        std::cerr << "Error: cannot write '" << init_py.string() << "'\n";
        return 1;
    }
    py_out <<
        "\"\"\"Consumer: " << cons_name << "\n"
        "\n"
        "Script callbacks for the pylabhub-consumer host.\n"
        "Edit this file to implement your data-consumption logic.\n"
        "\n"
        "Directory layout:\n"
        "  script/python/         <- this Python package\n"
        "    __init__.py          <- entry point (this file)\n"
        "\n"
        "Import siblings with relative imports:\n"
        "  from . import helpers  <- loads ./helpers.py\n"
        "\"\"\"\n"
        "import pylabhub_consumer as cons\n"
        "\n"
        "\n"
        "def on_init(api: cons.ConsumerAPI) -> None:\n"
        "    \"\"\"Called once before the consumption loop starts.\"\"\"\n"
        "    api.log('info', f\"on_init: uid={api.uid()}\")\n"
        "\n"
        "\n"
        "def on_consume(in_slot, flexzone, messages, api: cons.ConsumerAPI) -> None:\n"
        "    \"\"\"\n"
        "    Called on each incoming slot (or timeout if slot_acquire_timeout_ms > 0).\n"
        "\n"
        "    in_slot:  ctypes/numpy read-only copy of the input SHM slot,\n"
        "              or None on timeout.\n"
        "    flexzone: read-only ctypes/numpy view of the input flexzone, or None.\n"
        "    messages: list of bytes from ZMQ data channel.\n"
        "    api:      ConsumerAPI — log, stop, etc.\n"
        "    \"\"\"\n"
        "    if in_slot is None:\n"
        "        return  # timeout — no new data\n"
        "\n"
        "    for data in messages:\n"
        "        api.log('debug', f\"zmq data: {data!r}\")\n"
        "\n"
        "    # TODO: replace with real data-consumption logic\n"
        "    api.log('debug', f\"received slot #{api.in_slots_received()}\")\n"
        "\n"
        "\n"
        "def on_stop(api: cons.ConsumerAPI) -> None:\n"
        "    \"\"\"Called once after the consumption loop exits.\"\"\"\n"
        "    api.log('info', f\"on_stop: uid={api.uid()}\")\n";
    py_out.close();

    std::cout << "\nConsumer directory initialised: " << cons_dir.string() << "\n"
              << "  cons_uid  : " << cons_uid << "\n"
              << "  cons_name : " << cons_name << "\n"
              << "  consumer.json : " << json_path.string() << "\n"
              << "  script    : " << init_py.string() << "\n\n"
              << "Next steps:\n"
              << "  1. Edit consumer.json — set 'hub_dir' to your hub directory path\n"
              << "  2. Edit consumer.json — set 'channel'\n"
              << "  3. (Optional) Set 'consumer.auth.keyfile', then generate keypair:\n"
              << "       pylabhub-consumer --config " << json_path.string() << " --keygen\n"
              << "  4. Edit script/python/__init__.py — implement on_consume\n"
              << "  5. Run: pylabhub-consumer " << cons_dir.string() << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::atomic<bool> g_shutdown{false};

    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "pylabhub-consumer"}, &g_shutdown);
    signal_handler.install();

    const role_cli::RoleArgs args = role_cli::parse_role_args(argc, argv, "consumer");

    if (args.init_only)
        return do_init(args.role_dir, args.init_name);

    pylabhub::consumer::ConsumerConfig config;
    try
    {
        if (!args.role_dir.empty())
            config = pylabhub::consumer::ConsumerConfig::from_directory(args.role_dir);
        else
            config = pylabhub::consumer::ConsumerConfig::from_json_file(args.config_path);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    if (args.keygen_only)
    {
        if (config.auth.keyfile.empty())
        {
            std::cerr << "Error: --keygen requires 'consumer.auth.keyfile' in config\n";
            return 1;
        }

        const auto pw_opt = role_cli::get_new_role_password(
            "consumer",
            "Consumer vault password (empty = no encryption): ",
            "Confirm password: ");
        if (!pw_opt)
            return 1;

        try
        {
            const auto vault = pylabhub::utils::ActorVault::create(
                config.auth.keyfile, config.consumer_uid, *pw_opt);
            std::cout << "Consumer vault written to: " << config.auth.keyfile << "\n"
                      << "  consumer_uid : " << config.consumer_uid << "\n"
                      << "  public_key   : " << vault.public_key() << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // Resolve the config directory for status display.
    const std::string config_dir = !args.role_dir.empty()
        ? args.role_dir
        : std::filesystem::path(args.config_path).parent_path().string();

    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules(args.log_file));
    scripting::register_signal_handler_lifecycle(signal_handler, "[cons-main]");

    if (!config.auth.keyfile.empty())
    {
        const auto vault_password = scripting::get_role_password("consumer", "Consumer vault password: ");
        if (!vault_password)
            return 1;
        config.auth.load_keypair(config.consumer_uid, *vault_password);
    }

    pylabhub::consumer::ConsumerScriptHost cons_script;
    cons_script.set_config(std::move(config));
    cons_script.set_validate_only(args.validate_only);
    cons_script.set_shutdown_flag(&g_shutdown);

    try
    {
        cons_script.startup_();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Interpreter startup failed: " << e.what() << "\n";
        return 1;
    }

    if (!cons_script.script_load_ok())
    {
        std::cerr << "Script load failed.\n";
        cons_script.shutdown_();
        return 1;
    }

    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        cons_script.shutdown_();
        return 0;
    }

    if (!cons_script.is_running())
    {
        std::cerr << "Failed to start consumer — loop did not start.\n";
        cons_script.shutdown_();
        return 1;
    }

    // Register status callback now that config and API are available.
    const auto start_time = std::chrono::steady_clock::now();
    const auto &api = cons_script.api();
    const auto &cfg = cons_script.config();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        return fmt::format(
            "  pyLabHub {} (pylabhub-consumer)\n"
            "  Config:    {}\n"
            "  UID:       {}\n"
            "  Channel:   {}\n"
            "  Status:    running ({} slots received, {} errors)\n"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            config_dir, cfg.consumer_uid, cfg.channel,
            api.in_slots_received(), api.script_error_count(),
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, cons_script, "[cons-main]");
    cons_script.shutdown_();
    return 0;
    // runner_lifecycle destructor calls finalize():
    //   → SignalHandler (dynamic persistent) uninstalled first
    //   → Logger, ZMQContext, etc. stopped in reverse init order
}
