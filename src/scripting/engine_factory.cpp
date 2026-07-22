/**
 * @file engine_factory.cpp
 * @brief `pylabhub-scripting` side of the ScriptEngine plugin registry.
 *
 * Defines:
 *   - `create_engine_impl` — the actual dispatching factory that
 *     constructs `LuaEngine` / `PythonEngine` / `NativeEngine`.  Static
 *     to this TU; published only via `init_scripting()`.
 *   - `init_scripting()` — declared in
 *     `utils/script_engine_factory.hpp`; registers
 *     `create_engine_impl` with the utils-side registry.  Idempotent.
 *
 * Each binary that links `pylabhub-scripting.a` MUST call
 * `pylabhub::scripting::init_scripting()` once from `main()` BEFORE
 * any role host or `HubScriptRunner` is constructed.  See HEP-CORE-0011
 * §"Engine Construction Lifecycle".
 */

#include "utils/script_engine_factory.hpp"
#include "python_interpreter_module.hpp" // python_interpreter_is_alive / validate

#include "utils/config/script_config.hpp"
#include "utils/native_engine.hpp"
#include "utils/script_engine.hpp"
#include "utils/debug_info.hpp" // PLH_PANIC for validate failure
#include "lua_engine.hpp"
#include "python_engine.hpp"

#include <pybind11/pybind11.h> // py::gil_scoped_acquire

#include <atomic>

namespace pylabhub::scripting
{

namespace
{

namespace py = pybind11;

// TU-private dispatching factory.  Published only through the utils-side
// registry by `init_scripting()`.
//
// **GIL contract.**  PythonEngine's ctor uses `py::object{py::none()}`
// member-default-initializers — they require the GIL.  Production
// callers (role host worker_main_, HubScriptRunner) hold the GIL via
// PythonGilLease before calling create_engine, so the acquire below
// is a reentrant no-op.  Test callers (EngineFactoryTest test thread
// after pi_startup released main's GIL) do NOT hold it, so the
// acquire here is what makes the ctor safe.  py::gil_scoped_acquire's
// scope ends at function return; the engine's py::object members hold
// references to `Py_None` that are torn down later via
// PythonEngine's dtor (which lazy-acquires GIL if needed) or via
// `clear_pyobjects_` detach during `finalize_engine_`.
std::unique_ptr<ScriptEngine> create_engine_impl(const config::ScriptConfig &sc) noexcept
{
    try
    {
        if (sc.type == "native")
        {
            auto ne = std::make_unique<NativeEngine>();
            if (!sc.checksum.empty())
                ne->set_expected_checksum(sc.checksum);
            return ne;
        }
        if (sc.type == "lua")
        {
            return std::make_unique<LuaEngine>();
        }
        // Default: python (explicit "python" OR unknown type — preserves
        // the prior per-main behaviour).  Acquire GIL for the duration
        // of construction iff the interpreter is loaded; otherwise
        // PythonEngine's ctor will PLH_PANIC on validate (interpreter
        // not loaded).  The acquire is reentrant — if the caller
        // already holds GIL via PythonGilLease, this is a no-op.
        std::optional<py::gil_scoped_acquire> gil;
        if (python_interpreter_is_alive())
            gil.emplace();
        auto py_engine = std::make_unique<PythonEngine>();
        if (!sc.python_venv.empty())
            py_engine->set_python_venv(sc.python_venv);
        py_engine->set_release_global_lock_during_wait(sc.release_global_lock_during_wait);
        return py_engine;
    }
    catch (...)
    {
        // PythonEngine ctor PLH_PANICs on PythonInterpreter load
        // failure (which doesn't return here — it aborts).  Other
        // unexpected exceptions are mapped to nullptr to keep this
        // function noexcept.
        return nullptr;
    }
}

// ─── GIL-pickup helpers (Option E, HEP-CORE-0011) ───────────────────────────
//
// Workers acquire the GIL via py::gil_scoped_acquire for the lifetime
// of `worker_main_`.  pylabhub-utils can't include pybind11 (would pull
// libpython into utils.so's link closure for every consumer).  These
// implementations live here and are registered with the utils-side
// registry by `init_scripting()`; the utils-side `PythonGilLease` RAII
// dispatches through a function pointer.
//
// `acquire_python_gil_impl` returns nullptr if the interpreter is not
// loaded (Lua/native deployments) — the lease becomes a no-op.

namespace py = pybind11;

// noexcept: the utils-side `acquire_python_gil` dispatcher is noexcept
// and would terminate if our impl threw.  Wrap allocation in try/catch
// and return nullptr on failure (caller's `PythonGilLease::holds_gil()`
// returns false; worker logic decides whether that's recoverable).
PythonGilToken acquire_python_gil_impl() noexcept
{
    if (!python_interpreter_is_alive())
        return nullptr;

    // Heap-allocate the gil_scoped_acquire so its lifetime matches the
    // returned token.  pybind11's per-thread state is stored on the
    // calling thread; the matching release_python_gil_impl MUST run on
    // the same thread (worker contract).
    py::gil_scoped_acquire *gil = nullptr;
    try
    {
        gil = new py::gil_scoped_acquire();
    }
    catch (...)
    {
        return nullptr;
    }

    // Defence-in-depth: now that we hold the GIL, validate the
    // interpreter state.  PLH_PANIC on failure converts what would
    // otherwise be a cryptic CPython UB at the first py::object access
    // into a loud, located boundary error.
    if (!validate_python_interpreter())
    {
        delete gil;
        PLH_PANIC("acquire_python_gil_impl: validate_python_interpreter() "
                  "returned false after acquiring the GIL.  Interpreter "
                  "may be in a corrupt state.  Refusing to proceed.");
    }
    return gil;
}

void release_python_gil_impl(PythonGilToken token) noexcept
{
    if (token == nullptr)
        return;
    delete static_cast<py::gil_scoped_acquire *>(token);
}

// ─── Idle-wait global-lock release (HEP-CORE-0011) ──────────────────────────
//
// Opposite-direction mirror of the GIL-pickup pair.  Caller is the
// worker thread inside its `PythonGilLease` scope (so the GIL IS held).
// `py::gil_scoped_release` ctor calls `PyEval_SaveThread()` which
// requires the GIL be held — guaranteed by the caller's contract
// (verified in `run_data_loop` before constructing the RAII: the wrap
// only runs when the engine returned `release_global_lock_during_wait()
// == true`, which only PythonEngine returns true for, only when Python
// is in play, hence the worker holds GIL).
//
// Defensive: if the interpreter is not loaded (Lua-only build that
// somehow registered these helpers), return nullptr — the caller's
// matching `reacquire_engine_global_lock_impl` is then a no-op.
//
// ── Cost note: heap allocation per cycle ────────────────────────────────
//
// This impl `new`-allocates a `py::gil_scoped_release` and `delete`s it
// on the matching reacquire.  Per-cycle cost: ~30–100 ns malloc + ~30–
// 100 ns free, on top of the ~100–200 ns of actual `PyEval_SaveThread`
// / `PyEval_RestoreThread` work.  At 100 Hz this is ~10 µs/sec — well
// below noise.  At 10 kHz it is ~1 ms/sec — measurable but the script
// is already paying many times this on every callback.
//
// Reason for the heap alloc: pylabhub-utils declares the public
// `EngineGlobalLockRelease` RAII class but cannot include
// pybind11/embed.h (would pull libpython into utils.so's link closure
// for every consumer including layer-1/2 tests).  The opaque token is
// a `void*`; the simplest concrete payload is a heap-allocated
// `py::gil_scoped_release`.
//
// If a profiler ever shows this matters, two cheaper alternatives —
// in order of preference:
//
//   1. **Thread-local storage** — keep pybind11's wrapper for safety,
//      but stash it in a `thread_local std::optional<py::gil_scoped_release>`
//      inside this TU.  emplace() / reset() avoid heap alloc entirely.
//      The contract "every release is matched by a reacquire on the
//      same thread" already holds, so the TLS slot is single-occupancy.
//      No public API change, no library-boundary change.
//
//   2. **Raw CPython API** — return `PyEval_SaveThread()` directly as
//      the token; reacquire calls `PyEval_RestoreThread`.  No heap, no
//      pybind11 wrapper.  Saves the same ~60–100 ns as option 1.  Risk:
//      asymmetric with `PythonGilLease` (which uses pybind11's
//      `gil_scoped_acquire`) and bypasses any future pybind11
//      bookkeeping additions.  CPython's `PyEval_Save/RestoreThread`
//      contract has been stable for ~25 years, so the actual breakage
//      risk is small, but it remains a deliberate departure from the
//      pybind11-wrapper convention used elsewhere in this TU.
//
// Don't optimize speculatively — the current cost is invisible at our
// typical loop frequencies (10–1000 Hz).
EngineGlobalLockToken release_engine_global_lock_impl() noexcept
{
    if (!python_interpreter_is_alive())
        return nullptr;
    py::gil_scoped_release *rel = nullptr;
    try
    {
        rel = new py::gil_scoped_release();
    }
    catch (...)
    {
        return nullptr;
    }
    return rel;
}

void reacquire_engine_global_lock_impl(EngineGlobalLockToken token) noexcept
{
    if (token == nullptr)
        return;
    // Dtor calls PyEval_RestoreThread which reacquires the GIL on this
    // thread.  Must run on the same thread that called the release.
    delete static_cast<py::gil_scoped_release *>(token);
}

} // namespace

void init_scripting() noexcept
{
    // Idempotent: registering the same pointer twice is a no-op for the
    // registry; we add the local guard purely to short-circuit pointless
    // work on repeated calls.
    static std::atomic<bool> done{false};
    bool expected = false;
    if (!done.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    register_engine_factory(&create_engine_impl);
    register_python_gil_helpers(&acquire_python_gil_impl, &release_python_gil_impl);
    register_engine_global_lock_release_helpers(&release_engine_global_lock_impl,
                                                &reacquire_engine_global_lock_impl);
}

} // namespace pylabhub::scripting
