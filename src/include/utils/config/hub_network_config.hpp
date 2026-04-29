#pragma once
/**
 * @file hub_network_config.hpp
 * @brief HubNetworkConfig — broker socket configuration for the hub.
 *
 * Parsed from the top-level `"network"` JSON sub-object (HEP-CORE-0033 §6.2).
 * Owns the broker ROUTER endpoint and bind/connect mode (the broker normally
 * binds — connect is supported for federation experiments).  Strict key
 * whitelist per HEP-CORE-0033 §6.3.
 */

#include "utils/json_fwd.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct HubNetworkConfig
{
    std::string broker_endpoint{"tcp://0.0.0.0:5570"};  ///< ZMQ ROUTER endpoint
    bool        broker_bind{true};                       ///< true → bind, false → connect
    int32_t     zmq_io_threads{1};                       ///< zmq::context_t IO threads
};

inline HubNetworkConfig parse_hub_network_config(const nlohmann::json &j)
{
    HubNetworkConfig nc;  // defaults if section absent
    if (!j.contains("network"))
        return nc;
    if (!j["network"].is_object())
        throw std::runtime_error("hub: 'network' must be an object");

    const auto &sect = j["network"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "broker_endpoint" && k != "broker_bind" && k != "zmq_io_threads")
            throw std::runtime_error("hub: unknown config key 'network." + k + "'");
    }

    nc.broker_endpoint = sect.value("broker_endpoint", nc.broker_endpoint);
    nc.broker_bind     = sect.value("broker_bind",     nc.broker_bind);
    nc.zmq_io_threads  = sect.value("zmq_io_threads",  nc.zmq_io_threads);
    if (nc.zmq_io_threads < 1)
        throw std::runtime_error(
            "hub: 'network.zmq_io_threads' must be >= 1 (got " +
            std::to_string(nc.zmq_io_threads) + ")");
    return nc;
}

} // namespace pylabhub::config
