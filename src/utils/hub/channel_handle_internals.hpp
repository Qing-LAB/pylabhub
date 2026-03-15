// src/utils/channel_handle_internals.hpp
//
// INTERNAL HEADER — not installed, not part of the public API.
//
// Provides direct ZMQ socket access for active service classes (hub_producer,
// hub_consumer). Each socket must be used by exactly ONE thread after start();
// the caller is responsible for thread-ownership correctness.
#pragma once

#include "utils/channel_handle.hpp"
#include "cppzmq/zmq.hpp"

namespace pylabhub::hub
{

/// Returns a mutable pointer to the ctrl socket (ROUTER for producer, DEALER for consumer).
/// Returns nullptr if the handle has no ctrl socket.
/// After Producer/Consumer::start(), the peer_thread/ctrl_thread has EXCLUSIVE ownership.
inline zmq::socket_t *channel_handle_ctrl_socket(ChannelHandle &h)
{
    return static_cast<zmq::socket_t *>(h.internal_ctrl_socket_ptr());
}

/// Returns a mutable pointer to the data socket (XPUB/PUSH for producer, SUB/PULL for consumer).
/// Returns nullptr if the handle has no data socket (e.g., Bidir pattern).
/// After start(), data_thread (consumer) or main-thread-with-mutex (producer) has ownership.
inline zmq::socket_t *channel_handle_data_socket(ChannelHandle &h)
{
    return static_cast<zmq::socket_t *>(h.internal_data_socket_ptr());
}

} // namespace pylabhub::hub
