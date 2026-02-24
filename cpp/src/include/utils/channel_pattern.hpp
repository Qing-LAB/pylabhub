#pragma once
/**
 * @file channel_pattern.hpp
 * @brief Canonical ChannelPattern enum shared by Messenger and BrokerService.
 *
 * Defined once here to avoid duplication between the public Messenger API
 * (pylabhub::hub) and the broker-internal ChannelRegistry (pylabhub::broker).
 * BrokerService and ChannelRegistry import this via a type alias.
 *
 * Also provides the canonical string conversion helpers channel_pattern_to_str()
 * and channel_pattern_from_str() so that both Messenger and BrokerService use
 * identical wire names ("PubSub", "Pipeline", "Bidir") without duplication.
 */

#include <string>

namespace pylabhub::hub
{

/// ZMQ socket pattern for the producer–consumer data channel.
enum class ChannelPattern
{
    PubSub,   ///< Producer XPUB (binds), consumers SUB  (connect) — one-to-many streaming.
    Pipeline, ///< Producer PUSH  (binds), consumers PULL (connect) — load-balanced pipeline.
    Bidir,    ///< Producer ROUTER (binds), consumer DEALER (connect) — full bidirectional.
};

/// Convert ChannelPattern to its JSON wire / config string representation.
inline constexpr const char* channel_pattern_to_str(ChannelPattern p) noexcept
{
    switch (p)
    {
    case ChannelPattern::Pipeline: return "Pipeline";
    case ChannelPattern::Bidir:    return "Bidir";
    default:                       return "PubSub";
    }
}

/// Parse ChannelPattern from a JSON wire / config string. Returns PubSub on unknown values.
inline ChannelPattern channel_pattern_from_str(const std::string& s) noexcept
{
    if (s == "Pipeline") { return ChannelPattern::Pipeline; }
    if (s == "Bidir")    { return ChannelPattern::Bidir; }
    return ChannelPattern::PubSub;
}

} // namespace pylabhub::hub
