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

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

namespace py = pybind11;

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
                 // HubAPI::metrics() returns nlohmann::json — convert
                 // via Python's json.loads (same trick role-side
                 // ProducerAPI::metrics uses to avoid hand-rolling a
                 // json→py::dict converter; pybind11 itself doesn't
                 // know how to convert nlohmann::json natively).
                 auto j = self.metrics();
                 return py::module_::import("json")
                            .attr("loads")(j.dump())
                            .cast<py::dict>();
             },
             "Return a snapshot of the broker's metrics — channels / "
             "roles / bands / peers aggregates plus broker counters.\n"
             "Empty dict when called before HubHost::startup() or after "
             "shutdown() — same shape AdminService's query_metrics RPC "
             "uses (single source of truth via Phase 6.2b's "
             "hub_state_json serializers).");
}
