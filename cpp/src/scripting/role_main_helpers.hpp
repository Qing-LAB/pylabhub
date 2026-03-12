#pragma once
/**
 * @file role_main_helpers.hpp
 * @brief Shared helpers for pylabhub-producer, pylabhub-consumer, and pylabhub-processor
 *        main() entry points.
 *
 * These utilities encapsulate the identical boilerplate across all three role binaries:
 *   - Password prompt / env-variable lookup
 *   - Standard lifecycle module list
 *   - Signal handler lifecycle registration
 *   - Main monitoring loop
 *
 * Role-specific logic (do_init, keygen output text, status callback, config load) stays
 * in each binary's own main.cpp.
 */

#include "plh_datahub.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/role_cli.hpp"      // public TTY + password helpers (HEP-CORE-0024)
#include "utils/timeout_constants.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

namespace pylabhub::scripting
{

// ============================================================================
// TTY detection + password helpers
// ============================================================================
// These thin wrappers forward to the public pylabhub::role_cli API so that
// existing callers in producer/consumer/processor main files continue to compile
// without change while the public API becomes the single canonical implementation.

/** @copydoc pylabhub::role_cli::is_stdin_tty */
inline bool is_stdin_tty()
{
    return ::pylabhub::role_cli::is_stdin_tty();
}

/** @copydoc pylabhub::role_cli::read_password_interactive */
inline std::string read_password_interactive(const char *role_name, const char *prompt)
{
    return ::pylabhub::role_cli::read_password_interactive(role_name, prompt);
}

/** @copydoc pylabhub::role_cli::get_role_password */
inline std::optional<std::string> get_role_password(const char *role_name, const char *prompt)
{
    return ::pylabhub::role_cli::get_role_password(role_name, prompt);
}

// ============================================================================
// Lifecycle helpers
// ============================================================================

/**
 * @brief Return the standard lifecycle module list for a standalone role binary.
 *
 * Usage in main():
 * @code
 *   utils::LifecycleGuard lifecycle(scripting::role_lifecycle_modules());
 * @endcode
 *
 * All six modules (Logger, FileLock, Crypto, JsonConfig, ZMQ, DataExchangeHub) are
 * required by every role binary. A binary needing extra modules can append them:
 * @code
 *   auto mods = scripting::role_lifecycle_modules();
 *   mods.push_back(MyExtra::GetLifecycleModule());
 *   utils::LifecycleGuard lifecycle(std::move(mods));
 * @endcode
 */
inline std::vector<pylabhub::utils::ModuleDef> role_lifecycle_modules()
{
    return pylabhub::utils::MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule(),
        pylabhub::hub::GetLifecycleModule()   // DataExchangeHub — required for SHM DataBlock
    );
}

/**
 * @brief Redirect the Logger to a file sink if --log-file was specified.
 *
 * Call this immediately after creating the LifecycleGuard (which initialises
 * the Logger with the default console sink).  If @p log_file is empty this
 * is a no-op.
 *
 * @param log_file   Path from RoleArgs::log_file (may be empty).
 * @param log_tag    Tag for error messages, e.g. "[prod-main]".
 */
inline void apply_log_file(const std::string &log_file, const char *log_tag)
{
    if (log_file.empty())
        return;
    if (!pylabhub::utils::Logger::instance().set_logfile(log_file))
    {
        std::fprintf(stderr, "%s WARNING: failed to open log file '%s', "
                     "falling back to console\n", log_tag, log_file.c_str());
    }
}

/**
 * @brief Register the signal handler as a dynamic persistent lifecycle module.
 *
 * When successful, the handler is automatically uninstalled during lifecycle finalize()
 * without needing an explicit uninstall() call in the main exit path.
 *
 * A warning is logged (non-fatal) if registration fails; the handler remains installed
 * and must be uninstalled explicitly.
 *
 * @param handler   The already-installed InteractiveSignalHandler.
 * @param log_tag   Tag for the warning message, e.g. "[prod-main]".
 */
inline void register_signal_handler_lifecycle(
    pylabhub::InteractiveSignalHandler &handler, const char *log_tag)
{
    if (!pylabhub::utils::LifecycleManager::instance().register_dynamic_module(
            handler.make_lifecycle_module()) ||
        !pylabhub::utils::LifecycleManager::instance().load_module(
            "SignalHandler", std::source_location::current()))
    {
        LOGGER_WARN("{} SignalHandler lifecycle module registration failed — "
                    "falling back to explicit uninstall on exit.", log_tag);
    }
}

// ============================================================================
// Main monitoring loop
// ============================================================================

/**
 * @brief Run the main monitoring loop until g_shutdown is set or the host stops.
 *
 * Waits on host.wait_for_wakeup(kAdminPollIntervalMs) and also checks
 * host.is_running() so that an internal stop (api.stop() / CHANNEL_CLOSING_NOTIFY)
 * propagates the exit even when no signal was received.
 *
 * stop_role() calls core_.notify_incoming() which unblocks wait_for_wakeup()
 * immediately — so shutdown latency is microseconds rather than up to
 * kAdminPollIntervalMs (100 ms) with a plain sleep_for.
 *
 * On return, g_shutdown is guaranteed to be true (set via memory_order_release).
 *
 * @tparam Host  Any type with `bool is_running() const` and
 *               `void wait_for_wakeup(int ms)` methods.
 * @param g_shutdown  Shared shutdown flag (set by signal handler or api.stop()).
 * @param host        The script host being monitored.
 * @param log_tag     Tag for log messages, e.g. "[prod-main]".
 */
template <typename Host>
void run_role_main_loop(std::atomic<bool> &g_shutdown, Host &host, const char *log_tag)
{
    while (!g_shutdown.load(std::memory_order_relaxed))
    {
        host.wait_for_wakeup(pylabhub::kAdminPollIntervalMs);

        if (!host.is_running())
        {
            LOGGER_INFO("{} ScriptHost no longer running, exiting main loop", log_tag);
            break;
        }
    }

    // Ensure g_shutdown is visible to signal handler watcher before teardown starts.
    g_shutdown.store(true, std::memory_order_release);
    LOGGER_INFO("{} Main loop exited, shutting down.", log_tag);
}

} // namespace pylabhub::scripting
