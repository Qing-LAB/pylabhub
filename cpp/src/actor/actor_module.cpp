/**
 * @file actor_module.cpp
 * @brief Embedded Python module "pylabhub_actor".
 *
 * Exposes:
 *   - Decorator factory functions for per-role callback registration
 *   - `ActorRoleAPI` class binding for Python type hints
 *   - `_clear_dispatch_table()` utility (called by ActorHost before each script import)
 *
 * ## Python usage
 *
 * @code{.py}
 *   import pylabhub_actor as actor
 *
 *   # ── Producer role ──────────────────────────────────────────────────────
 *   @actor.on_init("raw_out")
 *   def raw_out_init(flexzone, api: actor.ActorRoleAPI):
 *       flexzone.device_id = 42
 *       api.update_flexzone_checksum()
 *
 *   @actor.on_write("raw_out")
 *   def write_raw(slot, flexzone, api: actor.ActorRoleAPI) -> bool:
 *       slot.ts = time.time()
 *       return True        # True/None = commit, False = discard
 *
 *   @actor.on_message("raw_out")
 *   def raw_out_ctrl(sender: str, data: bytes, api: actor.ActorRoleAPI):
 *       api.send(sender, b"ack")
 *
 *   @actor.on_stop("raw_out")          # decorator for producer stop
 *   def raw_out_stop(flexzone, api): ...
 *
 *   # ── Consumer role ──────────────────────────────────────────────────────
 *   @actor.on_init("cfg_in")
 *   def cfg_in_init(flexzone, api: actor.ActorRoleAPI):
 *       api.log('info', f"device_id={flexzone.device_id}")
 *
 *   @actor.on_read("cfg_in")
 *   def read_cfg(slot, flexzone, api: actor.ActorRoleAPI, *, timed_out: bool = False):
 *       if timed_out:
 *           api.send_ctrl(b"heartbeat")
 *           return
 *       process(slot.setpoint)
 *
 *   @actor.on_data("cfg_in")           # ZMQ broadcast frames
 *   def zmq_data(data: bytes, api: actor.ActorRoleAPI): ...
 *
 *   @actor.on_stop_c("cfg_in")         # decorator for consumer stop
 *   def cfg_in_stop(flexzone, api): ...
 * @endcode
 *
 * ## Decorator mechanics
 *
 * `actor.on_write("role")` is called at import time with the role name.
 * It returns a decorator that stores the function in the dispatch table
 * and returns the function unchanged. This is the standard decorator-factory
 * pattern. The decorator itself is never called at runtime — only at import.
 *
 * Registering a duplicate handler for the same event+role raises RuntimeError.
 */
#include "actor_api.hpp"
#include "actor_dispatch_table.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace pylabhub::actor;

// ============================================================================
// Global dispatch table (owned here; accessed via get_dispatch_table())
// ============================================================================

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static ActorDispatchTable g_dispatch_table;

ActorDispatchTable &pylabhub::actor::get_dispatch_table()
{
    return g_dispatch_table;
}

// ============================================================================
// Module definition
// ============================================================================

namespace
{

/// Build a decorator factory for a given event map.
/// Usage: m.def("on_write", make_factory(g_dispatch_table.on_write), ...);
///
/// When the script calls `@actor.on_write("role")`:
///   1. `actor.on_write("role")` calls this factory with role_name = "role".
///   2. Returns a decorator callable.
///   3. The decorator stores the user function in the event map and returns it.
///
/// Returns a plain C++ lambda so pybind11 can deduce its signature for m.def().
auto make_factory(std::unordered_map<std::string, py::object> &event_map)
{
    return [&event_map](const std::string &role_name) -> py::object
    {
        return py::cpp_function(
            [&event_map, role_name](py::object fn) -> py::object
            {
                if (event_map.count(role_name) != 0)
                {
                    throw std::runtime_error(
                        "pylabhub_actor: duplicate handler for role '"
                        + role_name + "' — each event+role pair may only "
                        "have one registered callback");
                }
                event_map[role_name] = fn;
                return fn;
            });
    };
}

} // anonymous namespace

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
PYBIND11_EMBEDDED_MODULE(pylabhub_actor, m)
{
    m.doc() =
        "pylabhub actor — per-role callback decorators and ActorRoleAPI binding.\n\n"
        "Usage:\n"
        "  import pylabhub_actor as actor\n\n"
        "  @actor.on_write('my_producer')\n"
        "  def write(slot, flexzone, api) -> bool: ...\n\n"
        "  @actor.on_read('my_consumer')\n"
        "  def read(slot, flexzone, api, *, timed_out=False): ...\n\n"
        "All decorators register handlers at import time. The decorator itself\n"
        "adds zero per-cycle runtime cost; the C++ dispatch table lookup is ~50 ns.";

    // ── Decorator factories ────────────────────────────────────────────────────
    //
    // Producer decorators
    m.def("on_init",    make_factory(g_dispatch_table.on_init),
          py::arg("role"),
          "Register on_init(flexzone, api) for a producer or consumer role.\n"
          "Called once after SHM is ready, before the write/read loop starts.");

    m.def("on_write",   make_factory(g_dispatch_table.on_write),
          py::arg("role"),
          "Register on_write(slot, flexzone, api) -> bool for a producer role.\n"
          "Return True or None to commit the slot; False to discard.\n"
          "slot is a writable ctypes struct — valid ONLY during this call.");

    m.def("on_message", make_factory(g_dispatch_table.on_message),
          py::arg("role"),
          "Register on_message(sender: str, data: bytes, api) for a producer role.\n"
          "Called when any consumer sends a ZMQ ctrl frame to this producer.");

    m.def("on_stop",    make_factory(g_dispatch_table.on_stop_p),
          py::arg("role"),
          "Register on_stop(flexzone, api) for a producer role.\n"
          "Called once after the write loop exits.");

    // Consumer decorators
    m.def("on_read",    make_factory(g_dispatch_table.on_read),
          py::arg("role"),
          "Register on_read(slot, flexzone, api, *, timed_out=False) for a consumer role.\n"
          "slot is a read-only ctypes struct (zero-copy from_buffer on readonly memoryview).\n"
          "Field writes raise TypeError. Valid ONLY during this call.\n"
          "When timed_out=True the slot is None (timeout_ms elapsed without a new slot).");

    m.def("on_data",    make_factory(g_dispatch_table.on_data),
          py::arg("role"),
          "Register on_data(data: bytes, api) for a consumer role.\n"
          "Called for each ZMQ broadcast frame received from the producer.");

    m.def("on_stop_c",  make_factory(g_dispatch_table.on_stop_c),
          py::arg("role"),
          "Register on_stop(flexzone, api) for a consumer role.\n"
          "Called once after the read loop exits.\n"
          "Note: use @actor.on_stop() for producer roles, @actor.on_stop_c() for consumers.");

    // ── Utility ───────────────────────────────────────────────────────────────
    m.def("_clear_dispatch_table",
          []() { g_dispatch_table.clear(); },
          "Clear all registered handlers. Called by ActorHost before each script import.");

    m.def("_registered_roles",
          [](const std::string &event) -> py::list
          {
              py::list result;
              const ActorDispatchTable &tbl = g_dispatch_table;
              const std::unordered_map<std::string, py::object> *map = nullptr;
              if (event == "on_write")   map = &tbl.on_write;
              else if (event == "on_read")    map = &tbl.on_read;
              else if (event == "on_init")    map = &tbl.on_init;
              else if (event == "on_data")    map = &tbl.on_data;
              else if (event == "on_message") map = &tbl.on_message;
              if (map != nullptr)
                  for (const auto &[k, _v] : *map)
                      result.append(k);
              return result;
          },
          py::arg("event"),
          "Return list of role names that have a handler for the given event string.");

    // ── SharedSpinLockPy class binding ─────────────────────────────────────────
    py::class_<SharedSpinLockPy>(m, "SharedSpinLockPy",
        "Cross-process spinlock backed by shared memory.\n\n"
        "Returned by api.spinlock(idx). Supports context manager protocol\n"
        "and explicit lock/unlock.\n\n"
        "The spinlock is shared between all processes that open the same\n"
        "SHM channel — producer and consumers share the same 8 spinlock slots.\n\n"
        "Usage:\n"
        "  # Context manager (always releases even on exception)\n"
        "  with api.spinlock(0):\n"
        "      flexzone.counter += 1\n"
        "      api.update_flexzone_checksum()\n\n"
        "  # Explicit lock/unlock\n"
        "  lk = api.spinlock(1)\n"
        "  lk.lock()\n"
        "  try:\n"
        "      flexzone.calibration = new_value\n"
        "  finally:\n"
        "      lk.unlock()\n\n"
        "  # Non-blocking attempt\n"
        "  lk = api.spinlock(2)\n"
        "  if lk.try_lock_for(100):  # 100 ms timeout\n"
        "      try: ...\n"
        "      finally: lk.unlock()")
        .def("lock",   &SharedSpinLockPy::lock,
             "Acquire the spinlock (blocking until acquired).")
        .def("unlock", &SharedSpinLockPy::unlock,
             "Release the spinlock. Raises RuntimeError if not held by this process.")
        .def("try_lock_for", &SharedSpinLockPy::try_lock_for, py::arg("timeout_ms"),
             "Try to acquire within timeout_ms. Returns True on success, False on timeout.")
        .def("is_locked_by_current_process", &SharedSpinLockPy::is_locked_by_current_process,
             "True if this process currently holds the spinlock.")
        .def("__enter__", &SharedSpinLockPy::enter, py::return_value_policy::reference,
             "Acquire the spinlock (context manager entry).")
        .def("__exit__",  &SharedSpinLockPy::exit,
             py::arg("exc_type"), py::arg("exc_val"), py::arg("exc_tb"),
             "Release the spinlock (context manager exit).");

    // ── ActorRoleAPI class binding ─────────────────────────────────────────────
    py::class_<ActorRoleAPI>(m, "ActorRoleAPI",
        "Proxy to C++ actor services for one named role.\n\n"
        "One instance per active role. Passed by reference to every callback\n"
        "of that role. All methods dispatch immediately to C++ — no Python state.\n\n"
        "## Common\n"
        "  log(level, msg)                 — log through the hub logger\n"
        "  uid() -> str                    — actor's own uid\n"
        "  role_name() -> str              — this role's name\n"
        "  stop()                          — request actor shutdown (all roles)\n\n"
        "## Producer roles\n"
        "  broadcast(data: bytes) -> bool  — ZMQ broadcast to all consumers\n"
        "  send(id: str, data: bytes) -> bool — ZMQ unicast to one consumer\n"
        "  consumers() -> list             — ZMQ identities of connected consumers\n"
        "  trigger_write()                 — wake write loop (interval_ms=-1 only)\n"
        "  update_flexzone_checksum() -> bool — recompute and store BLAKE2b\n\n"
        "## Consumer roles\n"
        "  send_ctrl(data: bytes) -> bool  — send ctrl frame to producer\n"
        "  slot_valid() -> bool            — False if slot checksum failed\n"
        "  verify_flexzone_checksum() -> bool — verify SHM flexzone checksum\n"
        "  accept_flexzone_state() -> bool — accept current flexzone as valid\n")

        // Common
        .def("log",        &ActorRoleAPI::log,
             py::arg("level"), py::arg("msg"),
             "Log through the hub logger. level: 'debug'|'info'|'warn'|'error'")
        .def("uid",        &ActorRoleAPI::uid,
             "Return the actor's unique identifier (from 'actor.uid' in the JSON config).")
        .def("role_name",  &ActorRoleAPI::role_name,
             "Return this role's name (as declared in the JSON 'roles' map).")
        .def("stop",       &ActorRoleAPI::stop,
             "Request actor shutdown. All roles will stop after their current callback.")

        // Producer
        .def("broadcast",  &ActorRoleAPI::broadcast, py::arg("data"),
             "Broadcast bytes to all connected consumers on the ZMQ data socket.")
        .def("send",       &ActorRoleAPI::send, py::arg("identity"), py::arg("data"),
             "Send bytes to a specific consumer identified by ZMQ identity string.")
        .def("consumers",  &ActorRoleAPI::consumers,
             "Return list of ZMQ identity strings for all connected consumers.")
        .def("trigger_write", &ActorRoleAPI::trigger_write,
             "Wake the write loop. Only has effect when interval_ms == -1.")
        .def("update_flexzone_checksum", &ActorRoleAPI::update_flexzone_checksum,
             "Recompute and store the SHM flexzone BLAKE2b checksum (producer).\n"
             "Call from on_init and after any write that modifies flexzone fields.")

        // Consumer
        .def("send_ctrl",  &ActorRoleAPI::send_ctrl, py::arg("data"),
             "Send a ctrl bytes frame to the producer.")
        .def("slot_valid", &ActorRoleAPI::slot_valid,
             "True when the current slot passed its checksum check.\n"
             "False when checksum failed and on_checksum_fail='pass' policy applies.")
        .def("verify_flexzone_checksum", &ActorRoleAPI::verify_flexzone_checksum,
             "Verify the SHM flexzone against its stored BLAKE2b checksum.")
        .def("accept_flexzone_state",    &ActorRoleAPI::accept_flexzone_state,
             "Accept the current SHM flexzone content as valid (consumer override).\n"
             "Subsequent actor-level checks compare against this snapshot.")

        // Shared spinlocks
        .def("spinlock", &ActorRoleAPI::spinlock, py::arg("index"),
             "Return a SharedSpinLockPy for the SHM spinlock at the given index.\n"
             "Spinlocks are shared between producer and all consumers on the same channel.\n"
             "Index must be in [0, spinlock_count()). Raises ValueError if SHM not configured.")
        .def("spinlock_count", &ActorRoleAPI::spinlock_count,
             "Number of available shared spinlock slots (8 in the current layout).\n"
             "Returns 0 if SHM is not configured for this role.");
}
