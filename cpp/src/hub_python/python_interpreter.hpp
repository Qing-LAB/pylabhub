#pragma once
/**
 * @file python_interpreter.hpp
 * @brief PythonInterpreter — embedded Python singleton lifecycle placeholder.
 *
 * In the new design **`HubScript::hub_thread_` owns the CPython interpreter**.
 * This module is kept as a static lifecycle registration point so that
 * `AdminShell` can declare it as a dependency (preserving startup ordering),
 * and to provide the shared `exec()` / `reset_namespace()` services that the
 * admin shell uses.
 *
 * ## Interpreter ownership
 *
 * | Who           | What                                                        |
 * |---------------|-------------------------------------------------------------|
 * | `HubScript`   | Creates `py::scoped_interpreter` on `hub_thread_` → owns   |
 *                 | `Py_Initialize` + `Py_Finalize`.                            |
 * | `PythonInterpreter` | Owns the persistent `__main__` namespace (set up by   |
 *                 | `init_namespace_()`, torn down by `release_namespace_()`).  |
 *
 * ## Lifecycle
 *
 * `startup_()` and `shutdown_()` are **no-ops** — they exist only so the
 * `LifecycleManager` can order `AdminShell` after `PythonInterpreter`.
 * Actual Python initialization is performed by `HubScript::hub_thread_fn_()`,
 * which calls `init_namespace_()` after `Py_Initialize` and
 * `release_namespace_()` before `Py_Finalize`.
 *
 * ## Thread safety
 *
 * - `exec()` serialises concurrent callers via a mutex; acquires the GIL
 *   internally. Returns an error if the interpreter is not yet ready
 *   (`is_ready()` == false).
 * - `get_instance()` is safe from any thread.
 * - `reset_namespace()` is safe from any thread after `init_namespace_()`.
 */

#include "plh_service.hpp"

#include <functional>
#include <memory>
#include <string>

namespace pylabhub
{

/**
 * @brief Result returned by `PythonInterpreter::exec()`.
 */
struct PyExecResult
{
    bool        success{false};  ///< True if no exception was raised.
    std::string output;          ///< Captured stdout + stderr during execution.
    std::string error;           ///< Exception message or traceback (empty on success).
    std::string result_repr; ///< Not yet implemented; always empty. Reserved for future
                             ///< AdminShell exec path (Python repr of last expression).
};

/**
 * @class PythonInterpreter
 * @brief Singleton lifecycle module that manages the persistent Python namespace
 *        and exposes `exec()` to the admin shell.
 */
class PythonInterpreter
{
  public:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /** Returns the ModuleDef for use with LifecycleGuard. startup/shutdown are no-ops. */
    static utils::ModuleDef GetLifecycleModule();

    /** Returns the global singleton instance. Safe from any thread. */
    static PythonInterpreter& get_instance();

    // -----------------------------------------------------------------------
    // Shutdown callback
    // -----------------------------------------------------------------------

    /**
     * @brief Registers a callback invoked when Python code calls `pylabhub.shutdown()`.
     *
     * Call this from hubshell before starting the main loop.
     * The callback is invoked from whichever thread calls exec() with shutdown code;
     * it should only set a flag — do not join threads or do heavy work inside it.
     */
    static void set_shutdown_callback(std::function<void()> cb);

    /** Invokes the registered shutdown callback (called by the pybind11 module). */
    static void request_shutdown();

    // -----------------------------------------------------------------------
    // Python execution
    // -----------------------------------------------------------------------

    /**
     * @brief Executes Python source code in the persistent namespace.
     *
     * Thread-safe: serialises callers with a mutex; acquires the GIL internally.
     * stdout and stderr are redirected to a StringIO buffer for the duration of
     * the call and returned in `PyExecResult::output`.
     *
     * Returns an error result immediately if the interpreter is not yet ready
     * (HubScript::hub_thread_ has not yet called init_namespace_()).
     *
     * @param code  Python source code to execute (may be multi-line).
     * @return      Execution result with captured output and error info.
     */
    PyExecResult exec(const std::string& code);

    /**
     * @brief Clears all user-defined names from the persistent namespace.
     *
     * Built-in names and imported modules that were set up at startup are
     * preserved. Thread-safe.  No-op if interpreter is not ready.
     */
    void reset_namespace();

    /**
     * @brief Returns true if the Python interpreter is ready and exec() may be called.
     *
     * Becomes true after HubScript::hub_thread_fn_() calls init_namespace_(),
     * and false again after it calls release_namespace_().
     */
    [[nodiscard]] bool is_ready() const noexcept;

    // -----------------------------------------------------------------------
    // Non-copyable, non-movable singleton
    // -----------------------------------------------------------------------
    PythonInterpreter(const PythonInterpreter&) = delete;
    PythonInterpreter& operator=(const PythonInterpreter&) = delete;
    PythonInterpreter(PythonInterpreter&&) = delete;
    PythonInterpreter& operator=(PythonInterpreter&&) = delete;

    // -----------------------------------------------------------------------
    // Internal lifecycle hooks (public so anonymous-namespace startup/shutdown
    // functions defined in the .cpp can call them; not part of the public API)
    // -----------------------------------------------------------------------
    /// @internal Called by lifecycle startup function (no-op in new design).
    void startup_();
    /// @internal Called by lifecycle shutdown function (no-op in new design).
    void shutdown_();

    // -----------------------------------------------------------------------
    // Internal interpreter management (called by HubScript::hub_thread_fn_())
    // -----------------------------------------------------------------------

    /**
     * @brief Sets up the persistent `__main__` namespace and marks the interpreter ready.
     *
     * **Must be called with the GIL held** from `HubScript::hub_thread_fn_()`,
     * immediately after `py::scoped_interpreter` is constructed (Py_Initialize done).
     *
     * After this call, `is_ready()` returns true and `exec()` is usable.
     */
    void init_namespace_();

    /**
     * @brief Clears the persistent namespace and marks the interpreter not ready.
     *
     * **Must be called with the GIL held** from `HubScript::hub_thread_fn_()`,
     * immediately before `py::scoped_interpreter` is destroyed (Py_Finalize).
     *
     * After this call, `is_ready()` returns false and `exec()` returns an error.
     */
    void release_namespace_();

  private:
    PythonInterpreter();
    ~PythonInterpreter();

    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub
