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

/**
 * @brief Register the callback invoked by `pylabhub.close_channel()`.
 *
 * The callback receives a channel name and requests BrokerService to close it,
 * which sends CHANNEL_CLOSING_NOTIFY to all participants.
 */
void set_close_channel_callback(std::function<void(const std::string&)> cb);

/**
 * @brief Register the callback invoked by `pylabhub.broadcast_channel()`.
 *
 * The callback receives (channel_name, message, data) and requests
 * BrokerService to fan out a CHANNEL_BROADCAST_NOTIFY to all participants.
 */
void set_broadcast_channel_callback(
    std::function<void(const std::string&, const std::string&, const std::string&)> cb);

} // namespace pylabhub::hub_python
