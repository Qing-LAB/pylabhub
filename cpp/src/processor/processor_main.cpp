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
 *       "timeout_ms":  5000,
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

#include "plh_datahub.hpp"
#include "utils/actor_vault.hpp"
#include "utils/uid_utils.hpp"
#include "utils/zmq_context.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/timeout_constants.hpp"

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

#if defined(PYLABHUB_IS_POSIX)
#include <unistd.h> // isatty, STDIN_FILENO
#endif

using namespace pylabhub::utils;

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

namespace
{

struct ProcArgs
{
    std::string config_path;  ///< Explicit JSON file path (--config mode)
    std::string proc_dir;     ///< Processor directory (positional or --init target)
    std::string init_name;    ///< --name for --init (skips stdin prompt)
    bool        validate_only{false};
    bool        keygen_only{false};
    bool        init_only{false};
};

void print_usage(const char *prog)
{
    std::cout
        << "Usage:\n"
        << "  " << prog << " --init [<proc_dir>]              # Create processor directory\n"
        << "  " << prog << " <proc_dir>                        # Run from directory\n"
        << "  " << prog << " --config <path.json> [--validate | --keygen | --run]\n\n"
        << "Options:\n"
        << "  --init [dir]    Create processor directory with processor.json template; exit 0\n"
        << "  --name <name>   Processor name for --init (skips interactive prompt)\n"
        << "  <proc_dir>      Processor directory containing processor.json\n"
        << "  --config <path> Path to processor JSON config file\n"
        << "  --validate      Validate config + script, print schema layout; exit 0 on success\n"
        << "  --keygen        Generate processor NaCl keypair at auth.keyfile path; exit 0\n"
        << "  --run           Explicit run mode (default when no other mode given)\n"
        << "  --help          Show this message\n";
}

ProcArgs parse_args(int argc, char *argv[])
{
    ProcArgs args;
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
        else if (arg == "--init")
        {
            args.init_only = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                args.proc_dir = argv[++i];
        }
        else if (arg == "--name" && i + 1 < argc)
        {
            args.init_name = argv[++i];
        }
        else if (arg == "--validate")
        {
            args.validate_only = true;
        }
        else if (arg == "--keygen")
        {
            args.keygen_only = true;
        }
        else if (arg == "--run")
        {
            // explicit run — default; no-op
        }
        else if (arg[0] != '-')
        {
            if (!args.proc_dir.empty())
            {
                std::cerr << "Error: multiple positional arguments not supported\n\n";
                print_usage(argv[0]);
                std::exit(1);
            }
            args.proc_dir = std::string(arg);
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    if (!args.init_only && args.config_path.empty() && args.proc_dir.empty())
    {
        std::cerr << "Error: specify a processor directory, --init, or --config <path>\n\n";
        print_usage(argv[0]);
        std::exit(1);
    }
    return args;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Password helpers (vault unlock)
// ---------------------------------------------------------------------------

static std::string read_password_interactive(const char *prompt)
{
#if defined(PYLABHUB_IS_POSIX)
    char *pw = ::getpass(prompt);
    if (!pw)
    {
        std::fprintf(stderr, "processor: failed to read password from terminal\n");
        return {};
    }
    return pw;
#else
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string pw;
    std::getline(std::cin, pw);
    return pw;
#endif
}

// getenv is called once at startup — not in a hot loop, so no caching needed.
// XPLAT: std::getenv is portable (C++11 and POSIX/Windows).
static std::string get_processor_password(const char *prompt)
{
    if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD"))
        return env;
    return read_password_interactive(prompt);
}

// ---------------------------------------------------------------------------
// do_init — create processor directory with processor.json template
// ---------------------------------------------------------------------------

static int do_init(const std::string &proc_dir_str, const std::string &cli_name)
{
    namespace fs = std::filesystem;

    const fs::path proc_dir = proc_dir_str.empty()
                              ? fs::current_path()
                              : fs::path(proc_dir_str);

    std::error_code ec;
    fs::create_directories(proc_dir / "logs",              ec);
    fs::create_directories(proc_dir / "run",               ec);
    fs::create_directories(proc_dir / "vault",             ec);
    fs::create_directories(proc_dir / "script" / "python", ec);
    if (ec)
    {
        std::cerr << "Error: cannot create directory '" << proc_dir.string()
                  << "': " << ec.message() << "\n";
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
    std::string proc_name;
    if (!cli_name.empty())
    {
        proc_name = cli_name;
    }
#if defined(PYLABHUB_IS_POSIX)
    else if (::isatty(STDIN_FILENO))
    {
        std::cout << "Processor name (human-readable, e.g. 'Doubler'): ";
        std::getline(std::cin, proc_name);
    }
    else
    {
        proc_name = "Processor";
    }
#else
    else
    {
        std::cout << "Processor name (human-readable, e.g. 'Doubler'): ";
        std::getline(std::cin, proc_name);
    }
#endif

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
    j["timeout_ms"]  = 5000;
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
    // "." means <proc_dir>/script/__init__.py (the default layout).
    j["script"]["path"] = ".";

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
    const ProcArgs args = parse_args(argc, argv);

    // ── Init mode: create processor directory and exit ────────────────────────
    if (args.init_only)
    {
        return do_init(args.proc_dir, args.init_name);
    }

    // ── Load config ───────────────────────────────────────────────────────────
    pylabhub::processor::ProcessorConfig config;
    try
    {
        if (!args.proc_dir.empty())
            config = pylabhub::processor::ProcessorConfig::from_directory(args.proc_dir);
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

        std::string password;
        if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD"))
        {
            password = env;
        }
        else
        {
            password = read_password_interactive("Processor vault password (empty = no encryption): ");
            const std::string confirm = read_password_interactive("Confirm password: ");
            if (password != confirm)
            {
                std::cerr << "Error: passwords do not match\n";
                return 1;
            }
        }

        try
        {
            const auto vault = pylabhub::utils::ActorVault::create(
                config.auth.keyfile, config.processor_uid, password);

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
    LifecycleGuard runner_lifecycle(MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule(),
        pylabhub::hub::GetLifecycleModule()           // DataExchangeHub — required for SHM DataBlock
    ));

    // ── Load processor keypair (if keyfile configured) ────────────────────────
    if (!config.auth.keyfile.empty())
    {
        const std::string vault_password = get_processor_password("Processor vault password: ");
        config.auth.load_keypair(config.processor_uid, vault_password);
    }

    // ── Processor script host ─────────────────────────────────────────────────
    // ProcessorScriptHost owns the Python interpreter lifetime via PythonScriptHost.
    // The interpreter thread:
    //   1. Loads the processor script package (GIL held)
    //   2. Connects Consumer + Producer, starts ZMQ + loop threads (releases GIL)
    //   3. Calls signal_ready_() — unblocks startup_() below
    //   4. Waits until stop_ is set (by shutdown_()) or api.stop() fires
    //   5. Stops threads, calls on_stop, releases all py::objects (Py_Finalize)
    // Resolve the config directory for status display.
    const std::string config_dir = !args.proc_dir.empty()
        ? args.proc_dir
        : std::filesystem::path(args.config_path).parent_path().string();

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

    // Main thread polls g_shutdown until a signal fires or api.stop() propagates it.
    while (!g_shutdown.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));

        // Check if ScriptHost internal shutdown was requested (e.g. via
        // CHANNEL_CLOSING_NOTIFY or api.stop()) but g_shutdown was not set.
        if (!proc_script.is_running())
        {
            LOGGER_INFO("[proc-main] ScriptHost no longer running, exiting main loop");
            break;
        }
    }

    LOGGER_INFO("[proc-main] Main loop exited: g_shutdown={}", g_shutdown.load());

    // ── Tear down ─────────────────────────────────────────────────────────────
    signal_handler.uninstall();
    proc_script.shutdown_();
    return 0;
    // LifecycleGuard destructor: ZMQContext → JsonConfig → Crypto → FileLock → Logger
}
