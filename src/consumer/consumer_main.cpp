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
 *       "in_hub_dir":         "/var/pylabhub/my_hub",
 *       "in_channel":         "lab.sensors.temperature",
 *       "in_transport":       "shm",
 *       "checksum": "enforced",
 *       "script": { "type": "python", "path": "." }
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

#include "consumer_role_host.hpp"
#include "consumer_fields.hpp"
#include "lua_engine.hpp"
#include "utils/native_engine.hpp"
#include "python_engine.hpp"

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
        RoleDirectory::create(cons_dir);
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

    j["consumer"]["uid"]       = cons_uid;
    j["consumer"]["name"]      = cons_name;
    j["consumer"]["log_level"] = "info";
    j["consumer"]["auth"]["keyfile"] = "";

    j["loop_timing"]         = "max_rate";
    j["in_hub_dir"]          = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";
    j["in_channel"]          = "lab.my.channel";
    j["in_transport"]        = "shm";
    j["in_shm_enabled"]      = true;
    j["checksum"]            = "enforced";

    j["stop_on_script_error"] = false;

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
        "    Called on each incoming slot (or None on timeout).\n"
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

    // ── Lifecycle init (required before JsonConfig/RoleConfig) ────────────────
    LifecycleGuard runner_lifecycle(scripting::role_lifecycle_modules(args.log_file));
    scripting::register_signal_handler_lifecycle(signal_handler, "[cons-main]");
    scripting::log_version_info("[cons-main]");

    // ── Load config via RoleConfig ───────────────────────────────────────────
    std::optional<pylabhub::config::RoleConfig> config;
    try
    {
        if (!args.role_dir.empty())
            config.emplace(pylabhub::config::RoleConfig::load_from_directory(
                args.role_dir, "consumer", pylabhub::consumer::parse_consumer_fields));
        else
            config.emplace(pylabhub::config::RoleConfig::load(
                args.config_path, "consumer", pylabhub::consumer::parse_consumer_fields));
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
            const auto pubkey = c.create_keypair(*pw_opt);
            std::cout << "Consumer vault written to: " << c.auth().keyfile << "\n"
                      << "  consumer_uid : " << c.identity().uid << "\n"
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
        const auto vault_password = scripting::get_role_password("consumer", "Consumer vault password: ");
        if (!vault_password)
            return 1;
        c.load_keypair(*vault_password);
    }

    // ── Create engine based on script type ───────────────────────────────────
    std::unique_ptr<pylabhub::scripting::ScriptEngine> engine;
    const auto &script_type = c.script().type;

    if (script_type == "native")
    {
        auto ne = std::make_unique<pylabhub::scripting::NativeEngine>();
        if (!c.script().checksum.empty())
            ne->set_expected_checksum(c.script().checksum);
        engine = std::move(ne);
    }
    else if (script_type == "lua")
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
    pylabhub::consumer::ConsumerRoleHost host(
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
        std::cerr << "Failed to start consumer — loop did not start.\n";
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
            "  pyLabHub {} (pylabhub-consumer, {})\n"
            "  Config:    {}\n"
            "  UID:       {}\n"
            "  Channel:   {}\n"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            cfg.script().type,
            config_dir, cfg.identity().uid, cfg.in_channel(),
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    scripting::run_role_main_loop(g_shutdown, host, "[cons-main]");
    host.shutdown_();
    return 0;
}
