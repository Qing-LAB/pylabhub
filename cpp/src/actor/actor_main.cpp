/**
 * @file actor_main.cpp
 * @brief pylabhub-actor — scripted producer/consumer host process.
 *
 * ## Usage
 *
 *     pylabhub-actor --config <path.json>            # Run (default)
 *     pylabhub-actor --config <path.json> --validate # Validate script only; exit 0/1
 *     pylabhub-actor --config <path.json> --run      # Explicit run mode
 *
 * ## Config file format
 *
 *     {
 *       "role":     "producer",                   // or "consumer"
 *       "channel":  "lab.sensor.temperature",
 *       "broker":   "tcp://127.0.0.1:5570",
 *       "script":   "my_script.py",
 *       "shm": {
 *         "enabled":    true,
 *         "slot_count": 4,
 *         "slot_size":  1024,
 *         "secret":     0
 *       },
 *       "write_interval_ms": 10,
 *       "log_level": "info"
 *     }
 *
 * ## Script interface (producer)
 *
 *     def on_init(ctx):              ...  # optional
 *     def on_write(ctx):             ...  # required
 *     def on_message(ctx, id, data): ...  # optional
 *     def on_stop(ctx):              ...  # optional
 *
 * ## Script interface (consumer)
 *
 *     def on_init(ctx):        ...  # optional
 *     def on_data(ctx, data):  ...  # required (ZMQ frames)
 *     def on_read(ctx, data):  ...  # optional (SHM slots when has_shm=true)
 *     def on_stop(ctx):        ...  # optional
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
// Global shutdown flag
// ---------------------------------------------------------------------------

static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int /*sig*/) noexcept
{
    if (g_shutdown_requested.load(std::memory_order_relaxed))
    {
        std::_Exit(1); // double signal — fast exit
    }
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Argument parsing helpers
// ---------------------------------------------------------------------------

namespace
{

struct ActorArgs
{
    std::string config_path;
    bool        validate_only{false};
};

void print_usage(const char *prog)
{
    std::cout
        << "Usage:\n"
        << "  " << prog << " --config <path.json> [--validate | --run]\n\n"
        << "Options:\n"
        << "  --config <path>   Path to runner JSON config (required)\n"
        << "  --validate        Validate script only; do not run. Exit 0 = OK.\n"
        << "  --run             Explicit run mode (default if --validate absent)\n"
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
        else if (arg == "--run")
        {
            args.validate_only = false;
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

    // ── Lifecycle guard ───────────────────────────────────────────────────────
    // Order: Logger → FileLock → CryptoUtils → JsonConfig → ZMQContext → Messenger
    LifecycleGuard runner_lifecycle(MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule(),
        pylabhub::hub::GetLifecycleModule() // Messenger (DataExchangeHub)
    ));

    // ── Python interpreter ────────────────────────────────────────────────────
    py::scoped_interpreter python_guard{};

    // ── Messenger: connect to the broker ─────────────────────────────────────
    auto &messenger = pylabhub::hub::Messenger::get_instance();

    // Override broker endpoint from config (Messenger uses its own config by default,
    // but we allow the runner config to override it for standalone use).
    // For now: connect() uses HubConfig if available; the runner config endpoint
    // is used when connecting to the broker in the ProducerActorHost/ConsumerActorHost.
    // (Full endpoint override requires Messenger API extension — deferred.)

    // ── Create and start the script host ─────────────────────────────────────
    if (config.role == pylabhub::actor::ActorConfig::Role::Producer)
    {
        pylabhub::actor::ProducerActorHost host(config, messenger);

        if (!host.load_script(/*verbose_validation=*/args.validate_only))
        {
            return 1;
        }
        if (args.validate_only)
        {
            std::cout << "Validation passed.\n";
            return 0;
        }

        if (!host.start())
        {
            std::cerr << "Failed to start producer runner.\n";
            return 1;
        }

        while (!g_shutdown_requested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        host.stop();
    }
    else
    {
        pylabhub::actor::ConsumerActorHost host(config, messenger);

        if (!host.load_script(/*verbose_validation=*/args.validate_only))
        {
            return 1;
        }
        if (args.validate_only)
        {
            std::cout << "Validation passed.\n";
            return 0;
        }

        if (!host.start())
        {
            std::cerr << "Failed to start consumer runner.\n";
            return 1;
        }

        while (!g_shutdown_requested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        host.stop();
    }

    return 0;
}
