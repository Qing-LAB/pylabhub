#pragma once
/**
 * @file hub_consumer.hpp
 * @brief ConsumerOptions — configuration for a role's Rx-side queue.
 *
 * Post-L3.γ A6.3: the hub::Consumer class is GONE. This header now exposes
 * only the ConsumerOptions data struct, which role hosts hand to
 * RoleAPIBase::build_rx_queue(). The factory logic lives inside
 * RoleAPIBase::Impl — ownership of the resulting ShmQueue/ZmqQueue is on
 * the RoleAPIBase itself.
 */

#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"
#include "utils/data_block.hpp"
#include "utils/hub_zmq_queue.hpp"  // ZmqSchemaField, kZmqDefaultBufferDepth
#include "utils/schema_library.hpp"
#include "utils/schema_types.hpp"   // SchemaSpec (slot_spec in ConsumerOptions)

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ConsumerOptions
{
    std::string channel_name;

    uint64_t shm_shared_secret{0};
    std::string consumer_uid{};
    std::string consumer_name{};

    // Schema (single source of truth; expected_schema_hash is
    // auto-computed from these at build_rx_queue time).
    SchemaSpec slot_spec{};
    SchemaSpec fz_spec{};

    // Transport (HEP-CORE-0021)
    std::string data_transport{"shm"};
    std::string shm_name{};
    std::string zmq_node_endpoint{};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};

    // Queue policy
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};

    /// See ProducerOptions::instance_id. Role hosts set e.g. "cons:UID-...:rx".
    std::string instance_id{};
};

} // namespace pylabhub::hub
