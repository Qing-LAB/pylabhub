#pragma once
/**
 * @file pylabhub_module.hpp
 * @brief Internal wiring API for the `pylabhub` embedded Python module.
 *
 * Called from `hubshell.cpp` after BrokerService is ready, to connect the
 * `pylabhub.channels()` Python function to the live BrokerService registry.
 *
 * Not part of the public pylabhub-utils ABI.
 */

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <functional>
#include <vector>

namespace pylabhub::hub_python
{

/**
 * @brief Register the callback invoked by `pylabhub.channels()` in the admin shell.
 *
 * Must be called after the PythonInterpreter lifecycle module has started and
 * after BrokerService is running. The callback is invoked from the admin shell
 * worker thread with the GIL held; it should release the GIL if it needs to
 * acquire any C++ locks.
 *
 * @param cb  Returns a list of channel-info dicts; each dict contains at
 *            minimum "name", "schema_hash", "consumer_count", "producer_pid".
 *            Pass a default-constructed function to deregister.
 */
void set_channels_callback(std::function<std::vector<pybind11::dict>()> cb);

} // namespace pylabhub::hub_python
