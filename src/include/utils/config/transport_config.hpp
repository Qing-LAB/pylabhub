#pragma once
/**
 * @file transport_config.hpp
 * @brief TransportConfig — categorical config for data-plane transport.
 *
 * Parsed from directional JSON fields: <direction>_transport, etc.
 * Transport enum is defined here as the canonical location.
 */

#include "utils/hub_zmq_queue.hpp" // kZmqDefaultBufferDepth
#include "utils/json_fwd.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

/// Data-plane transport type.
enum class Transport : uint8_t
{
    Shm, ///< SHM-backed (ShmQueue), default.
    Zmq, ///< ZMQ PUSH/PULL-backed (ZmqQueue).
};

struct TransportConfig
{
    Transport   transport{Transport::Shm};
    std::string zmq_endpoint;                                    ///< Required when transport==Zmq.
    bool        zmq_bind{true};                                  ///< Bind (true) or connect (false).
    size_t      zmq_buffer_depth{hub::kZmqDefaultBufferDepth};   ///< Internal ring depth.
    std::string zmq_overflow_policy{"drop"};                     ///< "drop" or "block".
    std::string zmq_packing{"aligned"};                          ///< "aligned" or "packed".
};

/// Parse transport config for a given direction.
/// @param j         Root JSON object.
/// @param direction "in" or "out".
/// @param tag       Context tag for error messages.
inline TransportConfig parse_transport_config(const nlohmann::json &j,
                                               const char *direction,
                                               const char *tag)
{
    const std::string pfx = std::string(direction) + "_";

    TransportConfig tc;

    const std::string transport_str = j.value(pfx + "transport", std::string{"shm"});
    if (transport_str == "shm")
        tc.transport = Transport::Shm;
    else if (transport_str == "zmq")
        tc.transport = Transport::Zmq;
    else if (!transport_str.empty())
        throw std::invalid_argument(
            std::string(tag) + ": '" + pfx + "transport' must be \"shm\" or \"zmq\", got: \"" +
            transport_str + "\"");

    tc.zmq_endpoint   = j.value(pfx + "zmq_endpoint", std::string{});
    tc.zmq_bind       = j.value(pfx + "zmq_bind", true);
    tc.zmq_buffer_depth = j.value(pfx + "zmq_buffer_depth",
                                   static_cast<size_t>(hub::kZmqDefaultBufferDepth));
    tc.zmq_overflow_policy = j.value(pfx + "zmq_overflow_policy", std::string{"drop"});
    tc.zmq_packing    = j.value(pfx + "zmq_packing", std::string{"aligned"});

    // Validate ZMQ fields when transport is ZMQ.
    if (tc.transport == Transport::Zmq)
    {
        if (tc.zmq_endpoint.empty())
            throw std::invalid_argument(
                std::string(tag) + ": '" + pfx + "zmq_endpoint' is required when " +
                pfx + "transport is \"zmq\"");

        if (tc.zmq_buffer_depth == 0)
            throw std::invalid_argument(
                std::string(tag) + ": '" + pfx + "zmq_buffer_depth' must be > 0");

        if (tc.zmq_packing != "aligned" && tc.zmq_packing != "packed")
            throw std::invalid_argument(
                std::string(tag) + ": '" + pfx + "zmq_packing' must be \"aligned\" or \"packed\", got: \"" +
                tc.zmq_packing + "\"");

        if (tc.zmq_overflow_policy != "drop" && tc.zmq_overflow_policy != "block")
            throw std::invalid_argument(
                std::string(tag) + ": '" + pfx + "zmq_overflow_policy' must be \"drop\" or \"block\", got: \"" +
                tc.zmq_overflow_policy + "\"");
    }

    return tc;
}

} // namespace pylabhub::config
