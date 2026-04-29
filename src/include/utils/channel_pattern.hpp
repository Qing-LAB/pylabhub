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

/**
 * @enum ChannelPattern
 * @brief ZMQ socket topology for the producer–consumer data channel.
 *
 * **Where set:** Role config JSON (`channel_pattern` field, parsed by per-role
 *   config readers); sent to the broker in the REG_REQ JSON `"channel_pattern"`
 *   field; stored in `ChannelEntry` (HEP-CORE-0033 §8).
 * **Where applied:**
 *   - Producer role host: creates and binds the data socket with the
 *     appropriate ZMQ socket type based on this value.
 *   - BrokerService: includes pattern in CHANNEL_READY_NOTIFY so consumers
 *     know which socket type to create when connecting.
 *   - Consumer role host: creates matching ZMQ socket and connects
 *     to the endpoint advertised by the broker.
 *
 * | Value    | Producer socket | Consumer socket | Use case                               |
 * |----------|-----------------|-----------------|----------------------------------------|
 * | PubSub   | XPUB (binds)    | SUB (connects)  | 1:many broadcast; consumers may miss   |
 * |          |                 |                 | data if slower than producer (lossy).  |
 * | Pipeline | PUSH (binds)    | PULL (connects) | Load-balanced; exactly one consumer    |
 * |          |                 |                 | receives each message (no duplicates). |
 * | Bidir    | ROUTER (binds)  | DEALER(connects)| Full bidirectional; each consumer gets |
 * |          |                 |                 | all messages (addressed routing).      |
 *
 * JSON wire values: `"PubSub"` (default) | `"Pipeline"` | `"Bidir"`.
 * **Design doc:** HEP-CORE-0007-DataHub-Protocol-and-Policy.md §3.1
 */
enum class ChannelPattern
{
    PubSub,   ///< XPUB/SUB — one-to-many broadcast streaming (default)
    Pipeline, ///< PUSH/PULL — load-balanced single-consumer pipeline
    Bidir,    ///< ROUTER/DEALER — full bidirectional, per-consumer addressing
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
