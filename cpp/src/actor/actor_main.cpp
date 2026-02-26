/**
 * @file actor_main.cpp
 * @brief pylabhub-actor — multi-role scripted actor host process.
 *
 * ## Usage
 *
 *     pylabhub-actor <actor_dir>                       # Run from directory (Phase 2)
 *     pylabhub-actor --config <path.json>              # Run (legacy flat-config)
 *     pylabhub-actor --config <path.json> --validate   # Validate + print layout; exit 0/1
 *     pylabhub-actor --config <path.json> --list-roles # Show role activation summary; exit 0
 *     pylabhub-actor --config <path.json> --keygen     # Generate actor keypair; exit 0
 *     pylabhub-actor --config <path.json> --run        # Explicit run mode
 *
 * When <actor_dir> is given, actor.json is read from <actor_dir>/actor.json.
 * If actor.json contains a "hub_dir" key, broker endpoint and pubkey are
 * loaded from <hub_dir>/hub.json and <hub_dir>/hub.pubkey, overriding all roles.
 *
 * ## Config format
 *
 *     {
 *       "actor": { "uid": "ACTOR-Sensor-12345678", "name": "TempSensor", "log_level": "info" },
 *       "script": {"module": "sensor_node", "path": "/opt/scripts"},
 *       "roles": {
 *         "raw_out": {
 *           "kind": "producer",
 *           "channel": "lab.sensor.temperature",
 *           "broker": "tcp://127.0.0.1:5570",
 *           "interval_ms": 100,
 *           "loop_trigger": "shm",
 *           "slot_schema": { "fields": [{"name": "ts", "type": "float64"},
 *                                       {"name": "value", "type": "float32"}] },
 *           "shm": { "enabled": true, "slot_count": 8, "secret": 0 }
 *         },
 *         "cfg_in": {
 *           "kind": "consumer",
 *           "channel": "lab.config.setpoints",
 *           "broker": "tcp://127.0.0.1:5570",
 *           "timeout_ms": 5000,
 *           "loop_trigger": "messenger",
 *           "slot_schema": { "fields": [{"name": "setpoint", "type": "float32"}] }
 *         }
 *       }
 *     }
 *
 * ## Python script interface (module convention)
 *
 *     # sensor_node.py
 *     import pylabhub_actor as actor
 *
 *     def on_init(api: actor.ActorRoleAPI):
 *         api.log('info', "starting")
 *
 *     def on_iteration(slot, flexzone, messages, api: actor.ActorRoleAPI) -> bool:
 *         # slot      — ctypes struct into SHM slot, or None (Messenger / timeout)
 *         # flexzone  — persistent ctypes struct for this role's flexzone, or None
 *         # messages  — list of (sender: str, data: bytes) from the incoming queue
 *         # api       — ActorRoleAPI proxy for this role
 *         if slot is not None:
 *             slot.ts = ...
 *         for sender, data in messages:
 *             api.broadcast(data)
 *         return True  # True/None=commit (producer); consumer return ignored
 *
 *     def on_stop(api: actor.ActorRoleAPI):
 *         api.log('info', "stopping")
 */

#include "actor_config.hpp"
#include "actor_host.hpp"

#include "plh_datahub.hpp"
#include "utils/uid_utils.hpp"
#include "utils/zmq_context.hpp"

#include <nlohmann/json.hpp>
#include <pybind11/embed.h>
#include <zmq.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace py = pybind11;
using namespace pylabhub::utils;

// ---------------------------------------------------------------------------
// Global shutdown flag (set by SIGINT/SIGTERM)
// ---------------------------------------------------------------------------

static std::atomic<bool>           g_shutdown{false};
static pylabhub::actor::ActorHost *g_host_ptr{nullptr};

static void signal_handler(int /*sig*/) noexcept
{
    if (g_shutdown.load(std::memory_order_relaxed))
        std::_Exit(1); // double signal — fast exit
    g_shutdown.store(true, std::memory_order_relaxed);
    if (g_host_ptr != nullptr)
        g_host_ptr->signal_shutdown();
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

namespace
{

struct ActorArgs
{
    std::string config_path;     ///< Path to actor JSON file (--config mode)
    std::string actor_dir;       ///< Actor directory (positional-arg mode or --init target)
    std::string register_hub_dir; ///< --register-with <hub_dir> target
    bool        validate_only{false};
    bool        list_roles{false};
    bool        keygen_only{false};
    bool        init_only{false};
    bool        register_only{false};
};

void print_usage(const char *prog)
{
    std::cout
        << "Usage:\n"
        << "  " << prog << " --init [<actor_dir>]                      # Create actor directory\n"
        << "  " << prog << " --register-with <hub_dir> [<actor_dir>]   # Add actor to hub's known_actors\n"
        << "  " << prog << " <actor_dir>                               # Run from directory\n"
        << "  " << prog << " --config <path.json> [--validate | --list-roles | --keygen | --run]\n\n"
        << "Options:\n"
        << "  --init [dir]               Create actor directory with actor.json template; exit 0\n"
        << "  --register-with <hub_dir>  Append this actor to <hub_dir>/hub.json known_actors; exit 0\n"
        << "  <actor_dir>                Actor directory containing actor.json\n"
        << "  --config <path>            Path to actor JSON config (legacy flat-config mode)\n"
        << "  --validate                 Validate script and print layout; exit 0 on success\n"
        << "  --list-roles               Show configured roles and activation status; exit 0\n"
        << "  --keygen                   Generate actor NaCl keypair at auth.keyfile path; exit 0\n"
        << "  --run                      Explicit run mode (default when no other mode given)\n"
        << "  --help                     Show this message\n";
}

ActorArgs parse_args(int argc, char *argv[])
{
    ActorArgs args;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (arg == "--config" && i + 1 < argc)
        {
            args.config_path = argv[++i];
        }
        else if (arg == "--register-with" && i + 1 < argc)
        {
            args.register_hub_dir = argv[++i];
            args.register_only = true;
            // Optional positional actor_dir after hub_dir.
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                args.actor_dir = argv[++i];
            }
        }
        else if (arg == "--init")
        {
            args.init_only = true;
            // Optional positional actor_dir after --init.
            if (i + 1 < argc && argv[i + 1][0] != '-')
                args.actor_dir = argv[++i];
        }
        else if (arg == "--validate")
        {
            args.validate_only = true;
        }
        else if (arg == "--list-roles")
        {
            args.list_roles = true;
        }
        else if (arg == "--keygen")
        {
            args.keygen_only = true;
        }
        else if (arg == "--run")
        {
            // explicit run — default; no flag needed
        }
        else if (arg[0] != '-')
        {
            // Positional argument: actor directory.
            if (!args.actor_dir.empty())
            {
                std::cerr << "Error: multiple positional arguments not supported\n\n";
                print_usage(argv[0]);
                std::exit(1);
            }
            args.actor_dir = std::string(arg);
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    if (!args.init_only && !args.register_only &&
        args.config_path.empty() && args.actor_dir.empty())
    {
        std::cerr << "Error: specify an actor directory, --init, --register-with, "
                     "or --config <path>\n\n";
        print_usage(argv[0]);
        std::exit(1);
    }
    return args;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// do_init — create actor directory with actor.json template
// ---------------------------------------------------------------------------

static int do_init(const std::string &actor_dir_str)
{
    namespace fs = std::filesystem;

    // ── Resolve directory ─────────────────────────────────────────────────────
    const fs::path actor_dir = actor_dir_str.empty()
                               ? fs::current_path()
                               : fs::path(actor_dir_str);

    std::error_code ec;
    fs::create_directories(actor_dir / "logs", ec);
    fs::create_directories(actor_dir / "run",  ec);
    // Create the standard role script package directory.
    const fs::path role_script_dir = actor_dir / "roles" / "data_out" / "script";
    fs::create_directories(role_script_dir, ec);
    if (ec)
    {
        std::cerr << "Error: cannot create directory '" << actor_dir.string()
                  << "': " << ec.message() << "\n";
        return 1;
    }

    // ── Check for existing actor.json ─────────────────────────────────────────
    const fs::path json_path = actor_dir / "actor.json";
    if (fs::exists(json_path))
    {
        std::cerr << "Error: actor.json already exists at '" << json_path.string()
                  << "'. Remove it first or choose a different directory.\n";
        return 1;
    }

    // ── Prompt for actor name ─────────────────────────────────────────────────
    std::string actor_name;
    std::cout << "Actor name (human-readable, e.g. 'TemperatureSensor'): ";
    std::getline(std::cin, actor_name);

    // ── Generate actor UID ────────────────────────────────────────────────────
    const std::string actor_uid = pylabhub::uid::generate_actor_uid(actor_name);

    // ── Build actor.json template ─────────────────────────────────────────────
    nlohmann::json j;
    // hub_dir: left as a placeholder — set by the user after --init
    j["hub_dir"] = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";

    j["actor"]["uid"]       = actor_uid;
    j["actor"]["name"]      = actor_name;
    j["actor"]["log_level"] = "info";

    // Minimal example producer role — per-role script points to ./roles/data_out/script/
    nlohmann::json &role = j["roles"]["data_out"];
    role["kind"]            = "producer";
    role["channel"]         = "lab.data.channel";
    role["interval_ms"]     = 100;
    role["loop_trigger"]    = "shm";
    // Per-role script: module="script" is the package at ./roles/data_out/script/__init__.py
    role["script"]["module"] = "script";
    role["script"]["path"]   = "./roles/data_out";
    role["shm"]["enabled"]    = true;
    role["shm"]["slot_count"] = 8;
    role["shm"]["secret"]     = 0;
    role["slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "timestamp"}, {"type", "float64"}},
        nlohmann::json{{"name", "value"},     {"type", "float32"}}
    });

    // ── Write actor.json ──────────────────────────────────────────────────────
    std::ofstream out(json_path);
    if (!out)
    {
        std::cerr << "Error: cannot write '" << json_path.string() << "'\n";
        return 1;
    }
    out << j.dump(2) << "\n";
    out.close();

    // ── Write roles/data_out/script/__init__.py template ─────────────────────
    const fs::path init_py = role_script_dir / "__init__.py";
    std::ofstream py_out(init_py);
    if (!py_out)
    {
        std::cerr << "Error: cannot write '" << init_py.string() << "'\n";
        return 1;
    }
    py_out <<
        "\"\"\"Role: data_out\n"
        "Actor: " << actor_name << "\n"
        "\n"
        "Script callbacks for the 'data_out' producer role.\n"
        "Edit this file to implement your data-production logic.\n"
        "\n"
        "Directory layout:\n"
        "  roles/data_out/script/          <- this Python package\n"
        "    __init__.py                   <- entry point (this file)\n"
        "    helpers.py                    <- optional helper module\n"
        "\n"
        "Import siblings with relative imports:\n"
        "  from . import helpers           <- loads ./helpers.py\n"
        "  from .helpers import my_func    <- import a specific name\n"
        "\"\"\"\n"
        "import pylabhub_actor as actor\n"
        "\n"
        "\n"
        "def on_init(api: actor.ActorRoleAPI) -> None:\n"
        "    \"\"\"Called once before the loop starts.\"\"\"\n"
        "    api.log('info', f\"on_init: role={api.role_name()} uid={api.uid()}\")\n"
        "\n"
        "\n"
        "def on_iteration(slot, flexzone, messages, api: actor.ActorRoleAPI) -> bool:\n"
        "    \"\"\"\n"
        "    Called every loop iteration.\n"
        "\n"
        "    slot:     ctypes.LittleEndianStructure (writable fields), or None\n"
        "              when triggered by Messenger timeout / no SHM slot available.\n"
        "    flexzone: persistent ctypes struct for this role's flexzone, or None.\n"
        "    messages: list of (sender: str, data: bytes) from the ZMQ incoming queue.\n"
        "    api:      ActorRoleAPI — log, send, broadcast, stop the actor, etc.\n"
        "\n"
        "    Return True or None to commit the slot (producer).\n"
        "    Return False to discard without publishing.\n"
        "    Consumer return value is ignored.\n"
        "    \"\"\"\n"
        "    import time\n"
        "\n"
        "    for sender, data in messages:\n"
        "        api.log('debug', f\"msg from {sender}: {data!r}\")\n"
        "\n"
        "    if slot is None:\n"
        "        return None  # Messenger trigger or timeout — no SHM slot\n"
        "\n"
        "    slot.timestamp = time.time()\n"
        "    slot.value     = 0.0   # TODO: replace with real data\n"
        "    return True\n"
        "\n"
        "\n"
        "def on_stop(api: actor.ActorRoleAPI) -> None:\n"
        "    \"\"\"Called once after the loop exits.\"\"\"\n"
        "    api.log('info', f\"on_stop: role={api.role_name()}\")\n";
    py_out.close();

    // ── Summary ───────────────────────────────────────────────────────────────
    std::cout << "\nActor directory initialised: " << actor_dir.string() << "\n"
              << "  actor_uid  : " << actor_uid << "\n"
              << "  actor_name : " << actor_name << "\n"
              << "  actor.json : " << json_path.string() << "\n"
              << "  script     : " << init_py.string() << "\n\n"
              << "Next steps:\n"
              << "  1. Edit actor.json — set 'hub_dir' to your hub directory path\n"
              << "  2. Edit actor.json — define your roles and their slot_schema\n"
              << "  3. Edit roles/data_out/script/__init__.py — implement on_iteration\n"
              << "     Add helpers.py (or other submodules) beside __init__.py as needed.\n"
              << "  4. Run: pylabhub-actor " << actor_dir.string() << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// do_register_with — append actor to hub's known_actors
// ---------------------------------------------------------------------------

static int do_register_with(const std::string& hub_dir_str, const std::string& actor_dir_str)
{
    namespace fs = std::filesystem;

    const fs::path actor_dir  = actor_dir_str.empty() ? fs::current_path()
                                                       : fs::path(actor_dir_str);
    const fs::path hub_dir    = fs::path(hub_dir_str);
    const fs::path actor_json = actor_dir / "actor.json";
    const fs::path hub_json   = hub_dir / "hub.json";

    if (!fs::exists(actor_json))
    {
        std::cerr << "Error: actor.json not found at '" << actor_json.string()
                  << "'. Run --init first.\n";
        return 1;
    }
    if (!fs::exists(hub_json))
    {
        std::cerr << "Error: hub.json not found at '" << hub_json.string() << "'.\n";
        return 1;
    }

    // Read actor identity.
    std::string actor_name;
    std::string actor_uid;
    try
    {
        std::ifstream f(actor_json);
        const auto j = nlohmann::json::parse(f);
        actor_name = j.at("actor").at("name").get<std::string>();
        actor_uid  = j.at("actor").at("uid").get<std::string>();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error reading actor.json: " << e.what() << "\n";
        return 1;
    }

    // Read, update, and write hub.json.
    nlohmann::json hub;
    try
    {
        std::ifstream f(hub_json);
        hub = nlohmann::json::parse(f);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error reading hub.json: " << e.what() << "\n";
        return 1;
    }

    if (!hub.contains("hub"))
    {
        hub["hub"] = nlohmann::json::object();
    }
    if (!hub["hub"].contains("known_actors") || !hub["hub"]["known_actors"].is_array())
    {
        hub["hub"]["known_actors"] = nlohmann::json::array();
    }

    // Check for duplicate entry.
    for (const auto& a : hub["hub"]["known_actors"])
    {
        if (a.value("uid", "") == actor_uid)
        {
            std::cerr << "Actor uid '" << actor_uid << "' already in known_actors.\n";
            return 0; // idempotent
        }
    }

    nlohmann::json entry;
    entry["name"] = actor_name;
    entry["uid"]  = actor_uid;
    entry["role"] = "any";
    hub["hub"]["known_actors"].push_back(std::move(entry));

    try
    {
        std::ofstream out(hub_json);
        if (!out)
        {
            std::cerr << "Error: cannot write hub.json at '" << hub_json.string() << "'.\n";
            return 1;
        }
        out << hub.dump(2) << "\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error writing hub.json: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Registered actor in '" << hub_json.string() << "':\n"
              << "  name: " << actor_name << "\n"
              << "  uid:  " << actor_uid  << "\n"
              << "  role: any\n\n"
              << "To verify, update hub.json 'connection_policy' to 'verified'.\n";
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Parse arguments ───────────────────────────────────────────────────────
    const ActorArgs args = parse_args(argc, argv);

    // ── Init mode: create actor directory and exit ────────────────────────────
    if (args.init_only)
    {
        return do_init(args.actor_dir);
    }

    // ── Register-with mode: append actor to hub known_actors and exit ─────────
    if (args.register_only)
    {
        return do_register_with(args.register_hub_dir, args.actor_dir);
    }


    // ── Load config ───────────────────────────────────────────────────────────
    pylabhub::actor::ActorConfig config;
    try
    {
        if (!args.actor_dir.empty())
            config = pylabhub::actor::ActorConfig::from_directory(args.actor_dir);
        else
            config = pylabhub::actor::ActorConfig::from_json_file(args.config_path);
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
            std::cerr << "Error: --keygen requires 'actor.auth.keyfile' in config\n";
            return 1;
        }

        // Generate Z85-encoded CURVE25519 keypair via libsodium CSPRNG.
        // zmq_curve_keypair() is hardware-seeded; no interactive entropy needed.
        std::array<char, 41> z85_pub{};
        std::array<char, 41> z85_sec{};
        if (zmq_curve_keypair(z85_pub.data(), z85_sec.data()) != 0)
        {
            std::cerr << "Error: zmq_curve_keypair() failed — libsodium not available?\n";
            return 1;
        }

        // Ensure parent directory exists.
        std::filesystem::path keypath(config.auth.keyfile);
        if (keypath.has_parent_path())
        {
            std::error_code ec;
            std::filesystem::create_directories(keypath.parent_path(), ec);
            if (ec)
            {
                std::cerr << "Error: cannot create directory '"
                          << keypath.parent_path().string() << "': " << ec.message() << "\n";
                return 1;
            }
        }

        // Write JSON keypair file.
        nlohmann::json kf;
        kf["actor_uid"]  = config.actor_uid;
        kf["public_key"] = std::string(z85_pub.data(), 40);
        kf["secret_key"] = std::string(z85_sec.data(), 40);
        kf["_note"]      = "Z85 CURVE keypair generated by pylabhub-actor --keygen. "
                           "Keep secret_key private.";

        std::ofstream out(keypath);
        if (!out)
        {
            std::cerr << "Error: cannot write keypair to '" << config.auth.keyfile << "'\n";
            return 1;
        }
        out << kf.dump(2) << "\n";
        out.close();

        std::cout << "Actor keypair written to: " << config.auth.keyfile << "\n"
                  << "  actor_uid : " << config.actor_uid << "\n"
                  << "  public_key: " << std::string(z85_pub.data(), 40) << "\n"
                  << "  (secret_key stored in file — keep private)\n";
        return 0;
    }

    // ── Lifecycle guard ───────────────────────────────────────────────────────
    LifecycleGuard runner_lifecycle(MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule()
    ));

    // ── Python interpreter ────────────────────────────────────────────────────
    py::scoped_interpreter python_guard{};

    // ── Load actor keypair (if keyfile configured) ────────────────────────────
    // Populates config.auth.client_pubkey / client_seckey for CurveZMQ client auth.
    // No-op when auth.keyfile is empty; logs warning and continues on failure.
    if (!config.auth.keyfile.empty())
        config.auth.load_keypair();

    // ── Create actor host ─────────────────────────────────────────────────────
    // Each role worker owns its own Messenger and connects to role.broker in start().
    pylabhub::actor::ActorHost host(config);
    g_host_ptr = &host;

    // Load script: imports Python module, looks up on_iteration/on_init/on_stop
    const bool verbose = args.validate_only || args.list_roles;
    if (!host.load_script(verbose))
    {
        std::cerr << "Script load failed.\n";
        return 1;
    }

    // ── list-roles mode: show activation summary and exit ─────────────────────
    if (args.list_roles)
    {
        // Summary already printed by load_script(verbose=true).
        return 0;
    }

    // ── validate mode: print layout and exit ─────────────────────────────────
    if (args.validate_only)
    {
        std::cout << "\nValidation passed.\n";
        return 0;
    }

    // ── Run mode ──────────────────────────────────────────────────────────────
    if (!host.start())
    {
        std::cerr << "Failed to start actor — no roles activated.\n";
        return 1;
    }

    host.wait_for_shutdown();
    host.stop();

    g_host_ptr = nullptr;
    return 0;
}
