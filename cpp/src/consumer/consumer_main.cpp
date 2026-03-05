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
 *       "timeout_ms": 5000,
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

struct ConsArgs
{
    std::string config_path;
    std::string cons_dir;
    std::string init_name;  ///< --name for --init (skips stdin prompt)
    bool        validate_only{false};
    bool        keygen_only{false};
    bool        init_only{false};
};

void print_usage(const char *prog)
{
    std::cout
        << "Usage:\n"
        << "  " << prog << " --init [<cons_dir>]              # Create consumer directory\n"
        << "  " << prog << " <cons_dir>                        # Run from directory\n"
        << "  " << prog << " --config <path.json> [--validate | --keygen | --run]\n\n"
        << "Options:\n"
        << "  --init [dir]    Create consumer directory with consumer.json template; exit 0\n"
        << "  --name <name>   Consumer name for --init (skips interactive prompt)\n"
        << "  <cons_dir>      Consumer directory containing consumer.json\n"
        << "  --config <path> Path to consumer JSON config file\n"
        << "  --validate      Validate config + script, print schema layout; exit 0 on success\n"
        << "  --keygen        Generate consumer NaCl keypair at auth.keyfile path; exit 0\n"
        << "  --run           Explicit run mode (default when no other mode given)\n"
        << "  --help          Show this message\n";
}

ConsArgs parse_args(int argc, char *argv[])
{
    ConsArgs args;
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
                args.cons_dir = argv[++i];
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
            // no-op
        }
        else if (arg[0] != '-')
        {
            if (!args.cons_dir.empty())
            {
                std::cerr << "Error: multiple positional arguments not supported\n\n";
                print_usage(argv[0]);
                std::exit(1);
            }
            args.cons_dir = std::string(arg);
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    if (!args.init_only && args.config_path.empty() && args.cons_dir.empty())
    {
        std::cerr << "Error: specify a consumer directory, --init, or --config <path>\n\n";
        print_usage(argv[0]);
        std::exit(1);
    }
    return args;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Password helpers
// ---------------------------------------------------------------------------

static std::string read_password_interactive(const char *prompt)
{
#if defined(PYLABHUB_IS_POSIX)
    char *pw = ::getpass(prompt);
    if (!pw)
    {
        std::fprintf(stderr, "consumer: failed to read password from terminal\n");
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

static std::string get_consumer_password(const char *prompt)
{
    if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD"))
        return env;
    return read_password_interactive(prompt);
}

// ---------------------------------------------------------------------------
// do_init — create consumer directory with consumer.json template
// ---------------------------------------------------------------------------

static int do_init(const std::string &cons_dir_str, const std::string &cli_name)
{
    namespace fs = std::filesystem;

    const fs::path cons_dir = cons_dir_str.empty()
                              ? fs::current_path()
                              : fs::path(cons_dir_str);

    std::error_code ec;
    fs::create_directories(cons_dir / "logs",              ec);
    fs::create_directories(cons_dir / "run",               ec);
    fs::create_directories(cons_dir / "vault",             ec);
    fs::create_directories(cons_dir / "script" / "python", ec);
    if (ec)
    {
        std::cerr << "Error: cannot create directory '" << cons_dir.string()
                  << "': " << ec.message() << "\n";
        return 1;
    }

    const fs::path json_path = cons_dir / "consumer.json";
    if (fs::exists(json_path))
    {
        std::cerr << "Error: consumer.json already exists at '" << json_path.string()
                  << "'. Remove it first or choose a different directory.\n";
        return 1;
    }

    std::string cons_name;
    if (!cli_name.empty())
    {
        cons_name = cli_name;
    }
#if defined(PYLABHUB_IS_POSIX)
    else if (::isatty(STDIN_FILENO))
    {
        std::cout << "Consumer name (human-readable, e.g. 'Logger'): ";
        std::getline(std::cin, cons_name);
    }
    else
    {
        cons_name = "Consumer";
    }
#else
    else
    {
        std::cout << "Consumer name (human-readable, e.g. 'Logger'): ";
        std::getline(std::cin, cons_name);
    }
#endif

    const std::string cons_uid = pylabhub::uid::generate_consumer_uid(cons_name);

    nlohmann::json j;
    j["hub_dir"] = "<replace with hub directory path, e.g. /var/pylabhub/my_hub>";

    j["consumer"]["uid"]       = cons_uid;
    j["consumer"]["name"]      = cons_name;
    j["consumer"]["log_level"] = "info";
    j["consumer"]["auth"]["keyfile"] = "";

    j["channel"]    = "lab.my.channel";
    j["timeout_ms"] = 5000;

    j["shm"]["enabled"] = true;
    j["shm"]["secret"]  = 0;

    j["slot_schema"]["fields"] = nlohmann::json::array({
        nlohmann::json{{"name", "value"}, {"type", "float32"}}
    });
    j["flexzone_schema"] = nullptr;

    j["script"]["path"] = ".";

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
        "    Called on each incoming slot (or timeout if timeout_ms > 0).\n"
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

    const ConsArgs args = parse_args(argc, argv);

    if (args.init_only)
        return do_init(args.cons_dir, args.init_name);

    pylabhub::consumer::ConsumerConfig config;
    try
    {
        if (!args.cons_dir.empty())
            config = pylabhub::consumer::ConsumerConfig::from_directory(args.cons_dir);
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

        std::string password;
        if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD"))
        {
            password = env;
        }
        else
        {
            password = read_password_interactive("Consumer vault password (empty = no encryption): ");
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
                config.auth.keyfile, config.consumer_uid, password);
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
    const std::string config_dir = !args.cons_dir.empty()
        ? args.cons_dir
        : std::filesystem::path(args.config_path).parent_path().string();

    LifecycleGuard runner_lifecycle(MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule(),
        pylabhub::hub::GetLifecycleModule()           // DataExchangeHub — required for SHM DataBlock
    ));

    if (!config.auth.keyfile.empty())
    {
        const std::string vault_password = get_consumer_password("Consumer vault password: ");
        config.auth.load_keypair(config.consumer_uid, vault_password);
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

    while (!g_shutdown.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));

        // Check if ScriptHost internal shutdown was requested (e.g. via
        // CHANNEL_CLOSING_NOTIFY or api.stop()) but g_shutdown was not set.
        if (!cons_script.is_running())
        {
            LOGGER_INFO("[cons-main] ScriptHost no longer running, exiting main loop");
            break;
        }
    }

    LOGGER_INFO("[cons-main] Main loop exited: g_shutdown={}", g_shutdown.load());
    signal_handler.uninstall();
    cons_script.shutdown_();
    return 0;
}
