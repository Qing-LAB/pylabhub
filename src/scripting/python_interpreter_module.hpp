#pragma once
/**
 * @file python_interpreter_module.hpp
 * @brief Lifecycle-module wrapper for the embedded CPython interpreter.
 *
 * The embedded CPython interpreter is a process-singleton resource owned
 * by the binary's MAIN THREAD.  Per HEP-CORE-0011 §"Engine Construction
 * Lifecycle" (Option E):
 *
 *   - **Init runs on main.**  After config parse, if any role/hub uses
 *     Python, main calls `ensure_python_interpreter_loaded()`.  pi_startup
 *     runs `Py_InitializeFromConfig` on main (acquiring the GIL on main),
 *     then immediately releases the GIL via a stored `py::gil_scoped_release`
 *     so workers can acquire it.
 *
 *   - **Workers pick up the GIL by contract.**  Each role host's
 *     `worker_main_` opens with `py::gil_scoped_acquire gil;` before
 *     touching any pybind11 state.  Validate the state explicitly via
 *     `validate_python_interpreter()` and `PLH_PANIC` on failure.
 *
 *   - **Finalize runs on main.**  After hosts are torn down (workers
 *     joined, engines destroyed) and BEFORE LifecycleGuard unwinds, main
 *     calls `release_python_interpreter()`.  When the last ref drops, the
 *     stored `gil_scoped_release` is dropped (re-acquiring the GIL on
 *     main) and `py::scoped_interpreter` is destroyed (`Py_FinalizeEx`
 *     on main — same thread that ran init, satisfying CPython's
 *     single-thread initialise/finalise contract).
 *
 *   - **Owner-managed teardown.**  The module is registered with
 *     `set_owner_managed_teardown(true)`.  LifecycleManager
 *     STRUCTURALLY skips pi_shutdown during graph cleanup, so there is
 *     no async cross-thread `Py_FinalizeEx`.  pi_shutdown is a
 *     contractual no-op.
 *
 * Lua-only and Native-only deployments never call `ensure_*` — the
 * module is never registered, never loaded, no Python startup cost, no
 * libpython resident in memory.
 *
 * @see HEP-CORE-0011 §"Engine Construction Lifecycle"
 * @see HEP-CORE-0001 §"Owner-managed teardown"
 */

#include "pylabhub_utils_export.h"

namespace pylabhub::scripting
{

/// Module name as registered with LifecycleManager.  Exposed so other
/// (future) modules can declare a dependency edge if needed.
PYLABHUB_UTILS_EXPORT extern const char *const kPythonInterpreterModuleName;

/**
 * @brief Ensure the PythonInterpreter dynamic lifecycle module is
 *        registered AND loaded.
 *
 * Idempotent + thread-safe.  Called once by `main()` after config
 * parse if the parsed config indicates Python is needed.  First call
 * registers the module and runs pi_startup on the calling thread
 * (= main): Py_InitializeFromConfig + GIL release.  Subsequent calls
 * bump the lifecycle refcount.
 *
 * Each successful call MUST be balanced by a `release_python_interpreter()`.
 * Production callers: `plh_role_main.cpp` / `plh_hub_main.cpp` after
 * config parse and before host construction.
 *
 * @return true on success; false on registration / load failure.  The
 *         caller is expected to fail process startup on false.
 */
PYLABHUB_UTILS_EXPORT bool ensure_python_interpreter_loaded() noexcept;

/**
 * @brief Drop one reference on the PythonInterpreter dynamic module.
 *
 * Called by `main()` AFTER hosts are torn down (workers joined, engines
 * destroyed) and BEFORE `LifecycleGuard` unwinds.  When the last ref
 * drops, runs the actual interpreter destruction synchronously on the
 * calling thread (must be the owner thread that ran the original
 * `ensure_*`) — re-acquires GIL, destroys py::scoped_interpreter,
 * runs Py_FinalizeEx.
 *
 * Imbalanced calls (release without matching ensure) are logged and
 * non-fatal.  Cross-thread release (calling on a thread other than the
 * recorded owner) logs an error and skips Py_FinalizeEx — the
 * interpreter leaks until process exit, which is preferable to a
 * cross-thread CPython UB.
 */
PYLABHUB_UTILS_EXPORT void release_python_interpreter() noexcept;

/**
 * @brief Cheap liveness check — true iff the interpreter is currently
 *        loaded and not yet finalised.
 *
 * Does NOT check GIL state — safe to call from any thread.  Use this
 * to gate a `py::gil_scoped_acquire` in worker_main_ so non-Python
 * deployments (Lua, Native) don't try to acquire a non-existent GIL.
 */
PYLABHUB_UTILS_EXPORT bool python_interpreter_is_alive() noexcept;

/**
 * @brief Runtime invariant check — true iff the interpreter is alive
 *        AND CPython agrees AND the CALLING thread holds the GIL.
 *
 * Designed for use immediately after `py::gil_scoped_acquire` in
 * `worker_main_` (and inside `PythonEngine`'s ctor as defence in
 * depth).  PLH_PANIC on `false` to convert what would otherwise be a
 * cryptic CPython segfault into a loud, located boundary error.
 *
 * Cheap: three atomic / Python-API checks.  Safe to call from any
 * thread (the GIL check returns false if the calling thread doesn't
 * hold the GIL — that's the point).
 */
PYLABHUB_UTILS_EXPORT bool validate_python_interpreter() noexcept;

} // namespace pylabhub::scripting
