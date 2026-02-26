/**
 * @file hubshell.cpp
 * @brief pyLabHub main entry point.
 *
 * Initialises all lifecycle modules in dependency order, starts the
 * BrokerService (channel discovery hub) in a background thread, executes
 * the optional Python startup script, and blocks until a shutdown is
 * requested (SIGINT / SIGTERM / pylabhub.shutdown()).
 *
 * Command-line interface
 * ----------------------
 *   pylabhub-hubshell --init [<hub_dir>]
 *       First-time setup: prompt hub_name + password, generate UUID4,
 *       write hub.json and encrypted hub.vault.  Defaults to current dir.
 *
 *   pylabhub-hubshell [<hub_dir>] [--dev]
 *       Normal run.  When <hub_dir> is given (and --dev is absent), the
 *       master password is read to unlock hub.vault; the stable broker
 *       CurveZMQ keypair is loaded from the vault.  With --dev an ephemeral
 *       keypair is generated and no password is required.  When <hub_dir>
 *       is omitted the legacy flat-config mode is used (backward compat).
 *
 * Password sources (checked in order)
 * ------------------------------------
 *   1. PYLABHUB_MASTER_PASSWORD environment variable (service / CI)
 *   2. Interactive terminal prompt via getpass()
 *
 * Shutdown sequence
 * -----------------
 * 1. Signal (SIGINT / SIGTERM) or `pylabhub.shutdown()` sets
 *    `g_shutdown_requested`.
 * 2. main() stops the broker and joins its thread.
 * 3. LifecycleGuard destructor tears down modules in reverse order:
 *    AdminShell → PythonInterpreter → HubConfig → Messenger → ZMQContext
 *    → CryptoUtils → Logger.
 *
 * Double-SIGINT fast-exit
 * -----------------------
 * A second SIGINT while shutdown is in progress calls `std::_Exit(1)` to
 * avoid hanging on a slow teardown (e.g., stuck ZMQ socket).
 */
#include "plh_datahub.hpp"
#include "hub_python/admin_shell.hpp"
#include "hub_python/python_interpreter.hpp"
#include "hub_python/pylabhub_module.hpp"

#include "utils/broker_service.hpp"
#include "utils/hub_identity.hpp"
#include "utils/hub_vault.hpp"
#include "utils/zmq_context.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#if defined(PYLABHUB_IS_POSIX)
#include <unistd.h>
#endif

namespace py = pybind11;
namespace fs = std::filesystem;

using namespace pylabhub::utils;

// ---------------------------------------------------------------------------
// Global shutdown flag
// ---------------------------------------------------------------------------

static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int /*sig*/) noexcept
{
    if (g_shutdown_requested.load(std::memory_order_relaxed))
    {
        // Double SIGINT / SIGTERM: fast exit without waiting for cleanup.
        std::_Exit(1);
    }
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Password helpers
// ---------------------------------------------------------------------------

/// Read password from terminal with echo suppressed.
static std::string read_password_interactive(const char* prompt)
{
#if defined(PYLABHUB_IS_POSIX)
    char* pw = ::getpass(prompt);
    if (!pw)
    {
        std::fprintf(stderr, "hubshell: failed to read password from terminal.\n");
        return {};
    }
    return pw;
#else
    std::fprintf(stderr, "Warning: echo suppression not available on this platform.\n");
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string pw;
    std::getline(std::cin, pw);
    return pw;
#endif
}

/// Get master password: PYLABHUB_MASTER_PASSWORD env var, then interactive prompt.
static std::string get_password(const char* prompt)
{
    if (const char* env = std::getenv("PYLABHUB_MASTER_PASSWORD"))
        return env;
    return read_password_interactive(prompt);
}

// ---------------------------------------------------------------------------
// --init flow
// ---------------------------------------------------------------------------

static int do_init(const fs::path& hub_dir)
{
    // Create directory structure.
    std::error_code ec;
    fs::create_directories(hub_dir / "logs", ec);
    fs::create_directories(hub_dir / "run",  ec);
    if (ec)
    {
        std::fprintf(stderr, "hubshell --init: cannot create directory '%s': %s\n",
                     hub_dir.string().c_str(), ec.message().c_str());
        return 1;
    }

    // Refuse re-init if hub.json already exists.
    if (fs::exists(hub_dir / "hub.json"))
    {
        std::fprintf(stderr,
                     "hubshell --init: hub.json already exists in '%s'.\n"
                     "  To re-initialize, remove hub.json and hub.vault first.\n",
                     hub_dir.string().c_str());
        return 1;
    }

    // Prompt hub_name.
    std::printf("Hub name (reverse-domain, e.g. lab.physics.daq1): ");
    std::fflush(stdout);
    std::string hub_name;
    if (!std::getline(std::cin, hub_name) || hub_name.empty())
    {
        std::fprintf(stderr, "hubshell --init: hub name is required.\n");
        return 1;
    }

    // Prompt master password (with confirmation when interactive).
    std::string password;
    if (const char* env = std::getenv("PYLABHUB_MASTER_PASSWORD"))
    {
        password = env;
        std::printf("Using password from PYLABHUB_MASTER_PASSWORD.\n");
    }
    else
    {
        password = read_password_interactive("Master password (empty = no encryption): ");
        const std::string confirm = read_password_interactive("Confirm password: ");
        if (password != confirm)
        {
            std::fprintf(stderr, "hubshell --init: passwords do not match.\n");
            return 1;
        }
    }

    // Generate hub_uid (UUID4 via libsodium).
    const std::string hub_uid = pylabhub::utils::generate_uuid4();

    try
    {
        // Create vault — generates broker CurveZMQ keypair + admin token.
        auto vault = pylabhub::utils::HubVault::create(hub_dir, hub_uid, password);

        // Write hub.json.
        // Note: admin_token is stored in hub.json for HubConfig compatibility
        // (Phase 1 pragmatic choice; Phase 5 removes admin.token from hub.json).
        const nlohmann::json hub_json = {
            {"hub", {
                {"name",            hub_name},
                {"uid",             hub_uid},
                {"description",     "pyLabHub instance"},
                {"broker_endpoint", "tcp://0.0.0.0:5570"},
                {"admin_endpoint",  "tcp://127.0.0.1:5600"}
            }},
            {"admin", {
                {"token", vault.admin_token()}
            }},
            {"broker", {
                {"channel_timeout_s",         10},
                {"consumer_liveness_check_s",  5}
            }}
        };

        {
            std::ofstream f(hub_dir / "hub.json");
            if (!f)
            {
                std::fprintf(stderr, "hubshell --init: cannot write hub.json\n");
                return 1;
            }
            f << hub_json.dump(2) << '\n';
        }

        const fs::path abs_dir = fs::weakly_canonical(hub_dir);
        std::printf("\nHub initialized successfully.\n");
        std::printf("  Location : %s\n", abs_dir.string().c_str());
        std::printf("  hub_name : %s\n", hub_name.c_str());
        std::printf("  hub_uid  : %s\n", hub_uid.c_str());
        std::printf("\nRun with: pylabhub-hubshell %s\n", abs_dir.string().c_str());
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "hubshell --init: failed: %s\n", e.what());
        return 1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Normal run flow
// ---------------------------------------------------------------------------

static int do_run(const fs::path& hub_dir, bool dev_mode)
{
    std::string vault_broker_secret;
    std::string vault_broker_public;

    // --- Hub directory mode (non-dev): unlock vault for stable keypair. ---
    if (!hub_dir.empty() && !dev_mode)
    {
        const fs::path hub_json_path = hub_dir / "hub.json";
        if (!fs::exists(hub_json_path))
        {
            std::fprintf(stderr,
                         "hubshell: hub.json not found in '%s'.\n"
                         "  Run: pylabhub-hubshell --init %s\n",
                         hub_dir.string().c_str(), hub_dir.string().c_str());
            return 1;
        }

        // Read hub_uid — used as Argon2id salt for vault decryption.
        std::string hub_uid;
        try
        {
            std::ifstream f(hub_json_path);
            const auto j = nlohmann::json::parse(f);
            hub_uid = j.at("hub").at("uid").get<std::string>();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "hubshell: cannot read hub_uid from hub.json: %s\n", e.what());
            return 1;
        }

        // Unlock vault.
        try
        {
            const std::string password = get_password("Master password: ");
            auto vault = pylabhub::utils::HubVault::open(hub_dir, hub_uid, password);
            vault.publish_public_key(hub_dir);            // writes hub.pubkey at 0644
            vault_broker_secret = vault.broker_curve_secret_key();
            vault_broker_public = vault.broker_curve_public_key();
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "hubshell: vault unlock failed: %s\n", e.what());
            return 1;
        }

        // Point HubConfig at hub.json (replaces flat-config discovery).
        pylabhub::HubConfig::set_config_path(hub_json_path);
    }
    else if (!hub_dir.empty() && dev_mode)
    {
        // Dev mode with hub_dir: use hub.json for config, ephemeral keypair (no vault).
        const fs::path hub_json_path = hub_dir / "hub.json";
        if (fs::exists(hub_json_path))
            pylabhub::HubConfig::set_config_path(hub_json_path);
    }
    // else: legacy flat-config mode — HubConfig discovers config on its own.

    // -----------------------------------------------------------------------
    // Signal handling — must be set up before lifecycle starts.
    // -----------------------------------------------------------------------
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Wire Python's pylabhub.shutdown() into our shutdown flag.
    pylabhub::PythonInterpreter::set_shutdown_callback([]()
    {
        g_shutdown_requested.store(true, std::memory_order_relaxed);
    });

    // -----------------------------------------------------------------------
    // Lifecycle guard — starts modules in dependency order.
    //
    // Order: Logger → FileLock → CryptoUtils → JsonConfig → HubConfig
    //        → ZMQContext → DataExchangeHub (Messenger) → PythonInterpreter
    //        → AdminShell
    //
    // Teardown (reverse): AdminShell → PythonInterpreter → Messenger
    //                      → ZMQContext → HubConfig → JsonConfig
    //                      → CryptoUtils → FileLock → Logger
    // -----------------------------------------------------------------------
    LifecycleGuard app_lifecycle(MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::HubConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule(),
        pylabhub::hub::GetLifecycleModule(),          // Messenger (DataExchangeHub)
        pylabhub::PythonInterpreter::GetLifecycleModule(),
        pylabhub::AdminShell::GetLifecycleModule()
    ));

    // -----------------------------------------------------------------------
    // BrokerService — built from HubConfig, runs in its own thread.
    // -----------------------------------------------------------------------
    const auto& hub_cfg = pylabhub::HubConfig::get_instance();

    pylabhub::broker::BrokerService::Config broker_cfg;
    broker_cfg.endpoint                         = hub_cfg.broker_endpoint();
    broker_cfg.use_curve                        = true;
    broker_cfg.channel_timeout                  = hub_cfg.channel_timeout();
    broker_cfg.consumer_liveness_check_interval = hub_cfg.consumer_liveness_check();
    broker_cfg.server_secret_key                = vault_broker_secret; // empty → ephemeral
    broker_cfg.server_public_key                = vault_broker_public;
    broker_cfg.on_ready = [](const std::string& endpoint, const std::string& pubkey)
    {
        LOGGER_INFO("HubShell: broker ready at {} (pubkey={})", endpoint, pubkey);
    };

    pylabhub::broker::BrokerService broker(broker_cfg);

    std::thread broker_thread([&broker]() { broker.run(); });

    // -----------------------------------------------------------------------
    // Wire pylabhub.channels() → BrokerService::list_channels_json_str().
    // -----------------------------------------------------------------------
    pylabhub::hub_python::set_channels_callback(
        [&broker]() -> std::vector<py::dict>
        {
            // Release the GIL while querying the broker (broker mutex may briefly block).
            std::string json_str;
            {
                py::gil_scoped_release release;
                json_str = broker.list_channels_json_str();
            }

            std::vector<py::dict> result;
            try
            {
                auto channels_json = nlohmann::json::parse(json_str);
                for (auto& ch : channels_json)
                {
                    py::dict d;
                    d["name"]           = ch.value("name",           "");
                    d["schema_hash"]    = ch.value("schema_hash",    "");
                    d["consumer_count"] = ch.value("consumer_count", 0);
                    d["producer_pid"]   = ch.value("producer_pid",   uint64_t{0});
                    d["status"]         = ch.value("status",         "Unknown");
                    result.push_back(d);
                }
            }
            catch (const std::exception& e)
            {
                LOGGER_WARN("HubShell: channels() callback error: {}", e.what());
            }
            return result;
        });

    // -----------------------------------------------------------------------
    // Execute Python startup script (if configured).
    // -----------------------------------------------------------------------
    const auto startup_script = hub_cfg.python_startup_script();
    if (!startup_script.empty() && fs::exists(startup_script))
    {
        LOGGER_INFO("HubShell: executing startup script: {}", startup_script.string());
        std::ifstream f(startup_script);
        if (f)
        {
            std::ostringstream ss;
            ss << f.rdbuf();
            auto result = pylabhub::PythonInterpreter::get_instance().exec(ss.str());
            if (!result.success)
            {
                LOGGER_ERROR("HubShell: startup script failed: {}", result.error);
            }
            if (!result.output.empty())
            {
                LOGGER_INFO("HubShell: startup script output:\n{}", result.output);
            }
        }
        else
        {
            LOGGER_WARN("HubShell: could not open startup script: {}", startup_script.string());
        }
    }

    // -----------------------------------------------------------------------
    // Main loop — block until shutdown is requested.
    // -----------------------------------------------------------------------
    LOGGER_INFO("HubShell: running. Send SIGINT or call pylabhub.shutdown() to stop.");

    while (!g_shutdown_requested.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));
    }

    // -----------------------------------------------------------------------
    // Graceful shutdown.
    // -----------------------------------------------------------------------
    LOGGER_INFO("HubShell: shutdown requested — stopping broker...");
    broker.stop();
    broker_thread.join();
    LOGGER_INFO("HubShell: broker stopped.");

    // LifecycleGuard destructor handles the rest (AdminShell → PythonInterpreter → ...).
    return 0;
}

// ---------------------------------------------------------------------------
// main — argument parsing
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    bool        init_mode   = false;
    bool        dev_mode    = false;
    std::string hub_dir_arg;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};
        if (arg == "--init")
            init_mode = true;
        else if (arg == "--dev")
            dev_mode = true;
        else if (arg.starts_with("--"))
        {
            std::fprintf(stderr, "hubshell: unknown option: %s\n", argv[i]);
            std::fprintf(stderr,
                         "Usage:\n"
                         "  pylabhub-hubshell --init [<hub_dir>]\n"
                         "  pylabhub-hubshell [<hub_dir>] [--dev]\n");
            return 1;
        }
        else
        {
            if (!hub_dir_arg.empty())
            {
                std::fprintf(stderr, "hubshell: unexpected argument: %s\n", argv[i]);
                return 1;
            }
            hub_dir_arg = argv[i];
        }
    }

    if (init_mode && dev_mode)
    {
        std::fprintf(stderr, "hubshell: --init and --dev cannot be combined.\n");
        return 1;
    }

    const fs::path hub_dir = hub_dir_arg.empty() ? fs::path{} : fs::path{hub_dir_arg};

    if (init_mode)
    {
        const fs::path init_dir = hub_dir.empty() ? fs::current_path() : hub_dir;
        return do_init(init_dir);
    }

    return do_run(hub_dir, dev_mode);
}
