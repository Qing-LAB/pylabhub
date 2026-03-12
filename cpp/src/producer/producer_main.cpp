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
 *       "hub_dir": "/var/pylabhub/my_hub",
 *       "channel":          "lab.sensors.temperature",
 *       "target_period_ms": 100,
 *       "shm": { "enabled": true, "secret": 0, "slot_count": 8 },
 *       "slot_schema": { "fields": [{"name": "value", "type": "float32"}] },
 *       "script": { "path": "." }
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

#include "producer_config.hpp"
#include "producer_script_host.hpp"

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
// do_init — create producer directory with producer.json template
// ---------------------------------------------------------------------------

static int do_init(const std::string &prod_dir_str, const std::string &cli_name)
{
    namespace fs = std::filesystem;

    const fs::path prod_dir = prod_dir_str.empty()
                              ? fs::current_path()
                              : fs::path(prod_dir_str);

    try
    {
        RoleDirectory::create(prod_dir, "producer.json");
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    const fs::path json_path = prod_dir / "producer.json";
    if (fs::exists(json_path))
    {
        std::cerr << "Error: producer.json already exists at '" << json_path.string()
                  << "'. Remove it first or choose a different directory.\n";
        return 1;
    }

    const auto name_opt = role_cli::resolve_init_name(
        cli_name, "Producer name (human-readable, e.g. 'TempSensor'): ");
    if (!name_opt)
        return 1;
    const std::string prod_name = *name_opt;

    const std::string prod_uid = pylabhub::uid::generate_producer_uid(prod_name);

    nlohmann::json j;
    j["hub_dir"] = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";

    j["producer"]["uid"]       = prod_uid;
    j["producer"]["name"]      = prod_name;
    j["producer"]["log_level"] = "info";
    j["producer"]["auth"]["keyfile"] = "";

    j["channel"]          = "lab.my.channel";
    j["target_period_ms"] = 100;

    j["shm"]["enabled"]    = true;
    j["shm"]["secret"]     = 0;
    j["shm"]["slot_count"] = 8;

    j["slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["flexzone_schema"] = nullptr;

    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

    j["validation"]["update_checksum"]      = true;
    j["validation"]["stop_on_script_error"] = false;

    std::ofstream out(json_path);
    if (!out)
    {
        std::cerr << "Error: cannot write '" << json_path.string() << "'\n";
        return 1;
    }
    out << j.dump(2) << "\n";
    out.close();

    const fs::path init_py = prod_dir / "script" / "python" / "__init__.py";
    std::ofstream py_out(init_py);
    if (!py_out)
    {
        std::cerr << "Error: cannot write '" << init_py.string() << "'\n";
        return 1;
    }
    py_out <<
        "\"\"\"Producer: " << prod_name << "\n"
        "\n"
        "Script callbacks for the pylabhub-producer host.\n"
        "Edit this file to implement your data-production logic.\n"
        "\n"
        "Directory layout:\n"
        "  script/python/         <- this Python package\n"
        "    __init__.py          <- entry point (this file)\n"
        "\n"
        "Import siblings with relative imports:\n"
        "  from . import helpers  <- loads ./helpers.py\n"
        "\"\"\"\n"
        "import pylabhub_producer as prod\n"
        "\n"
        "\n"
        "def on_init(api: prod.ProducerAPI) -> None:\n"
        "    \"\"\"Called once before the production loop starts.\"\"\"\n"
        "    api.log('info', f\"on_init: uid={api.uid()}\")\n"
        "\n"
        "\n"
        "def on_produce(out_slot, flexzone, messages, api: prod.ProducerAPI) -> bool:\n"
        "    \"\"\"\n"
        "    Called on each production interval.\n"
        "\n"
        "    out_slot: ctypes/numpy writable view of the output SHM slot.\n"
        "    flexzone: persistent ctypes struct for the output flexzone, or None.\n"
        "    messages: list of (sender: str, data: bytes) from ZMQ consumers.\n"
        "    api:      ProducerAPI — log, broadcast, stop, etc.\n"
        "\n"
        "    Return True or None to commit the slot and publish it.\n"
        "    Return False to discard without publishing.\n"
        "    \"\"\"\n"
        "    for sender, data in messages:\n"
        "        api.log('debug', f\"msg from {sender}: {data!r}\")\n"
        "\n"
        "    # TODO: replace with real data production\n"
        "    out_slot.value = 0.0\n"
        "    return True\n"
        "\n"
        "\n"
        "def on_stop(api: prod.ProducerAPI) -> None:\n"
        "    \"\"\"Called once after the production loop exits.\"\"\"\n"
        "    api.log('info', f\"on_stop: uid={api.uid()}\")\n";
    py_out.close();

    std::cout << "\nProducer directory initialised: " << prod_dir.string() << "\n"
              << "  prod_uid  : " << prod_uid << "\n"
              << "  prod_name : " << prod_name << "\n"
              << "  producer.json : " << json_path.string() << "\n"
              << "  script    : " << init_py.string() << "\n\n"
              << "Next steps:\n"
              << "  1. Edit producer.json — set 'hub_dir' to your hub directory path\n"
              << "  2. Edit producer.json — set 'channel'\n"
              << "  3. (Optional) Set 'producer.auth.keyfile', then generate keypair:\n"
              << "       pylabhub-producer --config " << json_path.string() << " --keygen\n"
              << "  4. Edit script/python/__init__.py — implement on_produce\n"
              << "  5. Run: pylabhub-producer " << prod_dir.string() << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::atomic<bool> g_shutdown{false};

    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "pylabhub-producer"}, &g_shutdown);
    signal_handler.install();

    const role_cli::RoleArgs args = role_cli::parse_role_args(argc, argv, "producer");

    if (args.init_only)
        return do_init(args.role_dir, args.init_name);

    pylabhub::producer::ProducerConfig config;
    try
    {
        if (!args.role_dir.empty())
            config = pylabhub::producer::ProducerConfig::from_directory(args.role_dir);
        else
            config = pylabhub::producer::ProducerConfig::from_json_file(args.config_path);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    // Resolve the config directory for status display.
    const std::string config_dir = !args.role_dir.empty()
        ? args.role_dir
        : std::filesystem::path(args.config_path).parent_path().string();

    if (args.keygen_only)
    {
        if (config.auth.keyfile.empty())
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
            const auto vault = pylabhub::utils::ActorVault::create(
                config.auth.keyfile, config.producer_uid, *pw_opt);
            std::cout << "Producer vault written to: " << config.auth.keyfile << "\n"
                      << "  producer_uid : " << config.producer_uid << "\n"
                      << "  public_key   : " << vault.public_key() << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules());
    scripting::register_signal_handler_lifecycle(signal_handler, "[prod-main]");

    if (!config.auth.keyfile.empty())
    {
        const auto vault_password = scripting::get_role_password("producer", "Producer vault password: ");
        if (!vault_password)
            return 1;
        config.auth.load_keypair(config.producer_uid, *vault_password);
    }

    pylabhub::producer::ProducerScriptHost prod_script;
    prod_script.set_config(std::move(config));
    prod_script.set_validate_only(args.validate_only);
    prod_script.set_shutdown_flag(&g_shutdown);

    try
    {
        prod_script.startup_();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Interpreter startup failed: " << e.what() << "\n";
        return 1;
    }

    if (!prod_script.script_load_ok())
    {
        std::cerr << "Script load failed.\n";
        prod_script.shutdown_();
        return 1;
    }

    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        prod_script.shutdown_();
        return 0;
    }

    if (!prod_script.is_running())
    {
        std::cerr << "Failed to start producer — loop did not start.\n";
        prod_script.shutdown_();
        return 1;
    }

    // Register status callback now that config and API are available.
    const auto start_time = std::chrono::steady_clock::now();
    const auto &api = prod_script.api();
    const auto &cfg = prod_script.config();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        return fmt::format(
            "  pyLabHub {} (pylabhub-producer)\n"
            "  Config:    {}\n"
            "  UID:       {}\n"
            "  Channel:   {}\n"
            "  Status:    running ({} slots written, {} drops, {} errors)\n"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            config_dir, cfg.producer_uid, cfg.channel,
            api.out_slots_written(), api.out_drop_count(),
            api.script_error_count(),
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, prod_script, "[prod-main]");
    prod_script.shutdown_();
    return 0;
    // runner_lifecycle destructor calls finalize():
    //   → SignalHandler (dynamic persistent) uninstalled first
    //   → Logger, ZMQContext, etc. stopped in reverse init order
}
