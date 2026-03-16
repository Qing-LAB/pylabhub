/**
 * @file script_host.cpp
 * @brief ScriptHost base class implementation — thread lifecycle + thread_local state.
 */
#include "utils/script_host.hpp"
#include "utils/logger.hpp"

namespace pylabhub::utils
{

// ---------------------------------------------------------------------------
// Thread-local state definition
// ---------------------------------------------------------------------------

ScriptHostThreadState &g_script_thread_state() noexcept
{
    static thread_local ScriptHostThreadState instance;
    return instance;
}

// ---------------------------------------------------------------------------
// ScriptHost::signal_ready_
// ---------------------------------------------------------------------------

void ScriptHost::signal_ready_()
{
    ready_.store(true, std::memory_order_release);
    try
    {
        init_promise_.set_value();
    }
    catch (const std::future_error&)
    {
        // Already signaled — no-op (idempotent guard).
    }
}

// ---------------------------------------------------------------------------
// ScriptHost::thread_fn_ — runs on dedicated thread (threaded mode only)
// ---------------------------------------------------------------------------

void ScriptHost::thread_fn_(const std::filesystem::path& script_path)
{
    g_script_thread_state().owner                = this;
    g_script_thread_state().is_interpreter_thread = true;

    try
    {
        const bool ok = do_initialize(script_path);
        // do_initialize() must have called signal_ready_() before returning.
        // If it returned false (fatal error) without signaling, signal exception now.
        if (!ok && !ready_.load(std::memory_order_acquire))
        {
            try
            {
                init_promise_.set_exception(std::make_exception_ptr(
                    std::runtime_error("ScriptHost: do_initialize() failed")));
            }
            catch (const std::future_error&) {} // already signaled
        }
    }
    catch (...)
    {
        if (!ready_.load(std::memory_order_acquire))
        {
            try { init_promise_.set_exception(std::current_exception()); }
            catch (const std::future_error&) {}
        }
        else
        {
            LOGGER_ERROR("ScriptHost: unhandled exception in interpreter thread after init");
        }
    }

    // Finalize: called after do_initialize() returns (tick loop ended or error).
    if (ready_.load(std::memory_order_acquire))
    {
        ready_.store(false, std::memory_order_release);
        do_finalize();
    }

    // Clear thread-local state.
    g_script_thread_state() = {};
}

// ---------------------------------------------------------------------------
// ScriptHost::base_startup_
// ---------------------------------------------------------------------------

void ScriptHost::base_startup_(const std::filesystem::path& script_path)
{
    if (owns_dedicated_thread())
    {
        thread_ = std::thread([this, script_path] { thread_fn_(script_path); });
        init_future_.get(); // blocks; rethrows any exception set by thread_fn_
    }
    else
    {
        // Direct mode: caller's thread owns the runtime.
        g_script_thread_state().owner                = this;
        g_script_thread_state().is_interpreter_thread = true;

        const bool ok = do_initialize(script_path);
        if (!ok)
        {
            g_script_thread_state() = {};
            throw std::runtime_error("ScriptHost: do_initialize() failed");
        }

        signal_ready_(); // base signals ready for direct mode (do_initialize returns quickly)
    }
}

// ---------------------------------------------------------------------------
// ScriptHost::base_shutdown_
// ---------------------------------------------------------------------------

void ScriptHost::base_shutdown_() noexcept
{
    stop_.store(true, std::memory_order_release);

    if (owns_dedicated_thread())
    {
        if (thread_.joinable())
            thread_.join();
        // ready_ and thread_local cleared by thread_fn_ on the interpreter thread.
    }
    else
    {
        // Direct mode: finalize on calling thread.
        if (ready_.load(std::memory_order_acquire))
        {
            ready_.store(false, std::memory_order_release);
            do_finalize(); // calls on_stop, closes runtime, clears g_script_thread_state
        }
    }
}

} // namespace pylabhub::utils
