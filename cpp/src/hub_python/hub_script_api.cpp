/**
 * @file hub_script_api.cpp
 * @brief HubScriptAPI implementation + pybind11 embedded module binding.
 *
 * The `hub_script_api` embedded module is imported automatically inside
 * `HubScript::startup_()` before the user script package is loaded.
 * Hub scripts do not need to `import hub_script_api` directly — they receive
 * fully-constructed Python objects from C++.
 */
#include "hub_script_api.hpp"
#include "plh_datahub.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <stdexcept>
#include <string>

namespace py = pybind11;

// ---------------------------------------------------------------------------
// ChannelInfo
// ---------------------------------------------------------------------------

namespace pylabhub
{

void ChannelInfo::request_close()
{
    if (api_)
        api_->mark_for_close(snap_.name);
}

// ---------------------------------------------------------------------------
// HubScriptAPI
// ---------------------------------------------------------------------------

std::string HubScriptAPI::hub_name() const
{
    return HubConfig::get_instance().hub_name();
}

std::string HubScriptAPI::hub_uid() const
{
    return HubConfig::get_instance().hub_uid();
}

void HubScriptAPI::log(const std::string& level, const std::string& msg) const
{
    if (level == "debug")
        LOGGER_DEBUG("[hub_script] {}", msg);
    else if (level == "warn" || level == "warning")
        LOGGER_WARN("[hub_script] {}", msg);
    else if (level == "error")
        LOGGER_ERROR("[hub_script] {}", msg);
    else
        LOGGER_INFO("[hub_script] {}", msg);
}

void HubScriptAPI::shutdown()
{
    if (shutdown_flag_)
        shutdown_flag_->store(true, std::memory_order_release);
}

std::vector<ChannelInfo> HubScriptAPI::channels() const
{
    std::vector<ChannelInfo> out;
    out.reserve(snapshot_.channels.size());
    // Const cast: ChannelInfo back-pointer is mutable self (pending_closes_ write).
    auto* self = const_cast<HubScriptAPI*>(this);
    for (const auto& e : snapshot_.channels)
        out.emplace_back(e, self);
    return out;
}

std::vector<ChannelInfo> HubScriptAPI::ready_channels() const
{
    std::vector<ChannelInfo> out;
    auto* self = const_cast<HubScriptAPI*>(this);
    for (const auto& e : snapshot_.channels)
        if (e.status == "Ready")
            out.emplace_back(e, self);
    return out;
}

std::vector<ChannelInfo> HubScriptAPI::pending_channels() const
{
    std::vector<ChannelInfo> out;
    auto* self = const_cast<HubScriptAPI*>(this);
    for (const auto& e : snapshot_.channels)
        if (e.status == "PendingReady")
            out.emplace_back(e, self);
    return out;
}

ChannelInfo HubScriptAPI::channel(const std::string& name) const
{
    auto* self = const_cast<HubScriptAPI*>(this);
    for (const auto& e : snapshot_.channels)
        if (e.name == name)
            return ChannelInfo(e, self);
    throw std::runtime_error("HubScriptAPI: channel '" + name + "' not found in snapshot");
}

} // namespace pylabhub

// ---------------------------------------------------------------------------
// pybind11 embedded module
// ---------------------------------------------------------------------------

PYBIND11_EMBEDDED_MODULE(hub_script_api, m)
{
    using namespace pylabhub;

    m.doc() = R"doc(
hub_script_api — typed API for hub scripts.

This module is automatically available inside the hub script package.
Do not import it directly; receive the api and tick objects from C++:

    def on_start(api):         pass
    def on_tick(api, tick):    pass
    def on_stop(api):          pass
)doc";

    // ------------------------------------------------------------------
    // ChannelInfo
    // ------------------------------------------------------------------
    py::class_<ChannelInfo>(m, "ChannelInfo", R"doc(
Read-only snapshot of a single hub channel.

Methods
-------
name()                 → str   — Channel name.
status()               → str   — "Ready" | "PendingReady" | "Closing"
consumer_count()       → int   — Number of registered consumers.
producer_pid()         → int   — PID of the producer (0 if unknown).
schema_hash()          → str   — 64-char hex hash, or "" if no schema.
producer_actor_name()  → str   — Actor name (empty if not set).
producer_actor_uid()   → str   — Actor UID (empty if not set).
request_close()               — Mark channel for close after on_tick returns.
)doc")
        .def("name",                &ChannelInfo::name)
        .def("status",              &ChannelInfo::status)
        .def("consumer_count",      &ChannelInfo::consumer_count)
        .def("producer_pid",        &ChannelInfo::producer_pid)
        .def("schema_hash",         &ChannelInfo::schema_hash)
        .def("producer_actor_name", &ChannelInfo::producer_actor_name)
        .def("producer_actor_uid",  &ChannelInfo::producer_actor_uid)
        .def("request_close",       &ChannelInfo::request_close,
             "Mark channel for close after on_tick returns.")
        .def("__repr__", [](const ChannelInfo& c) {
            return "<ChannelInfo name='" + c.name() + "' status='" + c.status() +
                   "' consumers=" + std::to_string(c.consumer_count()) + ">";
        });

    // ------------------------------------------------------------------
    // HubTickInfo
    // ------------------------------------------------------------------
    py::class_<HubTickInfo>(m, "HubTickInfo", R"doc(
Per-tick statistics passed as the second argument to on_tick(api, tick).

Attributes (read-only methods)
-------------------------------
tick_count()     → int  — Ticks since hub start.
elapsed_ms()     → int  — Actual wall-clock milliseconds since last tick.
uptime_ms()      → int  — Hub uptime in milliseconds.
channels_ready()   → int — Count of Ready channels.
channels_pending() → int — Count of PendingReady channels.
channels_closing() → int — Count of Closing channels.
)doc")
        .def("tick_count",       &HubTickInfo::tick_count)
        .def("elapsed_ms",       &HubTickInfo::elapsed_ms)
        .def("uptime_ms",        &HubTickInfo::uptime_ms)
        .def("channels_ready",   &HubTickInfo::channels_ready)
        .def("channels_pending", &HubTickInfo::channels_pending)
        .def("channels_closing", &HubTickInfo::channels_closing)
        .def("__repr__", [](const HubTickInfo& t) {
            return "<HubTickInfo tick=" + std::to_string(t.tick_count()) +
                   " ready=" + std::to_string(t.channels_ready()) +
                   " pending=" + std::to_string(t.channels_pending()) + ">";
        });

    // ------------------------------------------------------------------
    // HubScriptAPI
    // ------------------------------------------------------------------
    py::class_<HubScriptAPI>(m, "HubScriptAPI", R"doc(
Primary API object passed to hub script callbacks.

Identity
--------
hub_name()  → str   — Hub name in reverse-domain format.
hub_uid()   → str   — Stable hub UID ("HUB-NAME-HEXSUFFIX").

Logging
-------
log(level, msg)         — Log at "debug"/"info"/"warn"/"error".

Control
-------
shutdown()              — Request graceful hub shutdown.

Channel access
--------------
channels()              → list[ChannelInfo]  — All channels.
ready_channels()        → list[ChannelInfo]  — Ready channels only.
pending_channels()      → list[ChannelInfo]  — PendingReady channels only.
channel(name)           → ChannelInfo        — Lookup by name (raises if not found).
)doc")
        .def("hub_name",         &HubScriptAPI::hub_name)
        .def("hub_uid",          &HubScriptAPI::hub_uid)
        .def("log",              &HubScriptAPI::log,
             py::arg("level"), py::arg("msg"),
             "Log a message at the given level (debug/info/warn/error).")
        .def("shutdown",         &HubScriptAPI::shutdown,
             "Request a graceful hub shutdown.")
        .def("channels",         &HubScriptAPI::channels,
             "Return all channels as ChannelInfo objects.")
        .def("ready_channels",   &HubScriptAPI::ready_channels,
             "Return only Ready channels.")
        .def("pending_channels", &HubScriptAPI::pending_channels,
             "Return only PendingReady channels.")
        .def("channel",          &HubScriptAPI::channel,
             py::arg("name"),
             "Look up a channel by name (raises RuntimeError if not found).")
        .def("__repr__", [](const HubScriptAPI& a) {
            return "<HubScriptAPI hub='" + a.hub_name() + "'>";
        });
}
