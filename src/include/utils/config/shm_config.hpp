#pragma once
/**
 * @file shm_config.hpp
 * @brief ShmConfig — categorical config for shared-memory data plane.
 *
 * Parsed from directional JSON fields: <direction>_shm_enabled, etc.
 */

#include "utils/data_block_policy.hpp" // ConsumerSyncPolicy, parse_consumer_sync_policy
#include "utils/json_fwd.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace pylabhub::config
{

struct ShmConfig
{
    bool                     enabled{true};
    uint64_t                 secret{0};
    uint32_t                 slot_count{8};
    hub::ConsumerSyncPolicy  sync_policy{hub::ConsumerSyncPolicy::Sequential};
};

/// Parse SHM config for a given direction.
/// @param j         Root JSON object.
/// @param direction "in" or "out".
/// @param tag       Context tag for error messages.
inline ShmConfig parse_shm_config(const nlohmann::json &j,
                                    const char *direction,
                                    const char *tag)
{
    const std::string pfx = std::string(direction) + "_";

    ShmConfig sc;
    sc.enabled    = j.value(pfx + "shm_enabled", true);
    sc.secret     = j.value(pfx + "shm_secret", uint64_t{0});
    sc.slot_count = j.value(pfx + "shm_slot_count", uint32_t{8});

    const std::string sync_str = j.value(pfx + "shm_sync_policy", std::string{"sequential"});
    sc.sync_policy = ::pylabhub::parse_consumer_sync_policy(sync_str);

    if (sc.enabled && sc.slot_count == 0)
        throw std::invalid_argument(
            std::string(tag) + ": '" + pfx + "shm_slot_count' must be > 0 when SHM is enabled");

    return sc;
}

} // namespace pylabhub::config
