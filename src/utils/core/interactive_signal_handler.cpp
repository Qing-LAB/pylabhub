/**
 * @file interactive_signal_handler.cpp
 * @brief Implementation of InteractiveSignalHandler.
 *
 * Cross-platform design:
 * - Signal handler only sets atomics + wakes watcher thread (async-signal-safe).
 * - Watcher thread handles all I/O (print status, read stdin, print resume).
 * - POSIX: self-pipe for wakeup + poll() for stdin-with-timeout.
 * - Windows: manual-reset event for wakeup + WaitForMultipleObjects for stdin.
 */

#include "utils/interactive_signal_handler.hpp"
#include "utils/logger.hpp"

#include <fmt/format.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#if defined(PYLABHUB_IS_POSIX)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#elif defined(PYLABHUB_IS_WINDOWS)
#include <io.h>
#include <windows.h>
#endif

namespace pylabhub
{

// ============================================================================
// File-static globals — accessed from the async signal handler.
// Only one InteractiveSignalHandler may be active per process.
// ============================================================================

static std::atomic<int>  g_signal_count{0};
static std::atomic<bool> g_force_exit{false};

#if defined(PYLABHUB_IS_POSIX)
static int g_wake_pipe[2] = {-1, -1}; // [0]=read, [1]=write
#elif defined(PYLABHUB_IS_WINDOWS)
static HANDLE g_wake_event = nullptr;
#endif

// ============================================================================
// Async-signal-safe signal handler
// ============================================================================

static void signal_handler_impl(int /*sig*/) noexcept
{
    const int count = g_signal_count.fetch_add(1, std::memory_order_relaxed) + 1;

    if (count >= 3)
    {
        // Third signal — force exit immediately.
        std::_Exit(1);
    }

    // Wake the watcher thread.
#if defined(PYLABHUB_IS_POSIX)
    if (g_wake_pipe[1] >= 0)
    {
        char c = 'S';
        (void)::write(g_wake_pipe[1], &c, 1); // async-signal-safe
    }
#elif defined(PYLABHUB_IS_WINDOWS)
    if (g_wake_event != nullptr)
        ::SetEvent(g_wake_event);
#endif
}

// ============================================================================
// Impl
// ============================================================================

struct InteractiveSignalHandler::Impl
{
    SignalHandlerConfig    config;
    std::atomic<bool>     *shutdown_flag{nullptr};
    SignalStatusCallback   status_cb;
    std::mutex             cb_mutex;
    std::thread            watcher_thread;
    std::atomic<bool>      installed{false};
    bool                   interactive{false};

    // Saved previous signal dispositions.
    void (*prev_sigint)(int)  = SIG_DFL;
    void (*prev_sigterm)(int) = SIG_DFL;

    // ── Platform init/cleanup ──────────────────────────────────────────────

    static void platform_init()
    {
#if defined(PYLABHUB_IS_POSIX)
#if defined(__linux__)
        if (::pipe2(g_wake_pipe, O_NONBLOCK | O_CLOEXEC) != 0)
        {
            g_wake_pipe[0] = g_wake_pipe[1] = -1;
        }
#else
        // macOS / other POSIX: plain pipe + manual flags
        if (::pipe(g_wake_pipe) == 0)
        {
            for (int i = 0; i < 2; ++i)
            {
                int flags = ::fcntl(g_wake_pipe[i], F_GETFL);
                if (flags >= 0)
                    (void)::fcntl(g_wake_pipe[i], F_SETFL, flags | O_NONBLOCK);
                flags = ::fcntl(g_wake_pipe[i], F_GETFD);
                if (flags >= 0)
                    (void)::fcntl(g_wake_pipe[i], F_SETFD, flags | FD_CLOEXEC);
            }
        }
        else
        {
            g_wake_pipe[0] = g_wake_pipe[1] = -1;
        }
#endif
#elif defined(PYLABHUB_IS_WINDOWS)
        g_wake_event = ::CreateEventA(nullptr, TRUE, FALSE, nullptr); // manual-reset
#endif
    }

    static void platform_cleanup()
    {
#if defined(PYLABHUB_IS_POSIX)
        if (g_wake_pipe[0] >= 0)
        {
            ::close(g_wake_pipe[0]);
        }
        if (g_wake_pipe[1] >= 0)
        {
            ::close(g_wake_pipe[1]);
        }
        g_wake_pipe[0] = g_wake_pipe[1] = -1;
#elif defined(PYLABHUB_IS_WINDOWS)
        if (g_wake_event != nullptr)
        {
            ::CloseHandle(g_wake_event);
            g_wake_event = nullptr;
        }
#endif
    }

    // ── TTY detection ──────────────────────────────────────────────────────

    static bool detect_tty()
    {
#if defined(PYLABHUB_IS_POSIX)
        return (::isatty(STDIN_FILENO) != 0) && (::isatty(STDERR_FILENO) != 0);
#elif defined(PYLABHUB_IS_WINDOWS)
        return (::GetFileType(::GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR) &&
               (::GetFileType(::GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR);
#else
        return false;
#endif
    }

    // ── Watcher thread entry ───────────────────────────────────────────────

    void watcher_loop()
    {
        while (!g_force_exit.load(std::memory_order_relaxed))
        {
            // Block until woken by signal handler.
            if (!wait_for_signal(/* timeout_ms = */ 500))
            {
                continue; // Periodic check of g_force_exit.
            }

            const int count = g_signal_count.load(std::memory_order_acquire);
            if (count <= 0)
            {
                continue; // Spurious wake.
            }

            if (!interactive || count >= 2)
            {
                // Non-interactive, or double-signal: immediate shutdown.
                do_shutdown(count >= 2
                    ? "Shutdown confirmed (double interrupt)."
                    : fmt::format("{}: signal received, shutting down.",
                                  config.binary_name));
                return;
            }

            // If the application has already initiated internal shutdown (e.g.
            // api.stop() was called from a script), skip the interactive prompt
            // and complete the shutdown silently.  This prevents a 5-second hang
            // when a SIGTERM arrives during lifecycle teardown.
            if (shutdown_flag != nullptr &&
                shutdown_flag->load(std::memory_order_acquire))
            {
                do_shutdown(fmt::format("{}: shutdown already in progress.",
                                       config.binary_name));
                return;
            }

            // Interactive path: print status + prompt.
            print_interrupted();

            auto result = prompt_with_timeout();
            if (result == PromptResult::Confirm || result == PromptResult::DoubleInterrupt)
            {
                do_shutdown("Shutdown confirmed.");
                return;
            }

            // Timeout or decline: resume.
            if (result == PromptResult::Timeout)
            {
                fmt::print(stderr, "No answer for {}s — resuming operation.\n",
                           config.timeout_s);
            }
            else
            {
                fmt::print(stderr, "Resuming operation.\n");
            }

            LOGGER_INFO("{}: Ctrl-C prompt — user chose to resume.", config.binary_name);

            // Reset so next Ctrl-C starts fresh.
            g_signal_count.store(0, std::memory_order_relaxed);
            drain_pipe();
        }
    }

    // ── Helpers ────────────────────────────────────────────────────────────

    void do_shutdown(const std::string &msg)
    {
        fmt::print(stderr, "{}\n", msg);
        LOGGER_INFO("{}: {}", config.binary_name, msg);
        if (shutdown_flag != nullptr)
        {
            shutdown_flag->store(true, std::memory_order_release);
        }
    }

    void print_interrupted()
    {
        // Timestamp.
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#if defined(PYLABHUB_IS_POSIX)
        ::localtime_r(&tt, &tm_buf);
#elif defined(PYLABHUB_IS_WINDOWS)
        ::localtime_s(&tm_buf, &tt);
#endif
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

        fmt::print(stderr, "\n[{}] {} interrupted\n\n", ts, config.binary_name);

        // Call the status callback.
        std::string status;
        {
            std::lock_guard<std::mutex> lk(cb_mutex);
            if (status_cb)
            {
                try
                {
                    status = status_cb();
                }
                catch (...)
                {
                    status = "  (status callback threw an exception)";
                }
            }
        }
        if (!status.empty())
        {
            fmt::print(stderr, "{}\n\n", status);
        }
    }

    enum class PromptResult { Confirm, Decline, Timeout, DoubleInterrupt };

    PromptResult prompt_with_timeout()
    {
        fmt::print(stderr, "Shut down? (y/[n], {}s timeout) ", config.timeout_s);
        std::fflush(stderr);

        const int timeout_ms = config.timeout_s * 1000;
        return read_stdin_with_wake(timeout_ms);
    }

    // ── Platform-specific blocking ─────────────────────────────────────────

    /// Wait for the wake pipe/event. Returns true if woken, false on timeout.
    static bool wait_for_signal(int timeout_ms)
    {
#if defined(PYLABHUB_IS_POSIX)
        if (g_wake_pipe[0] < 0)
        {
            // Fallback: just poll the atomic.
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return g_signal_count.load(std::memory_order_acquire) > 0;
        }
        struct pollfd fds[1] = {{g_wake_pipe[0], POLLIN, 0}};
        int ret = ::poll(fds, 1, timeout_ms);
        if (ret > 0)
        {
            // Drain the pipe byte.
            char buf[16];
            (void)::read(g_wake_pipe[0], buf, sizeof(buf));
            return true;
        }
        return false;
#elif defined(PYLABHUB_IS_WINDOWS)
        if (g_wake_event == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return g_signal_count.load(std::memory_order_acquire) > 0;
        }
        DWORD ret = ::WaitForSingleObject(g_wake_event,
                                           static_cast<DWORD>(timeout_ms));
        if (ret == WAIT_OBJECT_0)
        {
            ::ResetEvent(g_wake_event);
            return true;
        }
        return false;
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return g_signal_count.load(std::memory_order_acquire) > 0;
#endif
    }

    /// Read stdin with a timeout, also watching the wake pipe for a second signal.
    static PromptResult read_stdin_with_wake(int timeout_ms)
    {
#if defined(PYLABHUB_IS_POSIX)
        struct pollfd fds[2] = {
            {STDIN_FILENO,    POLLIN, 0},
            {g_wake_pipe[0],  POLLIN, 0},
        };
        const int nfds = (g_wake_pipe[0] >= 0) ? 2 : 1;
        int ret = ::poll(fds, static_cast<nfds_t>(nfds), timeout_ms);

        if (ret <= 0)
        {
            return PromptResult::Timeout;
        }

        // Check wake pipe first — another Ctrl-C arrived during prompt.
        if (nfds == 2 && ((fds[1].revents & POLLIN) != 0))
        {
            char buf[16];
            (void)::read(g_wake_pipe[0], buf, sizeof(buf));
            return PromptResult::DoubleInterrupt;
        }

        // Check stdin.
        if ((fds[0].revents & POLLIN) != 0)
        {
            char buf[16];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0 && (buf[0] == 'y' || buf[0] == 'Y'))
            {
                return PromptResult::Confirm;
            }
            return PromptResult::Decline;
        }

        return PromptResult::Timeout;

#elif defined(PYLABHUB_IS_WINDOWS)
        HANDLE handles[2];
        DWORD handle_count = 0;

        HANDLE hStdin = ::GetStdHandle(STD_INPUT_HANDLE);
        if (hStdin != INVALID_HANDLE_VALUE && hStdin != nullptr)
            handles[handle_count++] = hStdin;
        if (g_wake_event != nullptr)
            handles[handle_count++] = g_wake_event;

        if (handle_count == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return PromptResult::Timeout;
        }

        DWORD ret = ::WaitForMultipleObjects(
            handle_count, handles, FALSE, static_cast<DWORD>(timeout_ms));

        if (ret == WAIT_TIMEOUT)
            return PromptResult::Timeout;

        // Determine which handle fired.
        DWORD idx = ret - WAIT_OBJECT_0;
        if (idx < handle_count && handles[idx] == g_wake_event)
        {
            ::ResetEvent(g_wake_event);
            return PromptResult::DoubleInterrupt;
        }

        if (idx < handle_count && handles[idx] == hStdin)
        {
            // Read console input.
            INPUT_RECORD rec;
            DWORD count = 0;
            if (::ReadConsoleInput(hStdin, &rec, 1, &count) && count > 0)
            {
                if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
                {
                    char ch = rec.Event.KeyEvent.uChar.AsciiChar;
                    if (ch == 'y' || ch == 'Y')
                        return PromptResult::Confirm;
                    return PromptResult::Decline;
                }
            }
            return PromptResult::Decline;
        }

        return PromptResult::Timeout;
#else
        // Generic fallback: no way to read stdin with timeout.
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return PromptResult::Timeout;
#endif
    }

    static void drain_pipe()
    {
#if defined(PYLABHUB_IS_POSIX)
        if (g_wake_pipe[0] >= 0)
        {
            char buf[64];
            while (::read(g_wake_pipe[0], buf, sizeof(buf)) > 0)
            { }
        }
#elif defined(PYLABHUB_IS_WINDOWS)
        if (g_wake_event != nullptr)
        {
            ::ResetEvent(g_wake_event);
        }
#endif
    }
};

// ============================================================================
// Public API
// ============================================================================

InteractiveSignalHandler::InteractiveSignalHandler(
    SignalHandlerConfig config,
    std::atomic<bool>  *shutdown_flag)
    : impl_(std::make_unique<Impl>())
{
    impl_->config        = std::move(config);
    impl_->shutdown_flag = shutdown_flag;
}

InteractiveSignalHandler::~InteractiveSignalHandler()
{
    if (impl_ && impl_->installed.load())
        uninstall();
}

void InteractiveSignalHandler::set_status_callback(SignalStatusCallback cb)
{
    std::lock_guard<std::mutex> lk(impl_->cb_mutex);
    impl_->status_cb = std::move(cb);
}

void InteractiveSignalHandler::install()
{
    if (impl_->installed.exchange(true))
    {
        return; // Already installed.
    }

    // Determine interactive mode.
    if (impl_->config.force_daemon)
    {
        impl_->interactive = false;
    }
    else if (impl_->config.force_interactive)
    {
        impl_->interactive = true;
    }
    else
    {
        impl_->interactive = Impl::detect_tty();
    }

    // Reset globals.
    g_signal_count.store(0, std::memory_order_relaxed);
    g_force_exit.store(false, std::memory_order_relaxed);

    // Platform init (pipe / event).
    impl_->platform_init();

    // Install signal handlers.
    impl_->prev_sigint  = std::signal(SIGINT,  signal_handler_impl);
    impl_->prev_sigterm = std::signal(SIGTERM, signal_handler_impl);

    // Start watcher thread.
    impl_->watcher_thread = std::thread([this]() { impl_->watcher_loop(); });
}

void InteractiveSignalHandler::uninstall()
{
    if (!impl_->installed.exchange(false))
    {
        return;
    }

    // Signal the watcher thread to exit.
    g_force_exit.store(true, std::memory_order_release);

    // Wake the watcher thread so it checks g_force_exit.
#if defined(PYLABHUB_IS_POSIX)
    if (g_wake_pipe[1] >= 0)
    {
        char c = 'Q';
        (void)::write(g_wake_pipe[1], &c, 1);
    }
#elif defined(PYLABHUB_IS_WINDOWS)
    if (g_wake_event != nullptr)
    {
        ::SetEvent(g_wake_event);
    }
#endif

    if (impl_->watcher_thread.joinable())
    {
        impl_->watcher_thread.join();
    }

    // Restore previous signal handlers.
    if (impl_->prev_sigint != SIG_ERR)
    {
        std::signal(SIGINT, impl_->prev_sigint);
    }
    if (impl_->prev_sigterm != SIG_ERR)
    {
        std::signal(SIGTERM, impl_->prev_sigterm);
    }

    impl_->platform_cleanup();
}

bool InteractiveSignalHandler::is_installed() const noexcept
{
    return impl_->installed.load(std::memory_order_relaxed);
}

// ============================================================================
// Lifecycle module support
// ============================================================================

// Process-global pointer for the C-style lifecycle callback.
// Only one InteractiveSignalHandler may be active per process (one SIGINT
// handler is the OS-level reality).  Lifecycle's duplicate-registration
// rejection (LifecycleManager::register_module returns false on duplicate
// name) prevents a second "SignalHandler" module from being registered,
// so this pointer is written exactly once per process lifetime.
static InteractiveSignalHandler *s_lifecycle_instance = nullptr;

static void signal_handler_lifecycle_cleanup(const char * /*unused*/, void * /*userdata*/) noexcept
{
    if (s_lifecycle_instance != nullptr)
    {
        s_lifecycle_instance->uninstall();
    }
}

utils::ModuleDef InteractiveSignalHandler::make_lifecycle_module()
{
    s_lifecycle_instance = this;
    utils::ModuleDef def("SignalHandler");
    // No startup callback — caller calls install() explicitly before registering.
    // Timeout slightly exceeds the watcher thread's interactive prompt timeout.
    def.set_shutdown(signal_handler_lifecycle_cleanup,
                     std::chrono::milliseconds(7000));
    def.set_as_persistent(true);
    return def;
}

} // namespace pylabhub
