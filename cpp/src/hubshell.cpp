/**
 * @file hubshell.cpp
 * @brief pyLabHub main entry point.
 *
 * Initialises all lifecycle modules in dependency order, starts the
 * BrokerService (channel discovery hub) in a background thread, executes
 * the optional Python startup script, and blocks until a shutdown is
 * requested (SIGINT / SIGTERM / pylabhub.shutdown()).
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
#include "utils/zmq_context.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
        // Double SIGINT / SIGTERM: fast exit without waiting for cleanup.
        std::_Exit(1);
    }
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
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
    broker_cfg.endpoint                        = hub_cfg.broker_endpoint();
    broker_cfg.use_curve                       = true;
    broker_cfg.channel_timeout                 = hub_cfg.channel_timeout();
    broker_cfg.consumer_liveness_check_interval = hub_cfg.consumer_liveness_check();
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
    if (!startup_script.empty() && std::filesystem::exists(startup_script))
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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
