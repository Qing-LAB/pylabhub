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
 *       "out_update_checksum": true,
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

#include "producer_role_host.hpp"
#include "producer_fields.hpp"
#include "lua_engine.hpp"
#include "python_engine.hpp"

#include "plh_datahub.hpp"
#include "utils/config/role_config.hpp"
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
        RoleDirectory::create(prod_dir);
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

    j["producer"]["uid"]       = prod_uid;
    j["producer"]["name"]      = prod_name;
    j["producer"]["log_level"] = "info";
    j["producer"]["auth"]["keyfile"] = "";

    j["out_hub_dir"]          = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";
    j["out_channel"]          = "lab.my.channel";
    j["target_period_ms"]     = 100;

    j["out_transport"]        = "shm";
    j["out_shm_enabled"]      = true;
    j["out_shm_secret"]       = 0;
    j["out_shm_slot_count"]   = 8;

    j["out_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_flexzone_schema"]  = nullptr;

    j["out_update_checksum"]      = true;
    j["stop_on_script_error"]     = false;

    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

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
        "    Return True to commit the slot and publish it.\n"
        "    Return False to discard without publishing.\n"
        "    Return None (or omit return) is treated as an error.\n"
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

    // ── Lifecycle init (required before JsonConfig/RoleConfig) ────────────────
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules(args.log_file));
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
    std::unique_ptr<pylabhub::scripting::ScriptEngine> engine;
    const auto &script_type = c.script().type;

    if (script_type == "lua")
    {
        engine = std::make_unique<pylabhub::scripting::LuaEngine>();
    }
    else
    {
        auto py = std::make_unique<pylabhub::scripting::PythonEngine>();
        if (!c.script().python_venv.empty())
            py->set_python_venv(c.script().python_venv);
        engine = std::move(py);
    }

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
