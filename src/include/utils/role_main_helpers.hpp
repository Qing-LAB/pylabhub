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
#include "plh_version_registry.hpp"
#include "utils/config/role_config.hpp"
#include "utils/interactive_signal_handler.hpp"
#include "utils/timeout_constants.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <source_location>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace pylabhub::scripting
{

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
 * The logger is started with the default (stderr) sink. After the role
 * config has been loaded, call @ref configure_logger_from_config to
 * attach the rotating file sink at the canonical path
 * `<role_dir>/logs/<uid>-<timestamp>.log` with rotation parameters taken
 * from the config's @c LoggingConfig section (HEP-0024 §12 CLI↔Config
 * separation: no CLI flag redirects runtime log output).
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
        pylabhub::hub::GetDataBlockModule()
    );
}

/**
 * @brief Attach a rotating file sink to the logger using config-driven
 *        rotation parameters and a convention-composed path.
 *
 * Path: @p cfg.logging().file_path if non-empty (treated as an absolute
 * or role-dir-relative path), otherwise auto-composed as
 * `<cfg.base_dir()>/logs/<cfg.identity().uid>.log` — the rotating sink's
 * timestamped mode (when @c LoggingConfig::timestamped is true) expands
 * this into `<...>/<uid>-YYYY-MM-DD-HH-MM-SS.uuuuuu.log` at open time.
 *
 * Rotation: @c max_size_bytes / @c max_backup_files / @c timestamped all
 * come from @p cfg.logging() — which is populated from the JSON's
 * @c logging section (optionally pre-seeded by the `--log-maxsize` /
 * `--log-backups` init-time CLI flags).
 *
 * Call once, right after RoleConfig has been successfully loaded. The
 * command is enqueued asynchronously on the logger worker; returns
 * synchronously once enqueue succeeds.
 *
 * @return true on successful enqueue; false on pre-flight error (e.g.
 *         unwritable directory). On failure the logger continues to use
 *         its existing sink (stderr) and the error reason is in @p ec.
 */
inline bool configure_logger_from_config(const config::RoleConfig &cfg,
                                           std::error_code &ec,
                                           const char *log_tag)
{
    namespace fs = std::filesystem;
    const auto &lc = cfg.logging();

    fs::path base = lc.file_path.empty()
                        ? (cfg.base_dir() / "logs" / (cfg.identity().uid + ".log"))
                        : fs::path(lc.file_path);
    if (base.is_relative())
        base = cfg.base_dir() / base;

    // Ensure the parent directory exists; logger pre-flight does this too,
    // but failing early here gives a cleaner diagnostic.
    std::error_code mk_ec;
    fs::create_directories(base.parent_path(), mk_ec);
    // (mk_ec swallowed — pre-flight inside set_rotating_logfile will catch)

    pylabhub::utils::Logger::RotatingLogConfig rcfg{};
    rcfg.max_file_size_bytes = lc.max_size_bytes;
    rcfg.max_backup_files    = lc.max_backup_files;
    rcfg.timestamped_names   = lc.timestamped;
    rcfg.use_flock           = true;

    if (!pylabhub::utils::Logger::instance()
             .set_rotating_logfile(base, rcfg, ec))
    {
        LOGGER_ERROR("{} Failed to attach rotating log sink at '{}': {}",
                     log_tag, base.string(), ec.message());
        return false;
    }
    LOGGER_INFO("{} Log sink: {} (max_size={} MiB, backups={}, timestamped={})",
                log_tag, base.string(),
                lc.max_size_bytes / (1024.0 * 1024.0),
                (lc.max_backup_files ==
                 pylabhub::config::LoggingConfig::kKeepAllBackups)
                    ? std::string("all")
                    : std::to_string(lc.max_backup_files),
                lc.timestamped);
    return true;
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
// Version info logging (call after LifecycleGuard construction)
// ============================================================================

/**
 * @brief Log the centralized version info string at startup.
 *
 * Call immediately after LifecycleGuard construction (Logger is ready).
 *
 * @param log_tag  Tag for the log message, e.g. "[prod-main]".
 */
inline void log_version_info(const char *log_tag)
{
    LOGGER_INFO("{} {}", log_tag, pylabhub::version::version_info_string());
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
