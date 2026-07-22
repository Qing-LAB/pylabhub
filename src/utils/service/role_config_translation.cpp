/**
 * @file role_config_translation.cpp
 * @brief Implementation of shared config→opts translators.
 *
 * See role_config_translation.hpp for full context and audit trail.
 */

#include "utils/role_config_translation.hpp"
#include "utils/hub_state.hpp" // hub::topology::parse

namespace pylabhub::scripting
{

hub::TxQueueOptions make_tx_opts(const config::RoleConfig &config,
                                 const hub::SchemaSpec &out_slot_spec,
                                 const hub::SchemaSpec &out_fz_spec, bool has_tx_fz)
{
    const auto &tr = config.out_transport();
    const auto &shm = config.out_shm();

    hub::TxQueueOptions opts;
    opts.has_shm = shm.enabled;
    opts.slot_spec = out_slot_spec;
    opts.fz_spec = out_fz_spec;

    // Topology from RoleConfig.out_channel_topology (per HEP-CORE-0017
    // §3.3.0).  Empty string → default OneToOne.  Invalid strings are
    // already rejected at config load by role_config's allowlist gate,
    // so a parse failure here is a defense-in-depth path.
    if (const auto &t = config.out_channel_topology(); !t.empty())
    {
        if (auto parsed = hub::topology::parse(t))
            opts.topology = *parsed;
    }

    opts.checksum_policy = config.checksum().policy;
    opts.flexzone_checksum = config.checksum().flexzone && has_tx_fz;

    // SHM config block — only meaningful when shm.enabled.  Note
    // (#275-S3): `opts.shm_config.shared_secret` is no longer populated —
    // the role-side queue builder retired the legacy secret-based ShmQueue
    // path; production binds via the capability fd
    // (`TxQueueOptions::shm_capability_fd`).  The field on
    // `DataBlockConfig` itself retires in #275-S5.
    if (shm.enabled)
    {
        opts.shm_config.ring_buffer_capacity = shm.slot_count;
        opts.shm_config.policy = hub::DataBlockPolicy::RingBuffer;
        opts.shm_config.consumer_sync_policy = shm.sync_policy;
        opts.shm_config.checksum_policy = config.checksum().policy;
        opts.shm_config.physical_page_size = hub::system_page_size();
    }

    // ZMQ transport (HEP-CORE-0021).  Overrides has_shm to false.
    if (tr.transport == config::Transport::Zmq)
    {
        opts.has_shm = false;
        opts.data_transport = "zmq";
        opts.zmq_node_endpoint = tr.zmq_endpoint;
        opts.zmq_bind = tr.zmq_bind;
        opts.zmq_buffer_depth = tr.zmq_buffer_depth;
        opts.zmq_overflow_policy = (tr.zmq_overflow_policy == "block") ? hub::OverflowPolicy::Block
                                                                       : hub::OverflowPolicy::Drop;
    }

    return opts;
}

hub::RxQueueOptions make_rx_opts(const config::RoleConfig &config,
                                 const hub::SchemaSpec &in_slot_spec,
                                 const hub::SchemaSpec &in_fz_spec, bool has_rx_fz)
{
    const auto &tr = config.in_transport();
    const auto &shm = config.in_shm();
    const auto &ch = config.in_channel();

    hub::RxQueueOptions opts;
    // Audit B5/G21: shm_name = in_channel for the SHM path; cleared
    // below if transport is ZMQ (Audit B11).
    // #275-S3: `opts.shm_shared_secret` retired with the legacy
    // secret-based ShmQueue path in role_api_base.cpp.
    opts.shm_name = ch;
    opts.slot_spec = in_slot_spec;
    opts.fz_spec = in_fz_spec;

    // Topology from RoleConfig.in_channel_topology per HEP-CORE-0017
    // §3.3.0.  Empty string → default OneToOne.
    if (const auto &t = config.in_channel_topology(); !t.empty())
    {
        if (auto parsed = hub::topology::parse(t))
            opts.topology = *parsed;
    }

    opts.checksum_policy = config.checksum().policy;
    opts.flexzone_checksum = config.checksum().flexzone && has_rx_fz;

    // Q1 resolution (2026-05-22): unlike the previous per-role static
    // method on ProcessorRoleHost (which set zmq_buffer_depth
    // unconditionally), this unified translator sets the field ONLY
    // inside the if-zmq branch.  Adopts Consumer's pre-existing
    // convention; functionally equivalent today (defaults converge)
    // but eliminates the latent divergence foot-gun.
    if (tr.transport == config::Transport::Zmq)
    {
        opts.data_transport = "zmq";
        opts.zmq_buffer_depth = tr.zmq_buffer_depth;
        // Under fan-in the consumer is the binding side and needs its
        // bind endpoint from config.  Under fan-out / one-to-one the
        // consumer is dialing and this field is unused (peer endpoint
        // arrives on CONSUMER_REG_ACK).  Passed through unconditionally
        // and read only on the binding side in build_rx_queue.
        opts.zmq_node_endpoint = tr.zmq_endpoint;
        opts.shm_name.clear();
    }

    return opts;
}

} // namespace pylabhub::scripting
