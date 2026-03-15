#pragma once
/**
 * @file script_host.hpp
 * @brief ScriptHost — abstract base for embedded scripting runtime lifecycle.
 *
 * `ScriptHost` unifies lifecycle management and thread ownership for embedded
 * scripting runtimes (Python, LuaJIT, and future engines) across all pylabhub
 * executables.
 *
 * ## Design principle
 *
 * The abstract interface owns **lifecycle and thread model only** — not invocation.
 * Concrete classes (`PythonScriptHost`, `LuaScriptHost`) expose their native-typed
 * calling conventions. Client code (`HubScript`, `ActorHost`) talks to the concrete
 * type, not to this interface.
 *
 * ## Threading model
 *
 * Two threading models are supported, selected per concrete class:
 *
 * | `owns_dedicated_thread()` | Model | Example |
 * |---|---|---|
 * | `true`  | Dedicated thread owns interpreter (GIL-style) | Python |
 * | `false` | Caller's thread owns interpreter               | LuaJIT |
 *
 * `base_startup_()` branches on this flag automatically.
 *
 * ### Threaded mode (Python)
 *
 * `base_startup_()` spawns `thread_` and blocks on `init_future_.get()`.
 * `thread_` calls `do_initialize()`, which runs the **full lifecycle** — init,
 * tick loop, and cleanup — all on that thread. When the interpreter is ready
 * (after on_start), `do_initialize()` must call `signal_ready_()` to unblock
 * `base_startup_()`. When `stop_` is set by `base_shutdown_()`, `do_initialize()`
 * exits its tick loop and returns. `thread_fn_()` then calls `do_finalize()`.
 *
 * This design allows concrete classes to hold RAII interpreter objects (e.g.,
 * `py::scoped_interpreter`) as local variables spanning the full lifetime,
 * without placing them in members that would be constructed before the runtime
 * is available.
 *
 * ### Direct mode (Lua)
 *
 * `base_startup_()` calls `do_initialize()` on the calling thread; it must return
 * promptly after calling `on_start`. `signal_ready_()` is called by `base_startup_()`
 * after `do_initialize()` returns. The caller drives the tick loop externally.
 * `base_shutdown_()` calls `do_finalize()` on the calling thread.
 *
 * ## Thread-local state
 *
 * `g_script_thread_state` is set on whichever thread initializes the runtime.
 * Concrete classes use it for:
 * - `LuaScriptHost`: validate `g_script_thread_state.owner == this` on every call
 * - `PythonScriptHost`: identify the GIL-owner thread for diagnostics
 * - Future multi-thread model: per-thread role/context tracking
 *
 * ## Lifecycle integration
 *
 * Each concrete subclass provides (by convention, cannot be virtual+static):
 * ```cpp
 * static utils::ModuleDef GetLifecycleModule();
 * ```
 * with singleton + C-style function pointer callbacks for `ModuleDef`.
 *
 * ## See also
 *
 * HEP-CORE-0011: ScriptHost Abstraction Framework (docs/HEP/)
 */

#include "pylabhub_utils_export.h"

#include <atomic>
#include <filesystem>
#include <future>
#include <thread>

namespace pylabhub::utils
{

class ScriptHost; // forward declaration for ScriptHostThreadState

// ---------------------------------------------------------------------------
// Per-thread state
// ---------------------------------------------------------------------------

/**
 * @brief Per-thread state record set by ScriptHost on its owning thread.
 *
 * Zero-initialized by default (owner = nullptr, is_interpreter_thread = false).
 * Set by ScriptHost infrastructure before `do_initialize()` is called.
 * Cleared by `do_finalize()` on the owning thread.
 *
 * Concrete classes use this for thread-identity checks and future per-thread
 * diagnostics without threading context through every call stack.
 */
struct ScriptHostThreadState
{
    ScriptHost* owner{nullptr};           ///< Which host initialized on this thread.
    bool        is_interpreter_thread{};  ///< true = this thread owns the script runtime.
};

/// Thread-local state — one instance per OS thread, zero-initialized.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
PYLABHUB_UTILS_EXPORT extern thread_local ScriptHostThreadState g_script_thread_state;

// ---------------------------------------------------------------------------
// ScriptHost abstract base
// ---------------------------------------------------------------------------

/**
 * @class ScriptHost
 * @brief Abstract base for embedded scripting runtime lifecycle.
 *
 * Subclasses implement `do_initialize()`, `do_finalize()`, and
 * `owns_dedicated_thread()`. They call `base_startup_()` / `base_shutdown_()`
 * from their public `startup_()` / `shutdown_()` lifecycle hooks.
 *
 * In threaded mode, `do_initialize()` must call `signal_ready_()` when the
 * interpreter is ready (after `on_start`), then block in a tick loop until
 * `stop_` is set, then return. `base_shutdown_()` sets `stop_` and joins.
 *
 * In direct mode, `do_initialize()` sets up the runtime, calls `on_start`,
 * and returns promptly. The caller drives the tick loop. `base_shutdown_()`
 * calls `do_finalize()` on the calling thread.
 */
class PYLABHUB_UTILS_EXPORT ScriptHost
{
public:
    ScriptHost()          = default;
    virtual ~ScriptHost() = default;

    ScriptHost(const ScriptHost&)            = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // ── Threading model ──────────────────────────────────────────────────────

    /**
     * @brief Returns true if this host requires a dedicated interpreter thread.
     *
     * - Python (GIL): `true` — `thread_` owns `py::scoped_interpreter`.
     * - Lua (no GIL): `false` — interpreter runs on the calling thread.
     */
    [[nodiscard]] virtual bool owns_dedicated_thread() const noexcept = 0;

    // ── State ─────────────────────────────────────────────────────────────────

    /**
     * @brief Thread-safe. True after `signal_ready_()` is called; false after
     *        `base_shutdown_()` completes.
     */
    [[nodiscard]] bool is_ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire);
    }

protected:
    // ── Hooks implemented by concrete class ───────────────────────────────────

    /**
     * @brief Initialize the runtime, load the script, call on_start.
     *
     * - **Threaded mode**: called on `thread_`. Must call `signal_ready_()` when
     *   the interpreter is ready, then run the tick loop until `stop_` is set.
     *   Returns only when shutting down. `thread_fn_()` calls `do_finalize()` after.
     *
     * - **Direct mode**: called on the calling thread. Must return promptly after
     *   calling `on_start`. `base_startup_()` calls `signal_ready_()` after return.
     *   `base_shutdown_()` calls `do_finalize()` later.
     *
     * @param script_path  Base script directory. Subclass appends `python/` or `lua/`.
     * @return `true` on success; `false` on fatal error (causes `base_startup_()` to throw).
     */
    virtual bool do_initialize(const std::filesystem::path& script_path) = 0;

    /**
     * @brief Release the runtime. Called after the tick loop ends or at shutdown.
     *
     * Subclass should: call `on_stop`, close the runtime (e.g., `lua_close()`),
     * and clear `g_script_thread_state` on the owning thread. Must not throw.
     */
    virtual void do_finalize() noexcept = 0;

    // ── Base machinery ────────────────────────────────────────────────────────

    /**
     * @brief Mark the interpreter as ready and unblock `base_startup_()`.
     *
     * **Threaded mode**: must be called from `do_initialize()` on `thread_`
     * after the interpreter is initialized and `on_start` has been called.
     *
     * **Direct mode**: called automatically by `base_startup_()` after
     * `do_initialize()` returns; concrete class need not call it.
     *
     * Safe to call at most once per `base_startup_()` / `base_shutdown_()` cycle.
     */
    void signal_ready_();

    /**
     * @brief Start the script host.
     *
     * Threaded mode: spawns `thread_`, blocks until `signal_ready_()` is called
     * from `do_initialize()` (or until an exception is thrown).
     *
     * Direct mode: sets thread-local state, calls `do_initialize()`, then
     * calls `signal_ready_()`.
     *
     * @throws std::runtime_error if initialization fails.
     * @param script_path  Passed through to `do_initialize()`.
     */
    void base_startup_(const std::filesystem::path& script_path);

    /**
     * @brief Stop the script host.
     *
     * Sets `stop_=true`. Threaded mode: joins `thread_`. Direct mode: calls
     * `do_finalize()` on the calling thread.
     *
     * Safe to call even if `base_startup_()` was never called. Must not throw.
     */
    void base_shutdown_() noexcept;

    /// Stop flag checked by the tick loop inside `do_initialize()` (threaded mode).
    std::atomic<bool> stop_{false};

private:
    /// Entry point for the dedicated interpreter thread (threaded mode only).
    void thread_fn_(const std::filesystem::path& script_path);

    std::atomic<bool>  ready_{false};
    std::thread        thread_;
    std::promise<void> init_promise_;
    std::future<void>  init_future_{init_promise_.get_future()};
};

} // namespace pylabhub::utils
