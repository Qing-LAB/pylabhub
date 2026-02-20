/**
 * @file pylabhub_module.cpp
 * @brief pybind11 embedded module definition for the `pylabhub` Python package.
 *
 * This module is available inside the hubshell's embedded Python interpreter.
 * It provides access to hub configuration, paths, channel information, and
 * control operations (shutdown, reset).
 *
 * Usage from the admin shell or user scripts:
 * @code
 *   import pylabhub
 *   print(pylabhub.hub_name())        # "asu.lab.experiments.main"
 *   print(pylabhub.broker_endpoint()) # "tcp://0.0.0.0:5570"
 *   d = pylabhub.config()             # dict with full config
 *   p = pylabhub.paths()              # dict with resolved paths
 *   ch = pylabhub.channels()          # list of active channel dicts
 *   pylabhub.reset()                  # clear interpreter namespace
 *   pylabhub.shutdown()               # request graceful hubshell exit
 * @endcode
 */
#include "python_interpreter.hpp"
#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace py = pybind11;

// ---------------------------------------------------------------------------
// Channel query callback (wired by hubshell in Phase 6)
// ---------------------------------------------------------------------------

namespace pylabhub::hub_python
{
/// Registered by hubshell when BrokerService is ready.
/// Returns a list of channel info dicts.
static std::function<std::vector<py::dict>()> g_channels_cb;

void set_channels_callback(std::function<std::vector<py::dict>()> cb)
{
    g_channels_cb = std::move(cb);
}
} // namespace pylabhub::hub_python

// ---------------------------------------------------------------------------
// PYBIND11_EMBEDDED_MODULE — registered at static init time
// ---------------------------------------------------------------------------

PYBIND11_EMBEDDED_MODULE(pylabhub, m)
{
    m.doc() = R"doc(
pylabhub — pyLabHub C++ bridge module.

Provides access to the hub's configuration, active channels, and control
operations from the embedded Python interpreter (admin shell or user scripts).

Example::

    import pylabhub
    print(pylabhub.hub_name())
    print(pylabhub.config())
    for ch in pylabhub.channels():
        print(ch['name'], ch['consumer_count'])
    pylabhub.shutdown()
)doc";

    // ------------------------------------------------------------------
    // Version
    // ------------------------------------------------------------------
    m.attr("__version__") = py::str(pylabhub::platform::get_version_string());

    // ------------------------------------------------------------------
    // Hub identity
    // ------------------------------------------------------------------
    m.def("hub_name", []() -> std::string
    {
        return pylabhub::HubConfig::get_instance().hub_name();
    },
    "Return the hub name in reverse-domain format (e.g. 'asu.lab.main').");

    m.def("hub_description", []() -> std::string
    {
        return pylabhub::HubConfig::get_instance().hub_description();
    },
    "Return the human-readable hub description.");

    // ------------------------------------------------------------------
    // Network endpoints
    // ------------------------------------------------------------------
    m.def("broker_endpoint", []() -> std::string
    {
        return pylabhub::HubConfig::get_instance().broker_endpoint();
    },
    "Return the ZMQ broker endpoint (e.g. 'tcp://0.0.0.0:5570').");

    m.def("admin_endpoint", []() -> std::string
    {
        return pylabhub::HubConfig::get_instance().admin_endpoint();
    },
    "Return the admin shell ZMQ endpoint (e.g. 'tcp://127.0.0.1:5600').");

    // ------------------------------------------------------------------
    // Config and paths (as dicts for easy Python use)
    // ------------------------------------------------------------------
    m.def("config", []() -> py::dict
    {
        const auto& c = pylabhub::HubConfig::get_instance();
        py::dict hub;
        hub["name"]             = c.hub_name();
        hub["description"]      = c.hub_description();
        hub["broker_endpoint"]  = c.broker_endpoint();
        hub["admin_endpoint"]   = c.admin_endpoint();
        hub["channel_timeout_s"]          = static_cast<int>(c.channel_timeout().count());
        hub["consumer_liveness_check_s"]  = static_cast<int>(c.consumer_liveness_check().count());

        py::dict result;
        result["hub"] = hub;
        return result;
    },
    R"doc(
Return the active hub configuration as a nested dict.

Example::

    cfg = pylabhub.config()
    print(cfg['hub']['name'])
    print(cfg['hub']['broker_endpoint'])
)doc");

    m.def("paths", []() -> py::dict
    {
        const auto& c = pylabhub::HubConfig::get_instance();
        py::dict d;
        d["root_dir"]           = c.root_dir().string();
        d["config_dir"]         = c.config_dir().string();
        d["scripts_python"]     = c.scripts_python_dir().string();
        d["scripts_lua"]        = c.scripts_lua_dir().string();
        d["data_dir"]           = c.data_dir().string();
        d["python_requirements"]= c.python_requirements().string();
        auto ss = c.python_startup_script();
        d["python_startup_script"] = ss.empty() ? py::object(py::none()) : py::object(py::str(ss.string()));
        return d;
    },
    R"doc(
Return all resolved hub paths as a dict.

Example::

    p = pylabhub.paths()
    print(p['data_dir'])      # '/opt/myhub/data'
    print(p['scripts_python']) # '/opt/myhub/share/scripts/python'
)doc");

    // ------------------------------------------------------------------
    // Active channels (wired to BrokerService in Phase 6)
    // ------------------------------------------------------------------
    m.def("channels", []() -> py::list
    {
        py::list result;
        if (pylabhub::hub_python::g_channels_cb)
        {
            for (auto& d : pylabhub::hub_python::g_channels_cb())
                result.append(d);
        }
        return result;
    },
    R"doc(
Return a list of active channel info dicts.

Each dict has keys: 'name', 'schema_hash', 'consumer_count', 'producer_pid'.
Returns an empty list until the broker is running and wired (Phase 6).

Example::

    for ch in pylabhub.channels():
        print(ch['name'], ':', ch['consumer_count'], 'consumers')
)doc");

    // ------------------------------------------------------------------
    // Control operations
    // ------------------------------------------------------------------
    m.def("shutdown", []()
    {
        pylabhub::PythonInterpreter::request_shutdown();
    },
    R"doc(
Request a graceful hubshell shutdown.

The shutdown is asynchronous — the hub finishes in-flight operations and then
calls Py_Finalize() before exiting. Use this instead of sys.exit() so that
all C++ resources are cleaned up properly.

Example::

    pylabhub.shutdown()
)doc");

    m.def("reset", []()
    {
        py::gil_scoped_release release;
        pylabhub::PythonInterpreter::get_instance().reset_namespace();
    },
    R"doc(
Reset the interpreter namespace, clearing all user-defined variables.

Built-ins and the 'pylabhub' module itself are preserved.

Example::

    x = 42
    pylabhub.reset()
    print(x)   # NameError: x is not defined
)doc");
}
