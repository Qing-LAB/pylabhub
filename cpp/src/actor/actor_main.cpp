/**
 * @file actor_main.cpp
 * @brief pylabhub-actor — multi-role scripted actor host process.
 *
 * ## Usage
 *
 *     pylabhub-actor --config <path.json>              # Run (default)
 *     pylabhub-actor --config <path.json> --validate   # Validate + print layout; exit 0/1
 *     pylabhub-actor --config <path.json> --list-roles # Show role activation summary; exit 0
 *     pylabhub-actor --config <path.json> --keygen     # Generate actor keypair; exit 0
 *     pylabhub-actor --config <path.json> --run        # Explicit run mode
 *
 * ## Multi-role config format
 *
 *     {
 *       "actor": { "uid": "sensor_001", "name": "TempSensor", "log_level": "info" },
 *       "script": "sensor_node.py",
 *       "roles": {
 *         "raw_out": {
 *           "kind": "producer",
 *           "channel": "lab.sensor.temperature",
 *           "broker": "tcp://127.0.0.1:5570",
 *           "interval_ms": 100,
 *           "slot_schema": { "fields": [{"name": "ts", "type": "float64"},
 *                                       {"name": "value", "type": "float32"}] },
 *           "shm": { "enabled": true, "slot_count": 8, "secret": 0 }
 *         },
 *         "cfg_in": {
 *           "kind": "consumer",
 *           "channel": "lab.config.setpoints",
 *           "broker": "tcp://127.0.0.1:5570",
 *           "timeout_ms": 5000,
 *           "slot_schema": { "fields": [{"name": "setpoint", "type": "float32"}] }
 *         }
 *       }
 *     }
 *
 * ## Python script interface
 *
 *     import pylabhub_actor as actor
 *
 *     @actor.on_init("raw_out")           # called once before write loop
 *     def raw_out_init(flexzone, api): ...
 *
 *     @actor.on_write("raw_out")          # called every interval_ms
 *     def write_raw(slot, flexzone, api) -> bool: ...   # True/None=commit, False=discard
 *
 *     @actor.on_message("raw_out")        # called on consumer ctrl message
 *     def raw_out_ctrl(sender, data, api): ...
 *
 *     @actor.on_stop("raw_out")           # producer stop
 *     def raw_out_stop(flexzone, api): ...
 *
 *     @actor.on_init("cfg_in")            # called once before read loop
 *     def cfg_in_init(flexzone, api): ...
 *
 *     @actor.on_read("cfg_in")            # called per slot or on timeout
 *     def read_cfg(slot, flexzone, api, *, timed_out: bool = False): ...
 *
 *     @actor.on_data("cfg_in")            # called per ZMQ broadcast frame
 *     def zmq_data(data, api): ...
 *
 *     @actor.on_stop_c("cfg_in")          # consumer stop
 *     def cfg_in_stop(flexzone, api): ...
 *
 * ## Backward compatibility
 *
 * The legacy flat single-role format ("role", "channel", "broker", "script") is
 * still accepted with a deprecation warning. The script must define top-level
 * `on_write`/`on_read` functions (not decorated) — these are found by attribute
 * lookup using the channel name as the role name.
 */

#include "actor_config.hpp"
#include "actor_host.hpp"

#include "plh_datahub.hpp"
#include "utils/zmq_context.hpp"

#include <pybind11/embed.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
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
    std::string config_path;
    bool        validate_only{false};
    bool        list_roles{false};
    bool        keygen_only{false};
};

void print_usage(const char *prog)
{
    std::cout
        << "Usage:\n"
        << "  " << prog << " --config <path.json> [--validate | --list-roles | --keygen | --run]\n\n"
        << "Options:\n"
        << "  --config <path>   Path to actor JSON config (required)\n"
        << "  --validate        Validate script and print layout; exit 0 on success\n"
        << "  --list-roles      Show configured roles and activation status; exit 0\n"
        << "  --keygen          Generate actor NaCl keypair at auth.keyfile path; exit 0\n"
        << "  --run             Explicit run mode (default when no other mode given)\n"
        << "  --help            Show this message\n";
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
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    if (args.config_path.empty())
    {
        std::cerr << "Error: --config <path> is required\n\n";
        print_usage(argv[0]);
        std::exit(1);
    }
    return args;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Parse arguments ───────────────────────────────────────────────────────
    const ActorArgs args = parse_args(argc, argv);

    // ── Load config ───────────────────────────────────────────────────────────
    pylabhub::actor::ActorConfig config;
    try
    {
        config = pylabhub::actor::ActorConfig::from_json_file(args.config_path);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    // ── keygen mode: generate NaCl keypair and exit ───────────────────────────
    if (args.keygen_only)
    {
        if (config.auth.keyfile.empty())
        {
            std::cerr << "Error: --keygen requires 'actor.auth.keyfile' in config\n";
            return 1;
        }
        // CryptoUtils::generate_keypair_file() — write pubkey+seckey to keyfile.
        // Actor keypair generation is deferred to a future implementation that
        // extends CryptoUtils. For now, print a placeholder.
        std::cout << "Keypair generation for actor '" << config.actor_uid << "'\n"
                  << "Target file: " << config.auth.keyfile << "\n"
                  << "(keypair generation not yet implemented — "
                     "see SECURITY_TODO Phase 5)\n";
        return 0;
    }

    // ── Lifecycle guard ───────────────────────────────────────────────────────
    LifecycleGuard runner_lifecycle(MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule(),
        pylabhub::hub::GetLifecycleModule()
    ));

    // ── Python interpreter ────────────────────────────────────────────────────
    py::scoped_interpreter python_guard{};

    // ── Messenger ─────────────────────────────────────────────────────────────
    auto &messenger = pylabhub::hub::Messenger::get_instance();

    // ── Create actor host ─────────────────────────────────────────────────────
    pylabhub::actor::ActorHost host(config, messenger);
    g_host_ptr = &host;

    // Load script: imports Python file, reads dispatch table
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
