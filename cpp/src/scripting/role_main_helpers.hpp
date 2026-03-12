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
#include "utils/timeout_constants.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

#if defined(PYLABHUB_IS_POSIX)
#include <unistd.h> // getpass, isatty, STDIN_FILENO
#elif defined(_WIN32)
#include <io.h>      // _isatty, _fileno
#include <windows.h> // SetConsoleMode, GetConsoleMode, ENABLE_ECHO_INPUT
#endif

namespace pylabhub::scripting
{

// ============================================================================
// TTY detection
// ============================================================================

/**
 * @brief Return true when stdin is an interactive terminal (not a pipe or redirect).
 *
 * Used to gate prompts: if stdin is not a TTY, interactive prompts are skipped and
 * callers must supply values via CLI flags or environment variables instead.
 */
inline bool is_stdin_tty()
{
#if defined(PYLABHUB_IS_POSIX)
    return ::isatty(STDIN_FILENO) != 0;
#elif defined(_WIN32)
    return ::_isatty(::_fileno(stdin)) != 0;
#else
    return false;
#endif
}

// ============================================================================
// Password helpers
// ============================================================================

/**
 * @brief Read a password from the terminal without echoing.
 *
 * POSIX: uses getpass(). Windows: temporarily disables ENABLE_ECHO_INPUT on the
 * console handle. Only call this when is_stdin_tty() is true.
 *
 * @param role_name  Used in the error message when reading fails (e.g. "producer").
 * @param prompt     Prompt displayed to the user.
 * @return Password string; empty if the user pressed Enter with no input.
 */
inline std::string read_password_interactive(const char *role_name, const char *prompt)
{
#if defined(PYLABHUB_IS_POSIX)
    char *pw = ::getpass(prompt);
    if (!pw)
    {
        std::fprintf(stderr, "%s: failed to read password from terminal\n", role_name);
        return {};
    }
    return pw;
#elif defined(_WIN32)
    (void)role_name;
    HANDLE hStdin     = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD  old_mode   = 0;
    // GetConsoleMode returns 0 for non-console handles (pipes, redirected files),
    // and for INVALID_HANDLE_VALUE — is_con guards all subsequent SetConsoleMode calls.
    const bool is_con = (hStdin != INVALID_HANDLE_VALUE) &&
                        (::GetConsoleMode(hStdin, &old_mode) != 0);
    if (is_con)
        ::SetConsoleMode(hStdin, old_mode & ~ENABLE_ECHO_INPUT);
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string pw;
    std::getline(std::cin, pw);
    if (is_con)
    {
        ::SetConsoleMode(hStdin, old_mode);
        std::fprintf(stderr, "\n"); // newline since echo was suppressed
    }
    return pw;
#else
    (void)role_name;
    std::fprintf(stderr, "%s", prompt);
    std::fflush(stderr);
    std::string pw;
    std::getline(std::cin, pw);
    return pw;
#endif
}

/**
 * @brief Return the vault password from the environment or by prompting.
 *
 * Priority: PYLABHUB_ACTOR_PASSWORD env var → interactive terminal prompt.
 * Returns nullopt when stdin is not a TTY and the env var is not set; the caller
 * should treat nullopt as a fatal error (error message already printed to stderr).
 *
 * @param role_name  Used in error messages and the terminal-read failure message.
 * @param prompt     Prompt displayed to the user when falling back to interactive input.
 */
inline std::optional<std::string> get_role_password(const char *role_name, const char *prompt)
{
    if (const char *env = std::getenv("PYLABHUB_ACTOR_PASSWORD"))
        return std::string(env);
    if (!is_stdin_tty())
    {
        std::fprintf(stderr,
                     "%s: vault password required; set PYLABHUB_ACTOR_PASSWORD "
                     "for non-interactive use\n",
                     role_name);
        return std::nullopt;
    }
    return read_password_interactive(role_name, prompt);
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
