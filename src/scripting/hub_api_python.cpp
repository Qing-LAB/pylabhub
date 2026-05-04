/**
 * @file hub_api_python.cpp
 * @brief PYBIND11_EMBEDDED_MODULE(pylabhub_hub) — Python surface for HubAPI.
 *        Phase 7 D4.1 (HEP-CORE-0033 §12.3, mirrors lua_engine D3.2).
 *
 * The embedded module is registered at static-init time when the
 * binary that links pylabhub-scripting starts up — `import pylabhub_hub`
 * inside Python then resolves to this binding.  The module exposes
 * `pylabhub_hub.HubAPI` with the Phase 7 minimum surface (log / uid /
 * metrics).
 *
 * Hub scripts access the bound HubAPI via two paths, both wired by
 * `PythonEngine::build_api_(HubAPI&)` (analogue of build_api_(RoleAPIBase&)):
 *
 *   1. As a module-global `api` — set as an attribute on the SCRIPT'S
 *      module after import.  Scripts can write `api.log("info", ...)`
 *      from any callback, including event/tick callbacks dispatched via
 *      generic `engine.invoke(name, args)` which does NOT pass api as
 *      an argument.  Mirrors the LuaEngine pattern (build_api_(HubAPI&)
 *      sets the api Lua global).
 *
 *   2. As an explicit positional arg to `on_init(api)` / `on_stop(api)`
 *      via `invoke_on_init` / `invoke_on_stop` — same shape role-side
 *      uses.  Scripts that prefer the explicit-arg style can write
 *      `def on_init(api): api.log(...)`.
 *
 * Mirror of the role-side modules:
 *   - `pylabhub_producer` (src/producer/producer_api.cpp)
 *   - `pylabhub_consumer` (src/consumer/consumer_api.cpp)
 *   - `pylabhub_processor` (src/processor/processor_api.cpp)
 *
 * Lives in pylabhub-scripting (not pylabhub-utils) because pybind11/embed.h
 * pulls in libpython — pure-C++ pylabhub-utils consumers must not depend
 * on that.  See src/scripting/CMakeLists.txt.
 */

#include "utils/hub_api.hpp"

#include "json_py_helpers.hpp"   // detail::json_to_py (S5)

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

namespace py = pybind11;

// Force-link symbol — referenced by `PythonEngine::build_api_(HubAPI&)`
// so the static linker pulls this .o into the final binary.  Without
// this, the linker drops the entire .o file (nothing else here is
// referenced — HubAPI lives in pylabhub-utils, not this file), and the
// PYBIND11_EMBEDDED_MODULE static initializer below never registers
// `pylabhub_hub` in the inittab.  Same defense the role-side files
// (producer_api.cpp / consumer_api.cpp / processor_api.cpp) get
// implicitly because they ALSO host the role-API class impls (which
// are referenced from PythonEngine::build_api_(RoleAPIBase&)).
extern "C" void plh_register_hub_api_python_module() {}

PYBIND11_EMBEDDED_MODULE(pylabhub_hub, m) // NOLINT
{
    using pylabhub::hub_host::HubAPI;

    py::class_<HubAPI>(m, "HubAPI",
        "Hub-side script API surface (HEP-CORE-0033 §12.3 Phase 7 minimum).\n"
        "Exposed as a module global `api` to scripts after\n"
        "PythonEngine::build_api_(HubAPI&) wires it.  Scripts written\n"
        "for the role side use the same idiom (api.log / api.uid).")

        // ── Phase 7 minimum API ───────────────────────────────────────────
        .def("log", &HubAPI::log, py::arg("level"), py::arg("msg"),
             "Emit a log line through the process logger with a "
             "[hub/<uid>] prefix.  Level tokens: 'debug', 'info', "
             "'warn'/'warning', 'error'.  Anything else routes to INFO.")

        .def("uid", &HubAPI::uid,
             "Return the hub instance uid (e.g. 'hub.lab1.uid00000001').\n"
             "Stable for the life of HubAPI; returned by reference to\n"
             "the underlying std::string (pybind11 copies on conversion).")

        .def("metrics",
             [](const HubAPI &self) -> py::dict {
                 // S5: shared fast-path walker
                 // (src/scripting/json_py_helpers.hpp) — same converter
                 // PythonEngine's dispatch hot path uses for
                 // event-detail kwargs.  Replaces the prior
                 // json.loads(dump()) round-trip; semantics match for
                 // metrics payloads (no NaN/Inf, no deep recursion —
                 // see header docstring for the divergence rows).
                 return pylabhub::scripting::detail::json_to_py(
                            self.metrics())
                     .cast<py::dict>();
             },
             "Return a snapshot of the broker's metrics — channels / "
             "roles / bands / peers aggregates plus broker counters.\n"
             "Empty dict when called before HubHost::startup() or after "
             "shutdown() — same shape AdminService's query_metrics RPC "
             "uses (single source of truth via Phase 6.2b's "
             "hub_state_json serializers).")

        // ── Phase 8a — Read accessors (HEP-CORE-0033 §12.3) ───────────────
        //
        // Each method delegates straight to the C++ HubAPI accessor and
        // converts via the shared json_to_py walker.  The lambda wrappers
        // do nothing the C++ side doesn't already do — they exist only
        // because pybind11 can't auto-bind a method returning
        // nlohmann::json (no native converter).

        .def("name", &HubAPI::name,
             "Hub display name (cfg.identity().name) — distinct from "
             "uid().  Empty string if HubHost backref isn't wired yet "
             "(only happens in unit tests).")

        .def("config",
             [](const HubAPI &self) -> py::dict {
                 return pylabhub::scripting::detail::json_to_py(
                            self.config())
                     .cast<py::dict>();
             },
             "Full hub config snapshot — equivalent to the contents of "
             "hub.json post-default-merge.  Read-only; mutations go "
             "through curated admin RPCs / Phase 8b control delegates.")

        .def("list_channels",
             [](const HubAPI &self) -> py::list {
                 return pylabhub::scripting::detail::json_to_py(
                            self.list_channels())
                     .cast<py::list>();
             },
             "List of all currently-registered channels (each a dict in "
             "the same shape AdminService's list_channels RPC emits).")

        .def("get_channel",
             [](const HubAPI &self, const std::string &name) -> py::object {
                 return pylabhub::scripting::detail::json_to_py(
                            self.get_channel(name));
             },
             py::arg("name"),
             "Single channel lookup.  Returns the channel dict or None "
             "if not registered.")

        .def("list_roles",
             [](const HubAPI &self) -> py::list {
                 return pylabhub::scripting::detail::json_to_py(
                            self.list_roles())
                     .cast<py::list>();
             },
             "List of all roles (producers / consumers / processors).")

        .def("get_role",
             [](const HubAPI &self, const std::string &uid) -> py::object {
                 return pylabhub::scripting::detail::json_to_py(
                            self.get_role(uid));
             },
             py::arg("uid"),
             "Single role lookup by uid.  Returns the role dict or "
             "None.")

        .def("list_bands",
             [](const HubAPI &self) -> py::list {
                 return pylabhub::scripting::detail::json_to_py(
                            self.list_bands())
                     .cast<py::list>();
             },
             "List of all bands (HEP-CORE-0030 pub/sub channels).")

        .def("get_band",
             [](const HubAPI &self, const std::string &name) -> py::object {
                 return pylabhub::scripting::detail::json_to_py(
                            self.get_band(name));
             },
             py::arg("name"),
             "Single band lookup.  Returns the band dict or None.")

        .def("list_peers",
             [](const HubAPI &self) -> py::list {
                 return pylabhub::scripting::detail::json_to_py(
                            self.list_peers())
                     .cast<py::list>();
             },
             "List of all federation peer hubs.")

        .def("get_peer",
             [](const HubAPI &self, const std::string &hub_uid) -> py::object {
                 return pylabhub::scripting::detail::json_to_py(
                            self.get_peer(hub_uid));
             },
             py::arg("hub_uid"),
             "Single peer lookup by hub uid.  Returns the peer dict or "
             "None.")

        .def("query_metrics",
             [](const HubAPI &self,
                const std::vector<std::string> &categories) -> py::dict {
                 return pylabhub::scripting::detail::json_to_py(
                            self.query_metrics(categories))
                     .cast<py::dict>();
             },
             py::arg("categories") = std::vector<std::string>{},
             "Filtered metrics — categories is a list of category names "
             "(e.g. ['channels', 'counters']); empty/absent = all "
             "categories.  Same JSON shape metrics() produces.");
}
