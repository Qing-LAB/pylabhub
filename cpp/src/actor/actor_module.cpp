/**
 * @file actor_module.cpp
 * @brief Embedded Python module "pylabhub_actor".
 *
 * Exposes:
 *   - `ActorRoleAPI` class binding for Python scripts
 *   - `SharedSpinLockPy` class binding
 *
 * ## Script interface (module-convention)
 *
 * Scripts are Python modules (not flat `.py` files).  The actor host imports
 * the module once and looks up well-known function names by attribute access:
 *
 * @code{.py}
 *   # my_actor/sensor_node.py
 *   import pylabhub_actor as actor
 *
 *   def on_init(api: actor.ActorRoleAPI):
 *       api.log('info', "sensor_node starting")
 *
 *   def on_iteration(slot, flexzone, messages, api: actor.ActorRoleAPI) -> bool:
 *       """
 *       Called every loop iteration.
 *
 *       slot      — writable/read-only ctypes struct into SHM slot, or None
 *                   (Messenger trigger or timeout).
 *       flexzone  — persistent ctypes struct for the role's flexzone, or None.
 *       messages  — list of (sender: str, data: bytes) tuples from the ZMQ
 *                   incoming queue drained since the last iteration.
 *       api       — ActorRoleAPI proxy (this role's context).
 *
 *       Return value (producer only): True/None = commit, False = discard.
 *       Consumer return value is ignored.
 *       """
 *       if slot is not None:
 *           slot.ts = ...
 *       for sender, data in messages:
 *           api.broadcast(data)
 *       return True
 *
 *   def on_stop(api: actor.ActorRoleAPI):
 *       api.log('info', "sensor_node stopping")
 * @endcode
 *
 * ## Function lookup
 *
 * The host looks up `on_init`, `on_iteration`, and `on_stop` by name from the
 * imported module object.  Missing names are silently skipped (no error).
 * All three receive `api` as their only or last argument.
 */
#include "actor_api.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace pylabhub::actor;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
PYBIND11_EMBEDDED_MODULE(pylabhub_actor, m)
{
    m.doc() =
        "pylabhub actor — ActorRoleAPI binding for module-convention scripts.\n\n"
        "Script interface:\n"
        "  def on_init(api): ...\n"
        "  def on_iteration(slot, flexzone, messages, api) -> bool: ...\n"
        "  def on_stop(api): ...\n\n"
        "The host imports the script as a Python module and looks up these\n"
        "function names by attribute access.  Missing names are silently skipped.\n\n"
        "on_iteration is called every loop iteration:\n"
        "  slot      -- ctypes struct into SHM slot, or None (Messenger / timeout)\n"
        "  flexzone  -- persistent ctypes struct for the role's flexzone, or None\n"
        "  messages  -- list of (sender: str, data: bytes) from the incoming queue\n"
        "  api       -- ActorRoleAPI proxy for this role\n\n"
        "Producer return: True/None = commit slot; False = discard.\n"
        "Consumer return: ignored.";

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
        "  actor_name() -> str             — human-readable actor name\n"
        "  channel() -> str                — channel name this role operates on\n"
        "  broker() -> str                 — configured broker endpoint (informational)\n"
        "  kind() -> str                   — 'producer' or 'consumer'\n"
        "  log_level() -> str              — configured log level (debug/info/warn/error)\n"
        "  script_dir() -> str             — absolute base directory of the actor script\n"
        "  stop()                          — request actor shutdown (all roles)\n"
        "  set_critical_error()            — latch critical error flag + request shutdown\n"
        "  critical_error() -> bool        — True after set_critical_error() was called\n"
        "  flexzone() -> object            — persistent flexzone object, or None\n\n"
        "## Producer roles\n"
        "  broadcast(data: bytes) -> bool  — ZMQ broadcast to all consumers\n"
        "  send(id: str, data: bytes) -> bool — ZMQ unicast to one consumer\n"
        "  consumers() -> list             — ZMQ identities of connected consumers\n"
        "  update_flexzone_checksum() -> bool — recompute and store BLAKE2b\n\n"
        "## Consumer roles\n"
        "  send_ctrl(data: bytes) -> bool  — send ctrl frame to producer\n"
        "  slot_valid() -> bool            — False if slot checksum failed\n"
        "  verify_flexzone_checksum() -> bool — verify SHM flexzone checksum\n"
        "  accept_flexzone_state() -> bool — accept current flexzone as valid\n\n"
        "## Diagnostics (read-only — collected by C++ host about the script)\n"
        "  script_error_count() -> int    — total Python exceptions in callbacks\n"
        "  loop_overrun_count() -> int    — write cycles where interval_ms deadline was exceeded\n"
        "  last_cycle_work_us() -> int    — µs of active work in the last write cycle\n")

        // Common
        .def("log",        &ActorRoleAPI::log,
             py::arg("level"), py::arg("msg"),
             "Log through the hub logger. level: 'debug'|'info'|'warn'|'error'")
        .def("uid",        &ActorRoleAPI::uid,
             "Return the actor's unique identifier (from 'actor.uid' in the JSON config).")
        .def("role_name",  &ActorRoleAPI::role_name,
             "Return this role's name (as declared in the JSON 'roles' map).")
        .def("actor_name", &ActorRoleAPI::actor_name,
             "Human-readable actor name (from 'actor.name' in config).")
        .def("channel",    &ActorRoleAPI::channel,
             "Channel name this role operates on.")
        .def("broker",     &ActorRoleAPI::broker,
             "Configured broker endpoint for this role (informational; may not be live).")
        .def("kind",       &ActorRoleAPI::kind,
             "Role kind: 'producer' or 'consumer'.")
        .def("log_level",  &ActorRoleAPI::log_level,
             "Configured log level (debug/info/warn/error).")
        .def("script_dir", &ActorRoleAPI::script_dir,
             "Absolute base directory prepended to sys.path for this actor's script.")
        .def("stop",       &ActorRoleAPI::stop,
             "Request actor shutdown. All roles will stop after their current callback.")
        .def("set_critical_error", &ActorRoleAPI::set_critical_error,
             "Latch the critical-error flag and request graceful shutdown.\n"
             "The current iteration completes before the loop exits.\n"
             "Once set, the flag is never cleared.\n\n"
             "Use this to signal unrecoverable script errors:\n"
             "  def on_iteration(slot, fz, msgs, api):\n"
             "      if something_bad:\n"
             "          api.set_critical_error()\n"
             "          return False")
        .def("critical_error", &ActorRoleAPI::critical_error,
             "True if set_critical_error() has been called for this role.\n"
             "Reflects the latch state — never resets within a role run.")
        .def("flexzone", &ActorRoleAPI::flexzone,
             "Return the persistent flexzone Python object for this role, or None\n"
             "if no flexzone is configured or SHM is not connected.\n\n"
             "The flexzone is always available as the second positional argument\n"
             "to on_iteration(slot, flexzone, messages, api).  This getter is\n"
             "provided for convenience when flexzone needs to be accessed outside\n"
             "on_iteration (e.g. from on_init or on_stop).")

        // Producer
        .def("broadcast",  &ActorRoleAPI::broadcast, py::arg("data"),
             "Broadcast bytes to all connected consumers on the ZMQ data socket.")
        .def("send",       &ActorRoleAPI::send, py::arg("identity"), py::arg("data"),
             "Send bytes to a specific consumer identified by ZMQ identity string.")
        .def("consumers",  &ActorRoleAPI::consumers,
             "Return list of ZMQ identity strings for all connected consumers.")
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

        // Diagnostics — read-only from Python.
        .def("script_error_count", &ActorRoleAPI::script_error_count,
             "Total Python exceptions caught in any callback for this role\n"
             "(on_init, on_iteration, on_stop).\n"
             "Resets to zero when the role restarts.\n\n"
             "Example:\n"
             "  if api.script_error_count() > 100:\n"
             "      api.log('error', 'too many errors — stopping')\n"
             "      api.stop()")
        .def("loop_overrun_count", &ActorRoleAPI::loop_overrun_count,
             "Write cycles where the interval_ms deadline was already past at the\n"
             "timing check (no sleep was needed — the write body exceeded interval_ms).\n"
             "Producer-only; always 0 for consumers and when interval_ms <= 0.\n"
             "Resets to zero when the role restarts.")
        .def("last_cycle_work_us", &ActorRoleAPI::last_cycle_work_us,
             "Elapsed active-work time in microseconds for the most recently completed\n"
             "write cycle: acquire_write_slot + on_iteration + commit + checksum.\n"
             "0 until the first write completes. Producer-only; always 0 for consumers.")

        // Shared spinlocks
        .def("spinlock", &ActorRoleAPI::spinlock, py::arg("index"),
             "Return a SharedSpinLockPy for the SHM spinlock at the given index.\n"
             "Spinlocks are shared between producer and all consumers on the same channel.\n"
             "Index must be in [0, spinlock_count()). Raises ValueError if SHM not configured.")
        .def("spinlock_count", &ActorRoleAPI::spinlock_count,
             "Number of available shared spinlock slots (8 in the current layout).\n"
             "Returns 0 if SHM is not configured for this role.")

        // Timing metrics (HEP-CORE-0008)
        .def("metrics", &ActorRoleAPI::metrics,
             "Returns a dict of timing metrics for this role.\n\n"
             "Keys (Domains 2+3 from DataBlock Pimpl):\n"
             "  context_elapsed_us : int — microseconds since first slot acquisition\n"
             "  iteration_count    : int — successful slot acquisitions this run\n"
             "  last_iteration_us  : int — start-to-start time between last two acquires (us)\n"
             "  max_iteration_us   : int — peak start-to-start time this run (us)\n"
             "  last_slot_wait_us  : int — time blocked waiting for a free slot (us)\n"
             "  overrun_count      : int — acquire cycles exceeding period_ms target\n"
             "  last_slot_work_us  : int — time from acquire to release (user code + overhead)\n"
             "  period_ms          : int — configured target period (0 = MaxRate)\n\n"
             "Key (Domain 4 — script supervision):\n"
             "  script_error_count : int — unhandled Python exceptions in callbacks\n\n"
             "All values reset when the role restarts (clear_metrics is called at start).\n"
             "See HEP-CORE-0008 for the metric domain model.");
}
