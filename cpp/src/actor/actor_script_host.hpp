#pragma once
/**
 * @file actor_script_host.hpp
 * @brief ActorScriptHost — PythonScriptHost subclass for the actor process.
 *
 * Moves the CPython interpreter from a stack-allocated `py::scoped_interpreter`
 * on the main thread to a dedicated interpreter thread via `PythonScriptHost`,
 * following the same pattern as `HubScript`.
 *
 * ## Lifecycle
 *
 * ```
 * actor_main.cpp:
 *   actor_script.set_config(config);
 *   actor_script.set_shutdown_flag(&g_shutdown);
 *   actor_script.startup_();          // spawns interpreter thread; blocks until ready
 *   // ... check script_load_ok() / has_active_roles() ...
 *   // ... wait loop on g_shutdown ...
 *   actor_script.shutdown_();         // sets stop_, joins interpreter thread
 * ```
 *
 * ## Thread model
 *
 * Interpreter thread (spawned by PythonScriptHost):
 *   1. `Py_Initialize` (via `py::scoped_interpreter` in `do_initialize`)
 *   2. `ActorHost::load_script()` — imports role Python packages (GIL held)
 *   3. `ActorHost::start()` — starts role workers; releases GIL via
 *      `main_thread_release_.emplace()` so worker loop_threads can acquire it
 *   4. `signal_ready_()` — unblocks `startup_()` on the main thread
 *   5. Wait loop until `stop_` (set by `shutdown_()`) or internal shutdown
 *   6. `ActorHost::stop()` — re-acquires GIL, joins workers, calls on_stop
 *   7. `Py_Finalize` (via `py::scoped_interpreter` destructor)
 *
 * ## See also
 *
 * HEP-CORE-0011: ScriptHost Abstraction Framework (docs/HEP/)
 */

#include "actor_config.hpp"
#include "actor_host.hpp"

#include "python_script_host.hpp"

#include <atomic>
#include <filesystem>
#include <memory>

namespace pylabhub::actor
{

/**
 * @class ActorScriptHost
 * @brief PythonScriptHost subclass that owns the actor's Python interpreter
 *        lifetime and drives all role workers.
 *
 * Constructed and managed manually in actor_main.cpp (not a LifecycleGuard
 * module), analogous to how HubScript is managed in hubshell.cpp.
 */
class ActorScriptHost : public scripting::PythonScriptHost
{
  public:
    ActorScriptHost() = default;

    /**
     * @brief Shutdown interpreter thread on destruction if not already done.
     * Ensures the thread is joined even on early-exit paths in actor_main.cpp.
     */
    ~ActorScriptHost() override;

    ActorScriptHost(const ActorScriptHost&)            = delete;
    ActorScriptHost& operator=(const ActorScriptHost&) = delete;

    // ── Pre-startup configuration ──────────────────────────────────────────

    /** Set actor config (call before startup_()). */
    void set_config(ActorConfig config);

    /** If true, do_python_work() will print validation info then exit early. */
    void set_validate_only(bool v) noexcept { validate_only_ = v; }

    /** If true, do_python_work() will print role summary then exit early. */
    void set_list_roles(bool v) noexcept { list_roles_ = v; }

    /**
     * @brief Pointer to the global shutdown flag in actor_main.cpp.
     *
     * When the actor script calls `api.stop()` (internal shutdown), the
     * interpreter thread sets this flag to wake up the main thread's wait loop.
     */
    void set_shutdown_flag(std::atomic<bool>* flag) noexcept { g_shutdown_ = flag; }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /**
     * @brief Start the interpreter thread.
     *
     * Blocks until do_python_work() calls signal_ready_() (after scripts are
     * loaded and roles are started), then returns.
     *
     * @throws std::runtime_error  if interpreter initialization fails.
     */
    void startup_();

    /**
     * @brief Stop the interpreter thread and wait for it to finish.
     *
     * Sets stop_, wakes the interpreter thread's wait loop, and joins.
     * Idempotent — safe to call multiple times.
     */
    void shutdown_() noexcept;

    /**
     * @brief Signal internal shutdown. Called from the signal handler.
     *
     * Forwards to ActorHost::signal_shutdown() so the wait loop in
     * do_python_work() detects the shutdown request.
     */
    void signal_shutdown() noexcept;

    // ── Post-startup results ───────────────────────────────────────────────

    /** True if load_script() succeeded. Available after startup_() returns. */
    [[nodiscard]] bool script_load_ok() const noexcept { return script_load_ok_; }

    /** True if at least one role was activated. Available after startup_() returns. */
    [[nodiscard]] bool has_active_roles() const noexcept { return has_active_roles_; }

  protected:
    void do_python_work(const std::filesystem::path& script_path) override;

  private:
    ActorConfig                config_;
    bool                       validate_only_{false};
    bool                       list_roles_{false};
    bool                       script_load_ok_{false};
    bool                       has_active_roles_{false};
    std::unique_ptr<ActorHost> host_;
    std::atomic<bool>*         g_shutdown_{nullptr};
};

} // namespace pylabhub::actor
