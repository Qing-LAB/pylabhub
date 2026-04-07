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

#include "processor_role_host.hpp"
#include "processor_fields.hpp"
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

    j["processor"]["uid"]       = proc_uid;
    j["processor"]["name"]      = proc_name;
    j["processor"]["log_level"] = "info";
    j["processor"]["auth"]["keyfile"] = "";

    j["loop_timing"]  = "max_rate";
    j["in_hub_dir"]   = "<replace with input hub directory path>";
    j["out_hub_dir"]  = "<replace with output hub directory path>";
    j["in_channel"]   = "lab.source.channel";
    j["out_channel"]  = "lab.output.channel";

    j["in_transport"]       = "shm";
    j["out_transport"]      = "shm";
    j["in_shm_enabled"]     = true;
    j["out_shm_enabled"]    = true;
    j["out_shm_slot_count"] = 4;

    j["in_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["out_flexzone_schema"] = nullptr;

    j["checksum"]             = "enforced";
    j["flexzone_checksum"]    = true;
    j["stop_on_script_error"] = false;

    j["script"]["path"] = ".";
    j["script"]["type"] = "python";

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
        "    Return True to commit the output slot.\n"
        "    Return False to discard without publishing.\n"
        "    Return None (or omit return) is treated as an error.\n"
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
