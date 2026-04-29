#pragma once
/**
 * @file hub_state_config.hpp
 * @brief HubStateConfig — `HubState` retention + cap parameters.
 *
 * Parsed from the top-level `"state"` JSON sub-object (HEP-CORE-0033 §6.2 / §8).
 * Controls how long disconnected role entries linger before eviction and the
 * LRU cap that bounds the `disconnected` list.  Strict key whitelist per
 * HEP-CORE-0033 §6.3.
 */

#include "utils/json_fwd.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

/// Sentinel: `disconnected_grace_ms = kInfiniteGrace` → never auto-evict.
/// On the wire this is encoded as `-1` (HEP-0033 §6.3 sentinel rule).
inline constexpr int64_t kInfiniteGrace = std::numeric_limits<int64_t>::max();

struct HubStateConfig
{
    int64_t  disconnected_grace_ms{60000};       ///< -1 → kInfiniteGrace
    int32_t  max_disconnected_entries{1000};
};

inline HubStateConfig parse_hub_state_config(const nlohmann::json &j)
{
    HubStateConfig sc;
    if (!j.contains("state"))
        return sc;
    if (!j["state"].is_object())
        throw std::runtime_error("hub: 'state' must be an object");

    const auto &sect = j["state"];
    for (auto it = sect.begin(); it != sect.end(); ++it)
    {
        const auto &k = it.key();
        if (k != "disconnected_grace_ms" && k != "max_disconnected_entries")
            throw std::runtime_error("hub: unknown config key 'state." + k + "'");
    }

    int64_t grace = sect.value("disconnected_grace_ms", int64_t{60000});
    if (grace == -1)
        sc.disconnected_grace_ms = kInfiniteGrace;
    else if (grace < 0)
        throw std::runtime_error(
            "hub: 'state.disconnected_grace_ms' must be >= 0 or -1 "
            "(infinite); got " + std::to_string(grace));
    else
        sc.disconnected_grace_ms = grace;

    sc.max_disconnected_entries = sect.value("max_disconnected_entries",
                                              sc.max_disconnected_entries);
    if (sc.max_disconnected_entries < 0)
        throw std::runtime_error(
            "hub: 'state.max_disconnected_entries' must be >= 0 (got " +
            std::to_string(sc.max_disconnected_entries) + ")");
    return sc;
}

} // namespace pylabhub::config
