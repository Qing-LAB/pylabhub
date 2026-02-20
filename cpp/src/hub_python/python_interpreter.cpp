/**
 * @file python_interpreter.cpp
 * @brief PythonInterpreter lifecycle module implementation.
 */
#include "python_interpreter.hpp"
#include "plh_datahub.hpp"

// pybind11 embed header must be included exactly once in the embedding executable.
#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace py = pybind11;
namespace pylabhub
{

// ---------------------------------------------------------------------------
// Global module state
// ---------------------------------------------------------------------------

static std::atomic<bool>    g_py_initialized{false};
static std::function<void()> g_shutdown_cb;
static std::mutex            g_shutdown_cb_mu;

// ---------------------------------------------------------------------------
// PythonInterpreter::Impl
// ---------------------------------------------------------------------------

struct PythonInterpreter::Impl
{
    // The scoped_interpreter owns Py_Initialize / Py_Finalize.
    // Must be alive for the entire lifetime of any Python operation.
    std::unique_ptr<py::scoped_interpreter> guard;

    // Persistent execution namespace — shared across all exec() calls.
    py::dict ns;

    // Serialise concurrent exec() callers (GIL serialises Python itself, but
    // we also want to serialise the surrounding StringIO redirect logic).
    std::mutex exec_mu;

    void startup()
    {
        guard = std::make_unique<py::scoped_interpreter>();

        // Bootstrap the persistent namespace from __main__.
        auto main_mod = py::module_::import("__main__");
        ns = main_mod.attr("__dict__").cast<py::dict>();

        // Pre-import the pylabhub module so it's immediately available.
        try
        {
            py::module_::import("pylabhub");
            LOGGER_INFO("PythonInterpreter: 'pylabhub' module imported into namespace");
        }
        catch (const py::error_already_set& e)
        {
            LOGGER_WARN("PythonInterpreter: could not import 'pylabhub': {}", e.what());
        }

        LOGGER_INFO("PythonInterpreter: interpreter ready (Python {})", Py_GetVersion());
    }

    void shutdown()
    {
        // Release the namespace first (it holds py::object refs; must happen before
        // Py_Finalize, which is triggered by destroying the scoped_interpreter).
        {
            py::gil_scoped_acquire gil;
            ns = py::dict();
        }
        guard.reset(); // calls Py_Finalize
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

PyExecResult PythonInterpreter::exec(const std::string& code)
{
    std::lock_guard exec_lock(pImpl->exec_mu);
    py::gil_scoped_acquire gil;

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
    std::lock_guard exec_lock(pImpl->exec_mu);
    py::gil_scoped_acquire gil;

    // Clear user-defined names; preserve builtins and the pylabhub module.
    auto to_keep = py::set();
    to_keep.add(py::str("__builtins__"));
    to_keep.add(py::str("__name__"));
    to_keep.add(py::str("__doc__"));
    to_keep.add(py::str("__package__"));
    to_keep.add(py::str("__spec__"));
    to_keep.add(py::str("pylabhub"));

    py::list keys_to_delete;
    for (auto item : pImpl->ns)
        if (!to_keep.contains(item.first))
            keys_to_delete.append(item.first);

    for (auto k : keys_to_delete)
        PyDict_DelItem(pImpl->ns.ptr(), k.ptr());

    LOGGER_INFO("PythonInterpreter: namespace reset");
}

// ---------------------------------------------------------------------------
// Private startup / shutdown (called by lifecycle hooks)
// ---------------------------------------------------------------------------

void PythonInterpreter::startup_() { pImpl->startup(); }
void PythonInterpreter::shutdown_() { pImpl->shutdown(); }

// ---------------------------------------------------------------------------
// Lifecycle startup / shutdown free functions
// ---------------------------------------------------------------------------

namespace
{
void do_python_startup(const char* /*arg*/)
{
    PythonInterpreter::get_instance().startup_();
    g_py_initialized.store(true, std::memory_order_release);
}

void do_python_shutdown(const char* /*arg*/)
{
    g_py_initialized.store(false, std::memory_order_release);
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
    module.set_shutdown(&do_python_shutdown, std::chrono::seconds(5));
    return module;
}

} // namespace pylabhub
