#pragma once
/**
 * @file monitoring_config.hpp
 * @brief MonitoringConfig — categorical config for peer/hub-dead monitoring.
 */

#include "utils/json_fwd.hpp"

#include <cstddef>
#include <cstdint>

namespace pylabhub::config
{

struct MonitoringConfig
{
    size_t ctrl_queue_max_depth{256};   ///< Max depth of ctrl send queue before oldest dropped.
    int    peer_dead_timeout_ms{30000}; ///< Peer silence timeout (ms). 0=disabled.
};

/// Parse monitoring fields from a JSON config object.
inline MonitoringConfig parse_monitoring_config(const nlohmann::json &j)
{
    MonitoringConfig mc;
    mc.ctrl_queue_max_depth = j.value("ctrl_queue_max_depth", size_t{256});
    mc.peer_dead_timeout_ms = j.value("peer_dead_timeout_ms", 30000);
    return mc;
}

} // namespace pylabhub::config
