/**
 * @file interactive_signal_handler.hpp
 * @brief Jupyter-style interactive Ctrl-C handler with status display and confirmation.
 *
 * On SIGINT/SIGTERM:
 * - **Interactive (TTY)**: prints status callback output, prompts "Shut down? (y/[n])",
 *   waits for timeout_s seconds. On 'y' → graceful shutdown; on timeout/other → resume.
 *   Second signal during prompt → immediate shutdown. Third signal → _Exit(1).
 * - **Non-interactive (no TTY)**: immediate graceful shutdown on first signal;
 *   second signal → _Exit(1).
 *
 * ## Cross-platform
 *
 * - POSIX: self-pipe for async-signal-safe wakeup + poll() for stdin timeout
 * - Windows: SetEvent for wakeup + WaitForMultipleObjects for stdin timeout
 *
 * ## Usage
 *
 *     std::atomic<bool> shutdown{false};
 *     InteractiveSignalHandler handler({"pylabhub-producer", 5}, &shutdown);
 *     handler.set_status_callback([&]() { return "  UID: PROD-FOO\n  ..."; });
 *     handler.install();
 *     while (!shutdown.load()) sleep(100ms);
 *     handler.uninstall();
 *
 * @see docs/HEP/HEP-CORE-0020-Interactive-Signal-Handler.md
 */
#pragma once

#include "plh_platform.hpp" // PYLABHUB_UTILS_EXPORT

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace pylabhub
{

/// Status callback — returns a multi-line string to display on Ctrl-C.
/// Called on the watcher thread; may read atomics and config but must not block.
using SignalStatusCallback = std::function<std::string()>;

/// Configuration for the interactive signal handler.
struct SignalHandlerConfig
{
    std::string binary_name;             ///< e.g. "pylabhub-producer"
    int         timeout_s       = 5;     ///< Prompt timeout (seconds); 0 = no prompt
    bool        force_interactive = false; ///< Override TTY auto-detect (always prompt)
    bool        force_daemon      = false; ///< Force non-interactive (never prompt)
};

/// Interactive Ctrl-C handler with Jupyter-style prompt.
///
/// Lifecycle: one instance per process. Must outlive the main loop.
/// The class installs SIGINT and SIGTERM handlers on install() and restores
/// default disposition on uninstall().
class PYLABHUB_UTILS_EXPORT InteractiveSignalHandler
{
public:
    InteractiveSignalHandler(SignalHandlerConfig config,
                             std::atomic<bool>  *shutdown_flag);
    ~InteractiveSignalHandler();

    InteractiveSignalHandler(const InteractiveSignalHandler &) = delete;
    InteractiveSignalHandler &operator=(const InteractiveSignalHandler &) = delete;

    /// Register the status callback (before or after install).
    void set_status_callback(SignalStatusCallback cb);

    /// Install signal handlers and start the watcher thread.
    void install();

    /// Stop the watcher thread and restore default signal disposition.
    void uninstall();

    /// True if install() has been called and uninstall() has not.
    bool is_installed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub
