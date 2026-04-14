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
 *   pylabhub-hubshell <hub_dir> [--dev]
 *       Normal run.  Without --dev the master password is read to unlock
 *       hub.vault; the stable broker CurveZMQ keypair is loaded from the
 *       vault.  With --dev an ephemeral keypair is used and no password
 *       is required.
 *
 *   pylabhub-hubshell --dev
 *       Development / test mode without a hub directory.  Uses built-in
 *       defaults (hub_name, broker at tcp://0.0.0.0:5570) and an ephemeral
 *       CurveZMQ keypair.  No hub.json required.
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
#include "plh_version_registry.hpp"
#include "hub_python/admin_shell.hpp"
#include "hub_python/hub_script.hpp"
#include "hub_python/python_interpreter.hpp"
#include "hub_python/pylabhub_module.hpp"

#include "utils/broker_service.hpp"
#include "utils/channel_access_policy.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/uid_utils.hpp"
#include "utils/uuid_utils.hpp"
#include "utils/hub_vault.hpp"
#include "utils/role_cli.hpp"
#include "utils/zmq_context.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "utils/json_fwd.hpp"

#include <atomic>
#include <chrono>
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
#include <unistd.h> // isatty, STDIN_FILENO
#elif defined(_WIN32)
#include <io.h>      // _isatty, _fileno
#include <windows.h> // SetConsoleMode, GetConsoleMode, ENABLE_ECHO_INPUT
#endif

namespace py = pybind11;
namespace fs = std::filesystem;

using namespace pylabhub::utils;

// ---------------------------------------------------------------------------
// Global shutdown flag — set by InteractiveSignalHandler or pylabhub.shutdown()
// ---------------------------------------------------------------------------

static std::atomic<bool> g_shutdown_requested{false};

// ---------------------------------------------------------------------------
// Password helpers (TTY detection and password reading delegated to role_cli.hpp)
// ---------------------------------------------------------------------------

/// Get master password: PYLABHUB_MASTER_PASSWORD env var → interactive prompt → error.
/// Returns false (with error already printed) when non-interactive and env var not set.
static bool get_password(const std::string& prompt, std::string& out)
{
    if (const char* env = std::getenv("PYLABHUB_MASTER_PASSWORD"))
    {
        out = env;
        return true;
    }
    if (!pylabhub::role_cli::is_stdin_tty())
    {
        std::fprintf(stderr,
                     "hubshell: vault password required; set PYLABHUB_MASTER_PASSWORD "
                     "for non-interactive use\n");
        return false;
    }
    out = pylabhub::role_cli::read_password_interactive("hubshell", prompt.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// --init flow
// ---------------------------------------------------------------------------

static int do_init(const fs::path& hub_dir, const std::string& cli_name = {})
{
    // Create directory structure.
    std::error_code ec;
    fs::create_directories(hub_dir / "logs",            ec);
    fs::create_directories(hub_dir / "run",             ec);
    fs::create_directories(hub_dir / "script" / "python", ec);
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

    // Resolve hub name: --name flag > interactive prompt; error if non-interactive.
    std::string hub_name;
    if (!cli_name.empty())
    {
        hub_name = cli_name;
    }
    else if (pylabhub::role_cli::is_stdin_tty())
    {
        std::printf("Hub name (reverse-domain, e.g. lab.physics.daq1): ");
        std::fflush(stdout);
        if (!std::getline(std::cin, hub_name) || hub_name.empty())
        {
            std::fprintf(stderr, "hubshell --init: hub name is required.\n");
            return 1;
        }
    }
    else
    {
        std::fprintf(stderr, "hubshell --init: --name is required in non-interactive mode\n");
        return 1;
    }

    // Prompt master password (with confirmation when interactive).
    std::string password;
    if (const char* env = std::getenv("PYLABHUB_MASTER_PASSWORD"))
    {
        password = env;
        std::printf("Using password from PYLABHUB_MASTER_PASSWORD.\n");
    }
    else if (!pylabhub::role_cli::is_stdin_tty())
    {
        std::fprintf(stderr,
                     "hubshell --init: vault password required; set PYLABHUB_MASTER_PASSWORD "
                     "for non-interactive use\n");
        return 1;
    }
    else
    {
        password = pylabhub::role_cli::read_password_interactive("hubshell","Master password (empty = no encryption): ");
        const std::string confirm = pylabhub::role_cli::read_password_interactive("hubshell","Confirm password: ");
        if (password != confirm)
        {
            std::fprintf(stderr, "hubshell --init: passwords do not match.\n");
            return 1;
        }
    }

    // Generate hub_uid in the canonical HUB-NAME-HEXSUFFIX format.
    const std::string hub_uid = pylabhub::uid::generate_hub_uid(hub_name);

    try
    {
        // Create vault — generates broker CurveZMQ keypair + admin token.
        auto vault = pylabhub::utils::HubVault::create(hub_dir, hub_uid, password);

        // Write hub.json.
        // SECURITY: admin_token is NOT stored in hub.json (which is 0644, world-readable).
        // It lives exclusively in the encrypted vault (0600) and is injected at runtime via
        // HubConfig::set_admin_token_override() called after vault.open() in do_run().
        const nlohmann::json hub_json = {
            {"hub", {
                {"name",            hub_name},
                {"uid",             hub_uid},
                {"description",     "pyLabHub instance"},
                {"broker_endpoint", "tcp://0.0.0.0:5570"},
                {"admin_endpoint",  "tcp://127.0.0.1:5600"}
            }},
            {"broker", {
                {"heartbeat_interval_ms",      500},
                {"ready_miss_heartbeats",       10},
                {"pending_miss_heartbeats",     10},
                {"grace_heartbeats",             4},
                {"consumer_liveness_check_s",    5}
            }},
            // Language-neutral hub script configuration.
            // "type" selects the ScriptHost subclass; "path" is the base directory.
            // Python scripts live at <path>/python/__init__.py.
            {"script", {
                {"type",                   "python"},
                {"path",                   "./script"},
                {"tick_interval_ms",       1000},
                {"health_log_interval_ms", 60000}
            }},
            // Python-specific settings (requirements only; script path moved to "script").
            {"python", {
                {"requirements", "../share/scripts/python/requirements.txt"}
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

        // Write the hub script package template (Python variant).
        {
            std::ofstream f(hub_dir / "script" / "python" / "__init__.py");
            if (f)
            {
                f << R"("""
Hub script for )" << hub_name << R"PY(.

Callbacks called by the C++ hub runtime:
  on_start(api)        -- Called once after the hub lifecycle is fully started.
  on_tick(api, tick)   -- Called every tick_interval_ms (default: every second).
  on_stop(api)         -- Called once before the hub shuts down.

Default dashboard: channels table (top) + rolling log tail (bottom).
The log is read from <hub_dir>/logs/hub.log which receives all hub output
once the file logger is active.  Replace on_tick() for custom logic.
"""
import pylabhub
from rich.live import Live
from rich.layout import Layout
from rich.table import Table
from rich.text import Text
from rich.panel import Panel
from rich import box

_live = None
_layout = None
_log_file = None

STATUS_COLORS = {
    "Ready": "green",
    "PendingReady": "yellow",
    "Closing": "red",
}


def _tail_log(path, n=18):
    """Return the last n lines of a file, or a placeholder on error."""
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            end = f.tell()
            if end == 0:
                return ["(log file empty)"]
            chunk = min(end, 8192)
            f.seek(-chunk, 2)
            data = f.read(chunk).decode("utf-8", errors="replace")
            return data.splitlines()[-n:]
    except OSError:
        return ["(log file not yet available)"]


def _make_channels_table(api, tick):
    uptime_s = tick.uptime_ms() / 1000.0
    h = int(uptime_s // 3600)
    m = int((uptime_s % 3600) // 60)
    s = int(uptime_s % 60)

    table = Table(
        title=(
            f"[bold]{api.hub_name()}[/bold]"
            f"  \u2502  uptime {h:02d}:{m:02d}:{s:02d}"
            f"  \u2502  tick #{tick.tick_count()}"
            f"  \u2502  {tick.channels_ready()} ready"
            f"  {tick.channels_pending()} pending"
            f"  {tick.channels_closing()} closing"
        ),
        box=box.SIMPLE_HEAVY,
        expand=True,
    )
    table.add_column("Channel", style="cyan", no_wrap=True)
    table.add_column("Status", justify="center")
    table.add_column("Consumers", justify="right")
    table.add_column("PID", justify="right", style="dim")
    table.add_column("Role", style="magenta")
    table.add_column("Role UID", style="dim")

    channels = api.channels()
    if not channels:
        table.add_row("[dim]no channels registered[/dim]", "", "", "", "", "")
    else:
        for ch in channels:
            color = STATUS_COLORS.get(ch.status(), "white")
            table.add_row(
                ch.name(),
                f"[{color}]{ch.status()}[/{color}]",
                str(ch.consumer_count()),
                str(ch.producer_pid()) if ch.producer_pid() else "\u2014",
                ch.producer_role_name() or "\u2014",
                ch.producer_role_uid() or "\u2014",
            )
    return table


def _make_log_panel(log_file):
    if log_file is None:
        return Panel(
            "[dim]no log file (--dev mode without hub_dir)[/dim]",
            title="Log",
            border_style="dim",
        )
    text = Text(overflow="fold")
    for line in _tail_log(log_file):
        if " ERR " in line or "[ERR]" in line:
            style = "red"
        elif " WRN " in line or "[WRN]" in line:
            style = "yellow"
        elif " DBG " in line or "[DBG]" in line:
            style = "dim"
        elif " SYS " in line or "[SYS]" in line:
            style = "bold magenta"
        else:
            style = ""
        text.append(line + "\n", style=style)
    return Panel(text, title="Log", border_style="dim", padding=(0, 1))


def on_start(api):
    global _live, _layout, _log_file
    _log_file = pylabhub.paths().get("log_file")
    api.log("info", f"Hub '{api.hub_name()}' dashboard started (uid={api.hub_uid()})")
    print(f"[hub_script] on_start: hub={api.hub_name()} uid={api.hub_uid()}", flush=True)
    _layout = Layout()
    _layout.split_column(
        Layout(name="channels", ratio=2),
        Layout(name="logs", ratio=1),
    )
    _live = Live(_layout, auto_refresh=False, screen=False)
    _live.start()


def on_tick(api, tick):
    if tick.tick_count() <= 3:
        print(f"[hub_script] on_tick #{tick.tick_count()}: "
              f"{tick.channels_ready()} ready, "
              f"{tick.channels_pending()} pending, "
              f"uptime={tick.uptime_ms()}ms", flush=True)
    if _live is None:
        return
    _layout["channels"].update(_make_channels_table(api, tick))
    _layout["logs"].update(_make_log_panel(_log_file))
    _live.refresh()


def on_stop(api):
    global _live
    print("[hub_script] on_stop called", flush=True)
    if _live is not None:
        _live.stop()
        _live = None
    api.log("info", "Hub dashboard stopped")
)PY";
            }
        }

        const fs::path abs_dir = fs::weakly_canonical(hub_dir);
        std::printf("\nHub initialized successfully.\n");
        std::printf("  Location : %s\n", abs_dir.string().c_str());
        std::printf("  hub_name : %s\n", hub_name.c_str());
        std::printf("  hub_uid  : %s\n", hub_uid.c_str());
        std::printf("  script   : %s/script/python/__init__.py\n", abs_dir.string().c_str());
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
            std::string password;
            if (!get_password("Master password: ", password))
                return 1;
            auto vault = pylabhub::utils::HubVault::open(hub_dir, hub_uid, password);
            vault.publish_public_key(hub_dir);            // writes hub.pubkey at 0644
            vault_broker_secret = vault.broker_curve_secret_key();
            vault_broker_public = vault.broker_curve_public_key();
            // Supply admin token from vault. hub.json must never store it (0644, world-readable).
            pylabhub::HubConfig::set_admin_token(vault.admin_token());
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "hubshell: vault unlock failed: %s\n", e.what());
            return 1;
        }

        // Point HubConfig at hub.json.
        pylabhub::HubConfig::set_config_path(hub_json_path);
    }
    else if (!hub_dir.empty() && dev_mode)
    {
        // Dev mode with hub_dir: use hub.json for config, ephemeral keypair (no vault).
        const fs::path hub_json_path = hub_dir / "hub.json";
        if (fs::exists(hub_json_path))
            pylabhub::HubConfig::set_config_path(hub_json_path);
    }
    else if (hub_dir.empty() && !dev_mode)
    {
        // No hub_dir without --dev: require explicit directory.
        std::fprintf(stderr,
                     "hubshell: <hub_dir> is required.\n"
                     "  Usage: pylabhub-hubshell <hub_dir>          (production)\n"
                     "         pylabhub-hubshell <hub_dir> --dev    (dev, no vault)\n"
                     "         pylabhub-hubshell --dev              (dev, built-in defaults)\n"
                     "         pylabhub-hubshell --init [<hub_dir>] (first-time setup)\n");
        return 1;
    }
    // else: hub_dir empty + dev_mode → built-in defaults, ephemeral keypair.

    // -----------------------------------------------------------------------
    // Signal handling — must be set up before lifecycle starts.
    // -----------------------------------------------------------------------
    pylabhub::InteractiveSignalHandler signal_handler(
        {.binary_name = "pylabhub-hubshell"}, &g_shutdown_requested);
    signal_handler.install();

    // Wire Python's pylabhub.shutdown() into our shutdown flag.
    pylabhub::PythonInterpreter::set_shutdown_callback([]()
    {
        g_shutdown_requested.store(true, std::memory_order_release);
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
    // When a hub_dir is available, inject a StartupLogFileSink module that switches
    // the log sink to a rotating file immediately after Logger starts — before
    // ZMQContext and DataExchangeHub emit their startup INFO messages.
    const std::string hub_log_path = hub_dir.empty()
        ? std::string{}
        : (hub_dir / "logs" / "hub.log").string();

    auto zmq_mod = pylabhub::hub::GetZMQContextModule();
    auto hub_mod = pylabhub::hub::GetLifecycleModule();          // Messenger (DataExchangeHub)

    if (!hub_log_path.empty())
    {
        // Ensure logs directory exists before lifecycle starts.
        std::error_code mkdir_ec;
        fs::create_directories(hub_dir / "logs", mkdir_ec);

        zmq_mod.add_dependency("StartupLogFileSink");
        hub_mod.add_dependency("StartupLogFileSink");
    }

    auto mods = MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::HubConfig::GetLifecycleModule(),
        std::move(zmq_mod),
        std::move(hub_mod),
        pylabhub::PythonInterpreter::GetLifecycleModule(),
        pylabhub::AdminShell::GetLifecycleModule()
    );

    if (!hub_log_path.empty())
    {
        mods.push_back(
            Logger::GetStartupLogFileSinkModule(
                hub_log_path, Logger::RotatingLogConfig{}));
    }

    LifecycleGuard app_lifecycle(std::move(mods));
    LOGGER_INFO("[hub-main] {}", pylabhub::version::version_info_string());

    // -----------------------------------------------------------------------
    // Write runtime info so tests (and tools) can discover actual bound endpoints
    // when port 0 is used.  Written after LifecycleGuard starts AdminShell.
    // -----------------------------------------------------------------------
    if (!hub_dir.empty())
    {
        const auto admin_ep = pylabhub::AdminShell::get_instance().actual_endpoint();
        const nlohmann::json runtime = {
            {"admin_endpoint", admin_ep}
        };
        std::ofstream rf(hub_dir / "runtime.json");
        if (rf.is_open())
        {
            rf << runtime.dump(2) << '\n';
            LOGGER_INFO("HubShell: wrote runtime.json (admin_endpoint={})", admin_ep);
        }
    }

    // -----------------------------------------------------------------------
    // BrokerService — built from HubConfig, runs in its own thread.
    // -----------------------------------------------------------------------
    const auto& hub_cfg = pylabhub::HubConfig::get_instance();

    pylabhub::broker::BrokerService::Config broker_cfg;
    broker_cfg.endpoint                         = hub_cfg.broker_endpoint();
    broker_cfg.use_curve                        = (broker_cfg.endpoint.rfind("tcp://", 0) == 0);
    broker_cfg.heartbeat_interval               = hub_cfg.heartbeat_interval();
    broker_cfg.ready_miss_heartbeats            = hub_cfg.ready_miss_heartbeats();
    broker_cfg.pending_miss_heartbeats          = hub_cfg.pending_miss_heartbeats();
    broker_cfg.grace_heartbeats                 = hub_cfg.grace_heartbeats();
    broker_cfg.ready_timeout_override           = hub_cfg.ready_timeout_override();
    broker_cfg.pending_timeout_override         = hub_cfg.pending_timeout_override();
    broker_cfg.grace_override                   = hub_cfg.grace_override();
    broker_cfg.consumer_liveness_check_interval = hub_cfg.consumer_liveness_check();
    broker_cfg.server_secret_key                = broker_cfg.use_curve ? vault_broker_secret : std::string{};
    broker_cfg.server_public_key                = broker_cfg.use_curve ? vault_broker_public : std::string{};
    broker_cfg.connection_policy                = hub_cfg.connection_policy();
    broker_cfg.known_roles                     = hub_cfg.known_roles();
    broker_cfg.channel_policies                 = hub_cfg.channel_policies();
    // HEP-CORE-0022: federation peers (convert HubPeerConfig → FederationPeer)
    broker_cfg.self_hub_uid = hub_cfg.hub_uid();
    for (const auto& p : hub_cfg.peers())
    {
        pylabhub::broker::FederationPeer fp;
        fp.hub_uid         = p.hub_uid;
        fp.broker_endpoint = p.broker_endpoint;
        fp.pubkey_z85      = p.pubkey_z85;
        fp.channels        = p.channels;
        broker_cfg.peers.push_back(std::move(fp));
    }
    broker_cfg.on_hub_connected = [](const std::string& hub_uid)
    {
        pylabhub::HubScript::get_instance().on_hub_peer_connected(hub_uid);
    };
    broker_cfg.on_hub_disconnected = [](const std::string& hub_uid)
    {
        pylabhub::HubScript::get_instance().on_hub_peer_disconnected(hub_uid);
    };
    broker_cfg.on_hub_message = [](const std::string& channel,
                                   const std::string& payload,
                                   const std::string& source_hub_uid)
    {
        pylabhub::HubScript::get_instance().on_hub_peer_message(channel, payload, source_hub_uid);
    };
    broker_cfg.on_ready = [dev_mode, &hub_dir](const std::string& endpoint, const std::string& pubkey)
    {
        LOGGER_INFO("HubShell: broker ready at {} (pubkey={})", endpoint, pubkey);
        // In dev mode with a hub_dir, write the ephemeral pubkey so that
        // producer/processor/consumer can discover it via hub_dir.
        if (dev_mode && !hub_dir.empty() && !pubkey.empty())
        {
            const auto pubkey_path = hub_dir / "hub.pubkey";
            std::ofstream f(pubkey_path);
            if (f.is_open())
            {
                f << pubkey << '\n';
                LOGGER_INFO("HubShell: wrote ephemeral pubkey to {}", pubkey_path.string());
            }
        }
    };
    if (hub_cfg.connection_policy() != pylabhub::broker::ConnectionPolicy::Open)
    {
        LOGGER_INFO("HubShell: connection_policy={} known_roles={}",
                    pylabhub::broker::connection_policy_to_str(hub_cfg.connection_policy()),
                    hub_cfg.known_roles().size());
    }

    if (!broker_cfg.use_curve)
    {
        LOGGER_WARN("HubShell: CURVE disabled for non-TCP broker endpoint '{}'",
                    broker_cfg.endpoint);
    }

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
    // Wire pylabhub.close_channel() → BrokerService::request_close_channel().
    // -----------------------------------------------------------------------
    pylabhub::hub_python::set_close_channel_callback(
        [&broker](const std::string& name)
        {
            py::gil_scoped_release release;
            broker.request_close_channel(name);
        });

    // -----------------------------------------------------------------------
    // Wire pylabhub.broadcast_channel() → BrokerService::request_broadcast_channel().
    // -----------------------------------------------------------------------
    pylabhub::hub_python::set_broadcast_channel_callback(
        [&broker](const std::string& channel, const std::string& message,
                  const std::string& data)
        {
            broker.request_broadcast_channel(channel, message, data);
        });

    // -----------------------------------------------------------------------
    // Wire pylabhub.metrics() → BrokerService::query_metrics_json_str().
    // -----------------------------------------------------------------------
    pylabhub::hub_python::set_metrics_callback(
        [&broker](const std::string& channel) -> std::string
        {
            return broker.query_metrics_json_str(channel);
        });

    // -----------------------------------------------------------------------
    // Wire pylabhub.blocks() → BrokerService::collect_shm_info_json().
    // -----------------------------------------------------------------------
    pylabhub::hub_python::set_blocks_callback(
        [&broker](const std::string& channel) -> std::string
        {
            return broker.collect_shm_info_json(channel);
        });

    // -----------------------------------------------------------------------
    // HubScript — loads as a dynamic lifecycle module.
    //
    // hub_thread_ (spawned inside startup_()) owns the full CPython interpreter
    // lifetime: Py_Initialize → init_namespace → load script → on_start →
    // [tick loop] → on_stop → release_namespace → Py_Finalize.
    //
    // LoadModule() blocks until hub_thread_ signals "Python ready" so that
    // subsequent code (e.g., the main wait loop) can rely on exec() working.
    // -----------------------------------------------------------------------
    pylabhub::HubScript::get_instance().set_broker(&broker);
    pylabhub::HubScript::get_instance().set_shutdown_flag(&g_shutdown_requested);

    if (!RegisterDynamicModule(pylabhub::HubScript::GetLifecycleModule()))
    {
        LOGGER_ERROR("HubShell: failed to register HubScript dynamic module");
        broker.stop();
        broker_thread.join();
        return 1;
    }

    if (!LoadModule("pylabhub::HubScript"))
    {
        LOGGER_ERROR("HubShell: Python environment initialization failed");
        broker.stop();
        broker_thread.join();
        return 1;
    }

    LOGGER_INFO("HubShell: Python environment ready");

    // -----------------------------------------------------------------------
    // Main loop — block until shutdown is requested.
    // The main thread holds no GIL.  hub_thread_ releases the GIL between
    // ticks so AdminShell::exec() calls can proceed concurrently.
    // -----------------------------------------------------------------------
    LOGGER_INFO("HubShell: running. Send SIGINT or call pylabhub.shutdown() to stop.");

    // Register status callback for interactive Ctrl-C handler.
    const auto start_time = std::chrono::steady_clock::now();
    signal_handler.set_status_callback([&]() -> std::string
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        const auto snapshot = broker.query_channel_snapshot();
        int ready = 0, pending = 0, closing = 0;
        for (const auto &ch : snapshot.channels)
        {
            if (ch.status == "Ready")         ++ready;
            else if (ch.status == "PendingReady") ++pending;
            else if (ch.status == "Closing")  ++closing;
        }

        return fmt::format(
            "  pyLabHub {} (pylabhub-hubshell)\n"
            "  Hub dir:   {}\n"
            "  Hub UID:   {}\n"
            "  Hub name:  {}\n"
            "  Broker:    {}\n"
            "  Channels:  {} ready, {} pending, {} closing\n"
            "  Uptime:    {}h {}m {}s",
            pylabhub::platform::get_version_string(),
            hub_dir.empty() ? "(dev mode)" : hub_dir.string(),
            hub_cfg.hub_uid(), hub_cfg.hub_name(),
            hub_cfg.broker_endpoint(),
            ready, pending, closing,
            secs / 3600, (secs % 3600) / 60, secs % 60);
    });

    while (!g_shutdown_requested.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(pylabhub::kAdminPollIntervalMs));
    }

    // -----------------------------------------------------------------------
    // Graceful shutdown sequence (order is critical for correctness).
    //
    // Logger is in file mode — write the notice to stderr so the user sees
    // feedback even when the rich dashboard is active.
    // -----------------------------------------------------------------------
    signal_handler.uninstall();
    std::fprintf(stderr, "\nHubShell: shutting down (press Ctrl+C again to force exit)...\n");
    LOGGER_INFO("HubShell: shutdown requested, beginning graceful teardown.");

    // Step 1: Stop AdminShell — prevents new Python exec() calls arriving
    //         while hub_thread_ is still running (and holding the GIL).
    //         AdminShell::shutdown_() is idempotent; the LifecycleGuard will
    //         call it again during static-module finalization (a no-op).
    LOGGER_INFO("HubShell: stopping admin shell...");
    pylabhub::AdminShell::get_instance().shutdown_();

    // Step 2: Schedule HubScript unload (asynchronous).
    //         The lifecycle's async shutdown thread calls hub_script.shutdown_(),
    //         which sets stop_ and joins hub_thread_ (on_stop + Py_Finalize).
    LOGGER_INFO("HubShell: unloading HubScript (on_stop + Py_Finalize will run async)...");
    (void)UnloadModule("pylabhub::HubScript");

    // Step 3: Stop the broker.  This can run concurrently with hub_thread_
    //         finishing up its on_stop callback.
    LOGGER_INFO("HubShell: stopping broker...");
    broker.stop();
    broker_thread.join();
    LOGGER_INFO("HubShell: broker stopped.");

    // Step 4: Wait for hub_thread_ to finish (on_stop + Py_Finalize).
    //         After this call, Python is fully finalized.
    LOGGER_INFO("HubShell: waiting for Python finalization...");
    WaitForUnload("pylabhub::HubScript");
    LOGGER_INFO("HubShell: Python finalized.");

    // LifecycleGuard destructor handles the rest:
    //   AdminShell (no-op, already stopped) → PythonInterpreter (no-op) → ...
    LOGGER_INFO("HubShell: termination requested by user, shutdown initiated.");
    std::fprintf(stderr,
                 "[DBG] Termination requested by user, shutdown initiated"
                 " — this may take a few seconds.\n");
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
    std::string init_name;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};
        if (arg == "--init")
            init_mode = true;
        else if (arg == "--dev")
            dev_mode = true;
        else if (arg == "--name" && i + 1 < argc)
            init_name = argv[++i];
        else if (arg.starts_with("--"))
        {
            std::fprintf(stderr, "hubshell: unknown option: %s\n", argv[i]);
            std::fprintf(stderr,
                         "Usage:\n"
                         "  pylabhub-hubshell --init [<hub_dir>]\n"
                         "  pylabhub-hubshell <hub_dir> [--dev]\n"
                         "  pylabhub-hubshell --dev\n");
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
        return do_init(init_dir, init_name);
    }

    return do_run(hub_dir, dev_mode);
}
