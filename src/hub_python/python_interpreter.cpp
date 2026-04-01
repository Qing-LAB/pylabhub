/**
 * @file python_interpreter.cpp
 * @brief PythonInterpreter lifecycle module implementation.
 *
 * In the new design the CPython interpreter (`py::scoped_interpreter`) is
 * owned by `HubScript::hub_thread_fn_()`.  This module is a lifecycle
 * registration placeholder (startup/shutdown are no-ops) that provides:
 *   - The persistent `__main__` namespace for `exec()` (set up / torn down
 *     by `init_namespace_()` / `release_namespace_()` called from hub_thread_)
 *   - `exec()` for AdminShell (thread-safe, guarded by `ready_` flag)
 *   - `set_shutdown_callback()` / `request_shutdown()` for `pylabhub.shutdown()`
 */
#include "python_interpreter.hpp"
#include "plh_datahub.hpp"

// pybind11 embed header must be included exactly once in the embedding executable.
#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace py = pybind11;
namespace pylabhub
{

// ---------------------------------------------------------------------------
// Global module state
// ---------------------------------------------------------------------------

static std::function<void()> g_shutdown_cb;
static std::mutex             g_shutdown_cb_mu;

// ---------------------------------------------------------------------------
// PythonInterpreter::Impl
// ---------------------------------------------------------------------------

struct PythonInterpreter::Impl
{
    // Shared reset implementation (no lock, no GIL acquire). Called by both reset_namespace()
    // (which holds exec_mu and the GIL) and reset_namespace_unlocked() (exec_mu already held
    // by exec(); GIL re-acquired by the caller before calling this).
    static void do_reset(py::dict &ns_dict)
    {
        auto to_keep = py::set();
        to_keep.add(py::str("__builtins__"));
        to_keep.add(py::str("__name__"));
        to_keep.add(py::str("__doc__"));
        to_keep.add(py::str("__package__"));
        to_keep.add(py::str("__spec__"));
        to_keep.add(py::str("pylabhub"));

        py::list keys_to_delete;
        for (auto item : ns_dict)
            if (!to_keep.contains(item.first))
                keys_to_delete.append(item.first);

        for (auto k : keys_to_delete)
        {
            if (PyDict_DelItem(ns_dict.ptr(), k.ptr()) != 0)
                PyErr_Clear(); // key already removed — harmless; continue reset
        }

        LOGGER_INFO("PythonInterpreter: namespace reset");
    }
    // Persistent execution namespace — shared across all exec() calls.
    // NOTE: py::object (not py::dict) — py::dict eagerly calls PyDict_New() in its
    // default constructor, which crashes if constructed before Py_Initialize() runs.
    // py::object default-constructs to a null handle (safe).
    // The real dict is assigned in init_namespace() after Py_Initialize.
    py::object ns;

    // Serialise concurrent exec() callers (GIL serialises Python itself, but
    // we also want to serialise the surrounding StringIO redirect logic).
    std::mutex exec_mu;

    // True between init_namespace() and release_namespace().
    std::atomic<bool> ready_{false};

    // -----------------------------------------------------------------------
    // Lifecycle hooks — no-ops; interpreter is owned by HubScript::hub_thread_.
    // -----------------------------------------------------------------------

    void startup()
    {
        // Interpreter is now owned by HubScript::hub_thread_fn_().
        // This function exists only so the LifecycleManager can sequence
        // AdminShell after PythonInterpreter.
        LOGGER_INFO("PythonInterpreter: lifecycle startup (interpreter owned by HubScript::hub_thread_)");
    }

    void shutdown()
    {
        LOGGER_INFO("PythonInterpreter: lifecycle shutdown (no-op; interpreter finalized by HubScript)");
    }

    // -----------------------------------------------------------------------
    // Namespace management — called from HubScript::hub_thread_fn_()
    // -----------------------------------------------------------------------

    void init_namespace()
    {
        // GIL must be held on entry (called from hub_thread_ with GIL held).
        // Bootstrap the persistent namespace from __main__.
        auto main_mod = py::module_::import("__main__");
        ns = main_mod.attr("__dict__").cast<py::dict>();

        // Pre-import the pylabhub module and bind it in the namespace so
        // admin shell exec() callers can use `pylabhub.xxx()` without a
        // manual `import pylabhub` statement.
        try
        {
            auto plh_mod = py::module_::import("pylabhub");
            // ns is py::object (not py::dict) — see declaration comment above.
            // Use raw CPython API to avoid constructing py::dict from ns.
            PyDict_SetItemString(ns.ptr(), "pylabhub", plh_mod.ptr());
            LOGGER_INFO("PythonInterpreter: 'pylabhub' module imported into namespace");
        }
        catch (const py::error_already_set& e)
        {
            LOGGER_WARN("PythonInterpreter: could not import 'pylabhub': {}", e.what());
        }

        // Mark interpreter as ready — exec() callers may proceed.
        ready_.store(true, std::memory_order_release);
        LOGGER_INFO("PythonInterpreter: namespace initialized (Python {})", Py_GetVersion());
    }

    void release_namespace()
    {
        // GIL must be held on entry (called from hub_thread_ with GIL held).
        // Mark interpreter as not ready — exec() will return error immediately.
        ready_.store(false, std::memory_order_release);

        // Release the namespace dict (holds py::object refs; must happen
        // before Py_Finalize, which is triggered by hub_thread_'s
        // scoped_interpreter destructor).
        ns = py::object(); // null handle; destructor is a no-op
        LOGGER_INFO("PythonInterpreter: namespace released");
    }
};

// ---------------------------------------------------------------------------
// PythonInterpreter — public interface
// ---------------------------------------------------------------------------

PythonInterpreter::PythonInterpreter() : pImpl(std::make_unique<Impl>()) {}
PythonInterpreter::~PythonInterpreter() = default;

// static
PythonInterpreter& PythonInterpreter::get_instance()
{
    static PythonInterpreter instance;
    return instance;
}

// static
void PythonInterpreter::set_shutdown_callback(std::function<void()> cb)
{
    std::lock_guard lock(g_shutdown_cb_mu);
    g_shutdown_cb = std::move(cb);
}

// static
void PythonInterpreter::request_shutdown()
{
    std::function<void()> cb;
    {
        std::lock_guard lock(g_shutdown_cb_mu);
        cb = g_shutdown_cb;
    }
    if (cb)
    {
        LOGGER_INFO("PythonInterpreter: shutdown requested from Python");
        cb();
    }
    else
    {
        LOGGER_WARN("PythonInterpreter: shutdown() called but no callback registered");
    }
}

bool PythonInterpreter::is_ready() const noexcept
{
    return pImpl->ready_.load(std::memory_order_acquire);
}

PyExecResult PythonInterpreter::exec(const std::string& code)
{
    // Fast-path guard (racy but cheap — avoids lock overhead when clearly not ready).
    if (!pImpl->ready_.load(std::memory_order_acquire))
    {
        PyExecResult result;
        result.success = false;
        result.error   = "Python interpreter not ready (HubScript is still initializing)";
        return result;
    }

    std::lock_guard exec_lock(pImpl->exec_mu);
    py::gil_scoped_acquire gil;
    // [REVIEW-2] Authoritative check after holding exec_mu AND the GIL.
    // release_namespace() also needs the GIL, so once we hold it ns cannot be
    // nulled under us — eliminating the TOCTOU between the fast-path check above
    // and actual use of pImpl->ns below.
    if (!pImpl->ready_.load(std::memory_order_acquire))
    {
        PyExecResult result;
        result.success = false;
        result.error   = "Python interpreter not ready (shutting down)";
        return result;
    }

    PyExecResult result;
    try
    {
        // Redirect stdout + stderr to a StringIO buffer.
        auto io        = py::module_::import("io");
        auto sys       = py::module_::import("sys");
        auto old_out   = sys.attr("stdout");
        auto old_err   = sys.attr("stderr");
        auto buf       = io.attr("StringIO")();
        sys.attr("stdout") = buf;
        sys.attr("stderr") = buf;

        // Guard: restore stdout/stderr even if py::exec() throws a non-Python exception
        // (std::bad_alloc, pybind11::cast_error, etc.) that the inner catch doesn't handle.
        // Normal path dismisses this guard and restores explicitly below so output can be
        // captured from `buf` before the restore happens.
        auto restore_guard = pylabhub::basics::make_scope_guard([&]() noexcept {
            try { sys.attr("stdout") = old_out; sys.attr("stderr") = old_err; }
            catch (...) {}
        });

        try
        {
            py::exec(code, pImpl->ns);
            result.success = true;
        }
        catch (const py::error_already_set& e)
        {
            result.success = false;
            result.error   = e.what();
        }

        restore_guard.dismiss(); // normal path: dismiss guard, restore inline below
        sys.attr("stdout") = old_out;
        sys.attr("stderr") = old_err;
        result.output = buf.attr("getvalue")().cast<std::string>();
    }
    catch (const std::exception& e)
    {
        result.success = false;
        result.error   = e.what();
    }
    return result;
}

void PythonInterpreter::reset_namespace()
{
    if (!pImpl->ready_.load(std::memory_order_acquire))
        return;

    std::lock_guard exec_lock(pImpl->exec_mu);
    py::gil_scoped_acquire gil;

    // ns is stored as py::object; borrow as py::dict for dict-specific iteration.
    // Debug-assert that the contract holds: ns must be a real dict (not None/null).
    assert(PyDict_Check(pImpl->ns.ptr()) && "PythonInterpreter::ns must be a dict");
    auto ns_dict = py::reinterpret_borrow<py::dict>(pImpl->ns.ptr());
    Impl::do_reset(ns_dict);
}

void PythonInterpreter::reset_namespace_unlocked()
{
    if (!pImpl->ready_.load(std::memory_order_acquire))
        return;

    // exec_mu is already held by the calling exec() invocation.
    // The pybind11 binding releases the GIL via py::gil_scoped_release before calling
    // this function — re-acquire it here for the Python dict operations.
    py::gil_scoped_acquire gil;

    auto ns_dict = py::reinterpret_borrow<py::dict>(pImpl->ns.ptr());
    Impl::do_reset(ns_dict);
}

// ---------------------------------------------------------------------------
// Private startup / shutdown (called by lifecycle hooks — no-ops in new design)
// ---------------------------------------------------------------------------

void PythonInterpreter::startup_()  { pImpl->startup(); }
void PythonInterpreter::shutdown_() { pImpl->shutdown(); }

// ---------------------------------------------------------------------------
// Namespace management wrappers (called by HubScript::hub_thread_fn_())
// ---------------------------------------------------------------------------

void PythonInterpreter::init_namespace_()    { pImpl->init_namespace(); }
void PythonInterpreter::release_namespace_() { pImpl->release_namespace(); }

// ---------------------------------------------------------------------------
// Lifecycle startup / shutdown free functions
// ---------------------------------------------------------------------------

namespace
{
void do_python_startup(const char* /*arg*/, void* /*userdata*/)
{
    PythonInterpreter::get_instance().startup_();
}

void do_python_shutdown(const char* /*arg*/, void* /*userdata*/)
{
    PythonInterpreter::get_instance().shutdown_();
}
} // namespace

// static
utils::ModuleDef PythonInterpreter::GetLifecycleModule()
{
    utils::ModuleDef module("pylabhub::PythonInterpreter");
    module.add_dependency("pylabhub::utils::Logger");
    module.add_dependency("pylabhub::HubConfig");
    module.set_startup(&do_python_startup);
    module.set_shutdown(&do_python_shutdown,
                        std::chrono::milliseconds(pylabhub::kMidTimeoutMs));
    return module;
}

} // namespace pylabhub
