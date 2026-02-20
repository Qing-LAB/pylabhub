/**
 * @file actor_module.cpp
 * @brief Embedded Python module "pylabhub_actor" — ActorAPI binding.
 *
 * Scripts can import this module for type hints:
 * @code{.py}
 *   import pylabhub_actor
 *
 *   def on_write(slot, flexzone, api: pylabhub_actor.ActorAPI) -> bool:
 *       api.log('info', "hello")
 *       return True
 * @endcode
 *
 * The `slot` and `flexzone` objects are plain ctypes struct instances or
 * numpy arrays — no import required to use them. The `api` object is an
 * instance of `ActorAPI` bound here.
 */
#include "actor_api.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace pylabhub::actor;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
PYBIND11_EMBEDDED_MODULE(pylabhub_actor, m)
{
    m.doc() = "pylabhub actor — API proxy bindings for producer and consumer scripts";

    py::class_<ActorAPI>(m, "ActorAPI",
        "Proxy to C++ actor services passed to every script callback.\n\n"
        "## Common\n"
        "  log(level, msg)                — log through the hub logger\n"
        "  consumers() -> list            — ZMQ identity strings of connected consumers\n\n"
        "## Producer\n"
        "  broadcast(data: bytes) -> bool — ZMQ broadcast to all consumers\n"
        "  send(id: str, data: bytes) -> bool  — ZMQ unicast to one consumer\n"
        "  update_flexzone_checksum() -> bool  — update SHM flexzone checksum\n\n"
        "## Consumer\n"
        "  send_ctrl(data: bytes) -> bool      — send ctrl frame to producer\n"
        "  slot_valid() -> bool               — True if slot checksum passed\n"
        "  verify_flexzone_checksum() -> bool  — verify SHM flexzone checksum\n"
        "  update_flexzone_checksum() -> bool  — accept current flexzone state\n")

        // ── Common ────────────────────────────────────────────────────────────
        .def("log", &ActorAPI::log,
             py::arg("level"), py::arg("msg"),
             "Log through the hub logger. level: 'debug'|'info'|'warn'|'error'")

        .def("consumers", &ActorAPI::consumers,
             "Return list of ZMQ identity strings for all connected consumers.")

        // ── Producer ──────────────────────────────────────────────────────────
        .def("broadcast", &ActorAPI::broadcast,
             py::arg("data"),
             "Broadcast bytes to all connected consumers on the ZMQ data socket.")

        .def("send", &ActorAPI::send,
             py::arg("identity"), py::arg("data"),
             "Send bytes to a specific consumer identified by ZMQ identity string.")

        .def("update_flexzone_checksum", &ActorAPI::update_flexzone_checksum,
             "Producer: compute and store BLAKE2b of SHM flexzone.\n"
             "Consumer: accept current SHM flexzone content as valid (local override).\n"
             "Returns False if SHM is not available or flexzone size is 0.")

        // ── Consumer ──────────────────────────────────────────────────────────
        .def("send_ctrl", &ActorAPI::send_ctrl,
             py::arg("data"),
             "Send a ctrl bytes frame to the producer.")

        .def("slot_valid", &ActorAPI::slot_valid,
             "True when the current slot passed its checksum check.\n"
             "False when checksum failed and on_checksum_fail='pass' policy applies.\n"
             "Always True when slot checksum is not enforced.")

        .def("verify_flexzone_checksum", &ActorAPI::verify_flexzone_checksum,
             "Verify the SHM flexzone against its stored BLAKE2b checksum.\n"
             "Returns False on mismatch or when SHM is not available.");
}
