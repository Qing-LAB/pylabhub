#pragma once
/**
 * @file utils/script_engine_factory.hpp
 * @brief ScriptEngine plugin-registry surface (pylabhub-utils side).
 *
 * Per HEP-CORE-0011 §"Engine Construction Lifecycle" the script engine
 * is constructed inside the host's `worker_main_` Step 0 — on the
 * thread that will own the GIL.  This header is the registry surface
 * the role hosts and `HubScriptRunner` use to construct an engine
 * without taking a direct link-time dependency on `pylabhub-scripting`.
 *
 * **Library split.**
 *   - `pylabhub-utils` (this header + the registry TU) owns the
 *     `register_engine_factory` setter and the `create_engine`
 *     dispatcher.  Both are unconditionally defined in
 *     `libpylabhub-utils-stable.so`, so EVERY consumer of utils.so
 *     loads cleanly — including layer-1/2 test binaries that don't
 *     touch scripting at all.
 *   - `pylabhub-scripting` (`engine_factory.cpp`) owns the actual
 *     dispatching implementation that knows about `LuaEngine`,
 *     `PythonEngine`, `NativeEngine`, and registers itself with the
 *     utils-side registry from `init_scripting()`.
 *
 * **Why a registry instead of a direct symbol reference.**  If utils.so
 * carried an undefined reference to `create_engine`, every test binary
 * linked against utils.so would either need to link
 * `pylabhub-scripting.a` (pulling libpython/lua into its dep closure)
 * or rely on weak symbols (silent-crash hazard).  The registry decouples
 * cleanly: utils.so is self-contained; binaries that need scripting
 * call `pylabhub::scripting::init_scripting()` once at startup, which
 * registers the dispatching factory.  Binaries that don't never need
 * to link scripting — `create_engine` simply returns `nullptr` if no
 * factory was registered.
 *
 * **Threading & GIL.**  Per HEP-CORE-0011, `create_engine` MUST be
 * called from the host's worker thread (the future GIL holder for any
 * Python engine).  `init_scripting()` may be called from anywhere
 * (typically `main()`) — it only flips a pointer.
 *
 * @see HEP-CORE-0011 §"Engine Construction Lifecycle"
 */

#include "pylabhub_utils_export.h"

#include <memory>

namespace pylabhub::config
{
class ScriptConfig;
}

namespace pylabhub::scripting
{

class ScriptEngine;

/// Function pointer signature for engine factories.  Implementations
/// must be `noexcept` — failure is signalled by returning `nullptr`,
/// which the caller (typically `worker_main_` Step 0) maps to
/// `ready_promise().set_value(false)` for clean startup failure.
// `noexcept` is intentionally NOT part of the alias (clangd indexer
// rejects it in some configurations even though C++17 allows it in
// type aliases).  Implementations are still required to be `noexcept`
// — failure is signalled by returning `nullptr`.
using EngineFactoryFn = std::unique_ptr<ScriptEngine> (*)(const config::ScriptConfig &);

/// Register an engine factory.  Called once per process by
/// `pylabhub::scripting::init_scripting()` (defined in pylabhub-scripting).
/// Multiple calls overwrite the previously-registered factory; passing
/// `nullptr` clears it.  Safe to call from any thread.
PYLABHUB_UTILS_EXPORT
void register_engine_factory(EngineFactoryFn fp) noexcept;

/// Construct a `ScriptEngine` for the given `script` config block by
/// dispatching to the registered factory.  Returns `nullptr` if no
/// factory has been registered (which is the normal state in test
/// binaries that don't need scripting).
///
/// **Must run on the thread that will be the engine's owner thread**
/// (= the role host's worker thread).  This is what makes the worker
/// the GIL holder for Python engines.
PYLABHUB_UTILS_EXPORT
std::unique_ptr<ScriptEngine> create_engine(const config::ScriptConfig &sc) noexcept;

/// One-time initializer for the scripting layer.  Defined in
/// `pylabhub-scripting` (`engine_factory.cpp`) and called explicitly
/// from each binary's `main()` (plh_role, plh_hub) before any host is
/// constructed.  Registers the dispatching `create_engine_impl`
/// factory AND the GIL-pickup helpers (see PythonGilLease below) with
/// the utils-side registry.  Idempotent (subsequent calls are no-ops).
///
/// This is declared here for visibility but DEFINED in pylabhub-scripting
/// — binaries that link pylabhub-scripting.a will resolve it; binaries
/// that don't will see a link error if they accidentally call it,
/// which is the desired behaviour.
void init_scripting() noexcept;

// ─── GIL-pickup helpers (Option E, HEP-CORE-0011) ───────────────────────────
//
// PythonInterpreter is initialised on main; main releases the GIL via a
// stored `py::gil_scoped_release` so worker threads can acquire it.
// Each worker thread (role host worker, HubScriptRunner worker) MUST
// acquire the GIL via `py::gil_scoped_acquire` before constructing or
// touching a PythonEngine.
//
// We can't use pybind11 directly from pylabhub-utils (it would pull
// libpython into utils.so's link closure for every consumer including
// layer-1/2 tests).  Instead we expose an opaque-handle API:
// `acquire_python_gil` / `release_python_gil` are registered by
// pylabhub-scripting's `init_scripting()`; pylabhub-utils dispatches via
// a function pointer.  Workers in pylabhub-utils use the
// `PythonGilLease` RAII wrapper; the underlying pybind11 calls live in
// pylabhub-scripting only.
//
// If the interpreter is not alive (Lua-only / Native-only deployments),
// `acquire_python_gil` returns `nullptr` and the lease is a no-op.

/// Opaque token for an acquired GIL state.  Returned by
/// `acquire_python_gil`; pass to `release_python_gil` to release.
/// `nullptr` means "no GIL acquired" (typical for non-Python deployments).
using PythonGilToken = void *;

using PythonGilAcquireFn = PythonGilToken (*)();
using PythonGilReleaseFn = void (*)(PythonGilToken);

/// Register the GIL-pickup helpers.  Called by `init_scripting()` in
/// pylabhub-scripting.  Safe to call from any thread.
PYLABHUB_UTILS_EXPORT
void register_python_gil_helpers(PythonGilAcquireFn acquire, PythonGilReleaseFn release) noexcept;

/// Acquire the GIL on the calling thread iff the PythonInterpreter
/// module is loaded.  Returns `nullptr` if Python is not in play (no
/// scripting linked, or Python module not loaded).  Use the
/// `PythonGilLease` RAII wrapper rather than calling this directly.
///
/// PLH_PANICs (via the registered helper) if the interpreter is alive
/// but `validate_python_interpreter()` fails — defence-in-depth against
/// a corrupt GIL state.
PYLABHUB_UTILS_EXPORT
PythonGilToken acquire_python_gil() noexcept;

/// Release a token returned by `acquire_python_gil`.  Safe to call with
/// a `nullptr` token (no-op).  Must be called on the SAME thread that
/// acquired the token (pybind11's per-thread state requirement).
PYLABHUB_UTILS_EXPORT
void release_python_gil(PythonGilToken token) noexcept;

/// RAII GIL pickup for worker threads.  Construct at the top of
/// `worker_main_`; destruct when the worker exits.  Holds the GIL for
/// the worker's entire lifetime if Python is in play; no-op otherwise.
class PythonGilLease
{
  public:
    PythonGilLease() noexcept : token_(acquire_python_gil()) {}
    ~PythonGilLease() noexcept { release_python_gil(token_); }

    PythonGilLease(const PythonGilLease &) = delete;
    PythonGilLease &operator=(const PythonGilLease &) = delete;
    PythonGilLease(PythonGilLease &&) = delete;
    PythonGilLease &operator=(PythonGilLease &&) = delete;

    /// True iff this lease actually holds the GIL (= Python interpreter
    /// is loaded in this process).  Workers can branch on this to skip
    /// engine construction in non-Python deployments.
    [[nodiscard]] bool holds_gil() const noexcept { return token_ != nullptr; }

  private:
    PythonGilToken token_;
};

// ─── Idle-wait global-lock release (HEP-CORE-0011) ──────────────────────────
//
// MIRROR of the GIL-pickup helpers above, opposite direction.  When the
// worker holds the GIL via `PythonGilLease` for the duration of
// `worker_main_`, certain idle-wait sites (queue read with timeout,
// deadline sleep, hub event wait) do NOT touch any engine state.  If
// the role's script spawns cooperative sub-threads (Flask, asyncio,
// `threading.Thread`), those sub-threads will starve unless the worker
// briefly releases the GIL during the idle wait.
//
// `EngineGlobalLockRelease` is the RAII type that does exactly that:
// release on construct, reacquire on destruct.  Implementation lives in
// `pylabhub-scripting`'s `init_scripting()` (uses `py::gil_scoped_release`
// — same machinery as `PythonGilLease`, opposite verb).  If no impl is
// registered (Lua-only / native-only deployments), the type is a no-op.
//
// **Usage contract:**
//   - The calling thread MUST currently hold the GIL (i.e. be the
//     worker thread inside its `PythonGilLease` scope).  Constructing
//     this RAII without holding the GIL is undefined behaviour
//     (PyEval_SaveThread asserts).  Loop frames guard this by reading
//     the engine's `release_global_lock_during_wait()` flag — engines
//     return false unless the interpreter is actually in play.
//   - Nothing inside the RAII's scope may touch any py::object / call
//     into the engine.  Pure C++ idle waits only.

using EngineGlobalLockToken = void *;
using EngineGlobalLockReleaseFn = EngineGlobalLockToken (*)() noexcept;
using EngineGlobalLockReacquireFn = void (*)(EngineGlobalLockToken) noexcept;

/// Register the GIL release/reacquire helpers.  Called by
/// `init_scripting()` in pylabhub-scripting alongside the GIL-pickup
/// helpers.  Safe to call from any thread.  Registering nullptr clears
/// the registration (the RAII becomes a no-op).
PYLABHUB_UTILS_EXPORT
void register_engine_global_lock_release_helpers(EngineGlobalLockReleaseFn release,
                                                 EngineGlobalLockReacquireFn reacquire) noexcept;

/// Release the engine's global interpreter lock on the calling thread.
/// Returns nullptr if no helper is registered or the interpreter is
/// not loaded — the caller's matching `reacquire_engine_global_lock`
/// is then a no-op.  Use the `EngineGlobalLockRelease` RAII rather
/// than calling this directly.
PYLABHUB_UTILS_EXPORT
EngineGlobalLockToken release_engine_global_lock() noexcept;

/// Reacquire the lock by consuming a token returned by
/// `release_engine_global_lock`.  Safe to call with nullptr (no-op).
/// MUST run on the same thread that called release.
PYLABHUB_UTILS_EXPORT
void reacquire_engine_global_lock(EngineGlobalLockToken token) noexcept;

/// RAII: while alive, the engine's global interpreter lock is released
/// on the constructing thread; the destructor reacquires it.  No-op if
/// no helper is registered (Lua/native-only builds) or the interpreter
/// is not loaded.
class EngineGlobalLockRelease
{
  public:
    EngineGlobalLockRelease() noexcept : token_(release_engine_global_lock()) {}
    ~EngineGlobalLockRelease() noexcept { reacquire_engine_global_lock(token_); }

    EngineGlobalLockRelease(const EngineGlobalLockRelease &) = delete;
    EngineGlobalLockRelease &operator=(const EngineGlobalLockRelease &) = delete;
    EngineGlobalLockRelease(EngineGlobalLockRelease &&) = delete;
    EngineGlobalLockRelease &operator=(EngineGlobalLockRelease &&) = delete;

    /// True iff this RAII actually released the GIL (= Python
    /// interpreter is loaded AND a release helper is registered).
    /// Mostly for tests; production code does not need to branch on it.
    [[nodiscard]] bool released() const noexcept { return token_ != nullptr; }

  private:
    EngineGlobalLockToken token_;
};

} // namespace pylabhub::scripting
