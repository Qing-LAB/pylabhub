#pragma once
/**
 * @file channel_pattern.hpp
 * @brief Canonical ChannelPattern enum shared by Messenger and BrokerService.
 *
 * Defined once here to avoid duplication between the public Messenger API
 * (pylabhub::hub) and the broker-internal ChannelRegistry (pylabhub::broker).
 * BrokerService and ChannelRegistry import this via a type alias.
 */

namespace pylabhub::hub
{

/// ZMQ socket pattern for the producer–consumer data channel.
enum class ChannelPattern
{
    PubSub,   ///< Producer XPUB (binds), consumers SUB  (connect) — one-to-many streaming.
    Pipeline, ///< Producer PUSH  (binds), consumers PULL (connect) — load-balanced pipeline.
    Bidir,    ///< Producer ROUTER (binds), consumer DEALER (connect) — full bidirectional.
};

} // namespace pylabhub::hub
