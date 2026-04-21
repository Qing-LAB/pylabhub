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
#include "utils/schema_types.hpp"   // SchemaSpec (slot_spec / fz_spec in ProducerOptions)

#include <cstddef>
#include <string>
#include <vector>

namespace pylabhub::hub
{

struct ProducerOptions
{
    std::string channel_name;

    bool            has_shm{false};
    DataBlockConfig shm_config{};

    // Schema (single source of truth for fields + packing; hash is
    // auto-computed from these at build_tx_queue time).
    SchemaSpec slot_spec{};
    SchemaSpec fz_spec{};

    // Transport (HEP-CORE-0021)
    std::string data_transport{"shm"};
    std::string zmq_node_endpoint{};
    bool zmq_bind{true};
    size_t zmq_buffer_depth{kZmqDefaultBufferDepth};
    OverflowPolicy zmq_overflow_policy{OverflowPolicy::Drop};

    // Queue policy
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
