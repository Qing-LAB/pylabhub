/**
 * @file actor_script_host.cpp
 * @brief ActorScriptHost — drives ActorHost from a dedicated interpreter thread.
 *
 * Python lifecycle (Py_Initialize / Py_Finalize) is owned by PythonScriptHost::do_initialize().
 * This file implements do_python_work(), which runs on the interpreter thread with the GIL held
 * and performs all actor-specific Python work.
 *
 * ## GIL ownership trace (run mode)
 *
 *   Entry to do_python_work()          GIL held (by interpreter thread)
 *   → load_script()                    nested gil_scoped_acquire = no-op; GIL still held
 *   → host_->start()                   releases GIL via main_thread_release_.emplace()
 *   → signal_ready_()                  C++ only (no GIL needed)
 *   → wait loop                        GIL NOT held (worker loop_threads hold it per iteration)
 *   → host_->stop()                    re-acquires GIL via main_thread_release_.reset()
 *   → host_.reset()                    GIL held; ~ActorHost() stop() is a no-op
 *   Return from do_python_work()       GIL held (required by PythonScriptHost contract)
 *   → py::scoped_interpreter destructs Py_Finalize
 *
 * ## GIL ownership trace (validate / list-roles mode)
 *
 *   Entry to do_python_work()          GIL held
 *   → load_script()                    nested gil_scoped_acquire = no-op; GIL still held
 *   → signal_ready_()                  C++ only
 *   → host_.reset()                    GIL held; stop() in ~ActorHost() is a no-op
 *   Return from do_python_work()       GIL held ✓
 */
#include "actor_script_host.hpp"

#include "utils/logger.hpp"

#include <chrono>
#include <thread>

namespace pylabhub::actor
{

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

ActorScriptHost::~ActorScriptHost()
{
    // Ensure the interpreter thread is joined even if actor_main.cpp exits without
    // calling shutdown_() explicitly (e.g., on an early error return).
    shutdown_();
}

// ---------------------------------------------------------------------------
// Configuration setters (called before startup_())
// ---------------------------------------------------------------------------

void ActorScriptHost::set_config(ActorConfig config)
{
    config_ = std::move(config);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ActorScriptHost::startup_()
{
    // base_startup_() spawns the interpreter thread and blocks until
    // signal_ready_() is called from do_python_work() (or throws on error).
    base_startup_({});
}

void ActorScriptHost::shutdown_() noexcept
{
    base_shutdown_();
}

void ActorScriptHost::signal_shutdown() noexcept
{
    // Forward to ActorHost so the existing wait_for_shutdown() mechanism is
    // triggered. Also set stop_ in case host_ is not yet created (early signal).
    if (host_)
        host_->signal_shutdown();
    stop_.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// do_python_work — called by PythonScriptHost::do_initialize() on interpreter thread.
//
// Preconditions:
//   - py::scoped_interpreter is live (Py_Initialize done).
//   - GIL is held on entry.
//   - stop_ may already be set if a signal fired before the thread started.
//
// Postconditions:
//   - GIL is held on return (PythonScriptHost contract).
//   - All py::object instances held by ActorHost are released (via host_.reset()).
//   - Returns normally (all exceptions handled internally or by ActorHost).
// ---------------------------------------------------------------------------

void ActorScriptHost::do_python_work(const std::filesystem::path& /*script_path*/)
{
    // GIL held on entry.

    // ── Early exit: signal fired before interpreter thread ran ────────────────
    if (stop_.load(std::memory_order_acquire))
    {
        script_load_ok_ = false;
        signal_ready_(); // must signal even on failure so startup_() unblocks
        return;          // GIL held ✓
    }

    // ── Create actor host ────────────────────────────────────────────────────
    host_ = std::make_unique<ActorHost>(config_);

    // ── Load role scripts ────────────────────────────────────────────────────
    // load_script() calls py::gil_scoped_acquire internally.  Since the GIL is
    // already held by this thread, that acquire becomes a nested no-op; the GIL
    // remains held when load_script() returns.
    const bool verbose = validate_only_ || list_roles_;
    script_load_ok_    = host_->load_script(verbose);

    if (!script_load_ok_)
    {
        // Failed to load any script module.  Signal ready so startup_() unblocks
        // promptly; actor_main.cpp will check script_load_ok() and exit.
        signal_ready_();
        host_.reset(); // GIL held; ~ActorHost()::stop() is a no-op
        return;        // GIL held ✓
    }

    // ── Validate / list-roles modes: early exit after script load ─────────────
    // load_script(verbose=true) already printed the layout / role summary.
    if (list_roles_ || validate_only_)
    {
        signal_ready_();
        host_.reset(); // GIL held ✓
        return;        // GIL held ✓
    }

    // ── Start role workers ────────────────────────────────────────────────────
    // start() emplaces main_thread_release_ (releases the GIL) so that worker
    // loop_threads can acquire the GIL for their Python callbacks.
    has_active_roles_ = host_->start();
    // GIL NOT held after start().

    // Signal ready.  startup_() unblocks; actor_main.cpp's wait loop begins.
    signal_ready_();

    if (!has_active_roles_)
    {
        LOGGER_INFO("[actor] ActorScriptHost: no roles activated");
        // stop() re-acquires GIL and cleans up (even with empty worker maps).
        host_->stop();  // GIL held after stop()
        host_.reset();
        return; // GIL held ✓
    }

    // ── Wait for shutdown ────────────────────────────────────────────────────
    // GIL not held during the wait loop.
    // Two sources of shutdown:
    //   a) stop_ set by base_shutdown_() — main thread detected g_shutdown
    //   b) host_->is_shutdown_requested() — api.stop() fired from a Python callback
    while (!stop_.load(std::memory_order_acquire) &&
           !host_->is_shutdown_requested())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // If internal shutdown (api.stop()), propagate to the main thread's wait loop.
    if (host_->is_shutdown_requested() && g_shutdown_)
        g_shutdown_->store(true, std::memory_order_release);

    // ── Stop role workers ─────────────────────────────────────────────────────
    // stop() re-acquires GIL (via main_thread_release_.reset()), then joins all
    // worker threads (releasing/re-acquiring GIL around each join), and calls
    // on_stop per role.
    host_->stop();  // GIL held after stop()
    host_.reset();  // ~ActorHost()::stop() is a no-op; GIL still held

    LOGGER_INFO("[actor] ActorScriptHost: all roles stopped; returning to PythonScriptHost");
    // Return with GIL held ✓
    // py::scoped_interpreter destructor follows: Py_Finalize
}

} // namespace pylabhub::actor
