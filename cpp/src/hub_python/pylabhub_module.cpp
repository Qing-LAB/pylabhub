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
 *   cfg = pylabhub.config()           # flat dict with all hub settings + paths
 *   print(cfg['name'])                # "asu.lab.experiments.main"
 *   print(cfg['broker_address'])      # "tcp://0.0.0.0:5570"
 *   chs = pylabhub.channels('ready')  # list of ready channel dicts
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

/// Registered by hubshell when BrokerService is ready.
/// Requests channel close → CHANNEL_CLOSING_NOTIFY to all participants.
static std::function<void(const std::string&)> g_close_channel_cb;

void set_channels_callback(std::function<std::vector<py::dict>()> cb)
{
    g_channels_cb = std::move(cb);
}

void set_close_channel_callback(std::function<void(const std::string&)> cb)
{
    g_close_channel_cb = std::move(cb);
}

/// Registered by hubshell when BrokerService is ready.
/// Broadcasts a message to all members of a channel.
static std::function<void(const std::string&, const std::string&, const std::string&)>
    g_broadcast_channel_cb;

void set_broadcast_channel_callback(
    std::function<void(const std::string&, const std::string&, const std::string&)> cb)
{
    g_broadcast_channel_cb = std::move(cb);
}
/// Registered by hubshell when BrokerService is ready.
/// Returns metrics JSON string for a channel (or all channels).
static std::function<std::string(const std::string&)> g_metrics_cb;

void set_metrics_callback(std::function<std::string(const std::string&)> cb)
{
    g_metrics_cb = std::move(cb);
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
    cfg = pylabhub.config()
    print(cfg['hub']['name'])
    print(cfg['network']['broker_address'])
    for ch in pylabhub.channels('ready'):
        print(ch['name'], ch['consumer_count'])
    pylabhub.shutdown()
)doc";

    // ------------------------------------------------------------------
    // Version
    // ------------------------------------------------------------------
    m.attr("__version__") = py::str(pylabhub::platform::get_version_string());

    // ------------------------------------------------------------------
    // Config — single consolidated dict with all non-secret hub settings
    // ------------------------------------------------------------------
    m.def("config", []() -> py::dict
    {
        const auto& c = pylabhub::HubConfig::get_instance();
        py::dict d;

        // Identity
        d["name"]        = c.hub_name();
        d["uid"]         = c.hub_uid();
        d["description"] = c.hub_description();

        // Network addresses
        d["broker_address"] = c.broker_endpoint();
        d["shell_address"]  = c.admin_endpoint();

        // Broker operational settings
        d["channel_timeout_s"]         = static_cast<int>(c.channel_timeout().count());
        d["consumer_liveness_check_s"] = static_cast<int>(c.consumer_liveness_check().count());

        // Resolved filesystem paths
        d["root_dir"]            = c.root_dir().string();
        d["config_dir"]          = c.config_dir().string();
        d["scripts_python"]      = c.scripts_python_dir().string();
        d["scripts_lua"]         = c.scripts_lua_dir().string();
        d["data_dir"]            = c.data_dir().string();
        d["python_requirements"] = c.python_requirements().string();
        const auto& sd = c.hub_script_dir();
        d["hub_script_dir"] = sd.empty()
            ? py::object(py::none()) : py::object(py::str(sd.string()));
        const auto& hd = c.hub_dir();
        d["log_file"] = hd.empty()
            ? py::object(py::none())
            : py::object(py::str((hd / "logs" / "hub.log").string()));

        return d;
    },
    R"doc(
Return the active hub configuration as a flat dict.

Keys: name, uid, description, broker_address, shell_address,
channel_timeout_s, consumer_liveness_check_s, root_dir, config_dir,
scripts_python, scripts_lua, data_dir, python_requirements,
hub_script_dir, log_file.

Example::

    cfg = pylabhub.config()
    print(cfg['name'])
    print(cfg['broker_address'])
    print(cfg['data_dir'])
)doc");

    // ------------------------------------------------------------------
    // Active channels (wired to BrokerService in Phase 6)
    // ------------------------------------------------------------------
    m.def("channels", [](const std::string& filter) -> py::object
    {
        py::list all, ready, pending, closing;
        if (pylabhub::hub_python::g_channels_cb)
        {
            for (auto& d : pylabhub::hub_python::g_channels_cb())
            {
                all.append(d);
                auto status = py::cast<std::string>(d["status"]);
                if (status == "Ready")             ready.append(d);
                else if (status == "PendingReady") pending.append(d);
                else if (status == "Closing")      closing.append(d);
            }
        }
        // With a filter argument, return just that category as a list.
        if (filter == "ready")        return ready;
        if (filter == "pending")      return pending;
        if (filter == "closing")      return closing;
        if (filter == "all")          return all;
        if (!filter.empty())
            throw py::value_error("Unknown filter '" + filter
                                  + "'; use 'ready', 'pending', 'closing', or 'all'");
        // No filter → return categorized dict.
        py::dict result;
        result["all"]     = all;
        result["ready"]   = ready;
        result["pending"] = pending;
        result["closing"] = closing;
        return result;
    },
    py::arg("filter") = "",
    R"doc(
Return channel info from the live broker, optionally filtered by status.

With no arguments, returns a dict with keys 'all', 'ready', 'pending', 'closing'.
Each value is a list of channel dicts with keys:
'name', 'schema_hash', 'consumer_count', 'producer_pid', 'status'.

With a filter argument, returns just that category as a list.

Returns empty collections until the broker is running and wired (Phase 6).

Example::

    chs = pylabhub.channels()
    for ch in chs['ready']:
        print(ch['name'], ':', ch['consumer_count'], 'consumers')

    # Or use the filter shortcut:
    for ch in pylabhub.channels('ready'):
        print(ch['name'])
)doc");

    // ------------------------------------------------------------------
    // Channel control
    // ------------------------------------------------------------------
    m.def("close_channel", [](const std::string& name)
    {
        if (!pylabhub::hub_python::g_close_channel_cb)
            throw py::value_error("Broker not wired — close_channel unavailable");
        pylabhub::hub_python::g_close_channel_cb(name);
    },
    py::arg("name"),
    R"doc(
Request graceful close of a channel.

Sends CHANNEL_CLOSING_NOTIFY to all participants (producer, consumers,
processors), causing them to shut down gracefully. The channel transitions
to 'Closing' status and is removed after participants disconnect.

Example::

    pylabhub.close_channel('lab.sensors.raw')
)doc");

    m.def("broadcast_channel", [](const std::string& channel, const std::string& message,
                                  const std::string& data)
    {
        if (!pylabhub::hub_python::g_broadcast_channel_cb)
            throw py::value_error("Broker not wired — broadcast_channel unavailable");
        py::gil_scoped_release release;
        pylabhub::hub_python::g_broadcast_channel_cb(channel, message, data);
    },
    py::arg("channel"), py::arg("message"), py::arg("data") = "",
    R"doc(
Broadcast a message to all members of a channel.

Sends CHANNEL_BROADCAST_NOTIFY to both the producer and all consumers of the
named channel. Each participant receives the message in their ``msgs`` list as
an event dict: ``{"event": "channel_event", "detail": "broadcast", "message": "start", ...}``.

Use this for pipeline coordination — e.g., broadcast "start" after all roles
have connected and are ready.

Example::

    pylabhub.broadcast_channel('test.pipe.raw', 'start')
    pylabhub.broadcast_channel('test.pipe.raw', 'stop', '{"reason": "test complete"}')
)doc");

    // ------------------------------------------------------------------
    // Metrics query (HEP-CORE-0019)
    // ------------------------------------------------------------------
    m.def("metrics", [](const std::string& channel) -> py::object
    {
        if (!pylabhub::hub_python::g_metrics_cb)
            throw py::value_error("Broker not wired — metrics() unavailable");
        std::string json_str;
        {
            py::gil_scoped_release release;
            json_str = pylabhub::hub_python::g_metrics_cb(channel);
        }
        // Parse JSON string → Python dict.
        py::module_ json_mod = py::module_::import("json");
        return json_mod.attr("loads")(json_str);
    },
    py::arg("channel") = "",
    R"doc(
Query aggregated metrics from all producers and consumers.

With no arguments (or empty string), returns metrics for all channels.
With a channel name, returns metrics for just that channel.

Returns a dict with 'status' key and either 'channels' (all) or 'metrics' (single).

Example::

    metrics = pylabhub.metrics()          # all channels
    ch = pylabhub.metrics('my-channel')   # single channel
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
        // Use the unlocked variant: exec() holds exec_mu when it runs py::exec(),
        // and this binding is called from within that py::exec() invocation.
        // Calling the locked reset_namespace() here would re-enter exec_mu on the
        // same thread → permanent deadlock. reset_namespace_unlocked() assumes
        // exec_mu is already held by the caller and only acquires the GIL.
        py::gil_scoped_release release;
        pylabhub::PythonInterpreter::get_instance().reset_namespace_unlocked();
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
