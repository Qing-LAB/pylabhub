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

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ConsumerOptions
{
    std::string channel_name;

    std::string expected_schema_hash{};

    uint64_t shm_shared_secret{0};
    std::optional<DataBlockConfig> expected_shm_config{};

    std::string consumer_uid{};
    std::string consumer_name{};

    int timeout_ms{5000};

    std::string expected_schema_id{};

    // HEP-CORE-0021: ZMQ Endpoint Registry
    std::vector<ZmqSchemaField> zmq_schema{};
    std::string zmq_packing{"aligned"};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};

    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};

    std::string queue_type{};

    std::string shm_name{};
    std::string data_transport{"shm"};
    std::string zmq_node_endpoint{};

    /// See ProducerOptions::instance_id. Role hosts set e.g. "cons:UID-...:rx".
    std::string instance_id{};
};

} // namespace pylabhub::hub
