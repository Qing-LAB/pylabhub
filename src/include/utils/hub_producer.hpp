#pragma once
/**
 * @file hub_producer.hpp
 * @brief ProducerOptions — configuration for a role's Tx-side queue.
 *
 * Post-L3.γ A6.3: the hub::Producer class is GONE. This header now exposes
 * only the ProducerOptions data struct, which role hosts hand to
 * RoleAPIBase::build_tx_queue(). The factory logic lives inside
 * RoleAPIBase::Impl — ownership of the resulting ShmQueue/ZmqQueue is on
 * the RoleAPIBase itself.
 */

#include "pylabhub_utils_export.h"
#include "utils/channel_pattern.hpp"
#include "utils/data_block.hpp"
#include "utils/hub_zmq_queue.hpp"  // ZmqSchemaField, OverflowPolicy, kZmqDefaultBufferDepth
#include "utils/schema_library.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ProducerOptions
{
    std::string    channel_name;
    ChannelPattern pattern{ChannelPattern::PubSub};

    bool            has_shm{false};
    DataBlockConfig shm_config{};

    std::string schema_hash{};
    uint32_t    schema_version{0};

    int timeout_ms{5000};

    std::string role_name{};
    std::string role_uid{};

    std::string schema_id{};

    // HEP-CORE-0021: ZMQ Endpoint Registry
    std::string data_transport{"shm"};
    std::string zmq_node_endpoint{};
    bool zmq_bind{true};
    std::vector<ZmqSchemaField> zmq_schema{};
    std::string zmq_packing{"aligned"};
    std::vector<ZmqSchemaField> fz_schema{};
    std::string fz_packing{"aligned"};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};
    OverflowPolicy zmq_overflow_policy{OverflowPolicy::Drop};

    // Queue abstraction
    ChecksumPolicy checksum_policy{ChecksumPolicy::Enforced};
    bool flexzone_checksum{true};
    bool always_clear_slot{true};

    /// Stable identifier for the queue's internal threads' lifecycle module.
    /// Role hosts populate this (e.g. "prod:UID-...:tx") before calling
    /// RoleAPIBase::build_tx_queue; empty means the queue will auto-generate
    /// a pointer-address fallback (fine for one-off direct factory use).
    std::string instance_id{};
};

} // namespace pylabhub::hub
