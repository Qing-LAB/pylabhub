// src/utils/channel_handle_factory.hpp
//
// INTERNAL HEADER — not installed, not part of the public API.
//
// Declares factory helpers used by Messenger to build ChannelHandle objects
// without exposing the ChannelHandleImpl definition to messenger.cpp.
#pragma once

#include "utils/channel_handle.hpp"

#include "cppzmq/zmq.hpp"

namespace pylabhub::hub
{

/// Build a producer-side ChannelHandle from pre-bound sockets.
/// @p data_sock_or_dummy is moved only when @p has_data_sock is true.
ChannelHandle make_producer_handle(const std::string &channel,
                                   ChannelPattern     pattern,
                                   bool               has_shm,
                                   zmq::socket_t    &&ctrl_sock,
                                   zmq::socket_t    &&data_sock_or_dummy,
                                   bool               has_data_sock);

/// Build a consumer-side ChannelHandle from pre-connected sockets.
/// @p data_sock_or_dummy is moved only when @p has_data_sock is true.
ChannelHandle make_consumer_handle(const std::string &channel,
                                   ChannelPattern     pattern,
                                   bool               has_shm,
                                   zmq::socket_t    &&ctrl_sock,
                                   zmq::socket_t    &&data_sock_or_dummy,
                                   bool               has_data_sock);

} // namespace pylabhub::hub
