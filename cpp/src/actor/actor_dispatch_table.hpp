#pragma once
/**
 * @file actor_dispatch_table.hpp
 * @brief Shared dispatch table populated by Python decorator registration.
 *
 * `actor_module.cpp` owns the global `ActorDispatchTable` instance.
 * `actor_host.cpp` reads it after importing the Python script.
 *
 * The table is populated at Python import time when the script executes
 * decorator calls like:
 *
 * @code{.py}
 *   import pylabhub_actor as actor
 *
 *   @actor.on_write("raw_out")
 *   def write_raw(slot, fz, api) -> bool: ...
 * @endcode
 *
 * The decorator `actor.on_write("raw_out")` is a pybind11 cpp_function that:
 *   1. Receives the role name ("raw_out") as a string argument.
 *   2. Returns a decorator callable.
 *   3. The returned decorator stores the function in dispatch_table.on_write["raw_out"]
 *      and returns the function unchanged.
 *
 * Runtime cost per callback: one `unordered_map::find` (~50 ns) at call time.
 * The decorator machinery runs once at import time — zero per-cycle cost.
 */

#include <pybind11/pybind11.h>

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace py = pybind11;

namespace pylabhub::actor
{

/**
 * @struct ActorDispatchTable
 * @brief Per-role handler maps, populated by @actor.on_*() decorators.
 *
 * Each map is keyed by the role name string (as declared in the JSON "roles"
 * block). Values are callable Python objects stored during script import.
 *
 * Producer roles use: on_init, on_write, on_message, on_stop_p
 * Consumer roles use: on_init, on_read, on_data, on_stop_c
 */
struct ActorDispatchTable
{
    // Shared (both producer and consumer roles use on_init / on_stop)
    std::unordered_map<std::string, py::object> on_init;    ///< (flexzone, api)
    std::unordered_map<std::string, py::object> on_stop_p;  ///< producer (flexzone, api)
    std::unordered_map<std::string, py::object> on_stop_c;  ///< consumer (flexzone, api)

    // Producer
    std::unordered_map<std::string, py::object> on_write;   ///< (slot, fz, api) -> bool
    std::unordered_map<std::string, py::object> on_message; ///< (sender, data, api)

    // Consumer
    std::unordered_map<std::string, py::object> on_read;    ///< (slot, fz, api, *, timed_out=False)
    std::unordered_map<std::string, py::object> on_data;    ///< (data, api)

    void clear()
    {
        on_init.clear();
        on_stop_p.clear();
        on_stop_c.clear();
        on_write.clear();
        on_message.clear();
        on_read.clear();
        on_data.clear();
    }
};

/**
 * @brief Access the global dispatch table (owned by actor_module.cpp).
 *        Thread-safe: the table is only written at Python import time (under GIL),
 *        and only read after import while still under GIL.
 */
ActorDispatchTable &get_dispatch_table();

} // namespace pylabhub::actor
