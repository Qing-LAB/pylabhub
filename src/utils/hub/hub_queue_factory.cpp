/**
 * @file hub_queue_factory.cpp
 * @brief Two-level dispatch for `hub::Queue::create_reader/writer`.
 *
 * Per HEP-CORE-0017 §3.3.0 (2026-07-08 topology migration):
 *
 * ```
 * hub::Queue::create_reader(topology, transport, opts)
 *     │
 *     ├─ Gate 1: fan-in + SHM ⇒ nullptr (SHM host-local, single-producer)
 *     ├─ Gate 4: endpoint_hint validity per side (BINDING may set;
 *     │         DIALING must leave empty; enforced by callers wiring
 *     │         opts.endpoint_hint from role config for the correct side).
 *     ├─ Gate 5: enum bounds (compile-time, from ChannelTopology enum
 *     │         + Transport enum being closed sets).
 *     │
 *     └─ Dispatch:
 *         Transport::Zmq  ⇒ translate opts → ZmqQueue::RxCreateOptions,
 *                            call ZmqQueue::create_reader(topology, opts)
 *         Transport::Shm  ⇒ translate opts → ShmQueue::RxCreateOptions,
 *                            call ShmQueue::create_reader(topology, opts)
 * ```
 *
 * Symmetric for `create_writer`.  Gates 2/3 (fan-out/one-to-one are
 * permissive on both transports) are trivially satisfied by the enum
 * value being FanOut / OneToOne — no branch needed.
 *
 * Belt-and-braces: the gate here is the LAST line of defense.  The
 * broker's admission path already refuses `(fan-in, shm)` at REG_REQ
 * via `topology::transport_compatible` (`hub_state.hpp:167`); a config
 * that reaches this factory with `(FanIn, Shm)` has bypassed that
 * check, so we log and return nullptr rather than throwing.
 */
#include "utils/hub_queue_factory.hpp"

#include "utils/logger.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/hub_shm_queue.hpp"

namespace pylabhub::hub
{

std::optional<Transport> transport_from_string(std::string_view s) noexcept
{
    if (s == "zmq") return Transport::Zmq;
    if (s == "shm") return Transport::Shm;
    return std::nullopt;
}

// ─── Reader ────────────────────────────────────────────────────────

std::unique_ptr<QueueReader>
Queue::create_reader(ChannelTopology topology,
                     Transport       transport,
                     RxOptions       opts)
{
    // Gate 1: fan-in requires ZMQ (SHM is host-local single-producer).
    // §3.3.0 decision matrix: SHM never binds a fan-in socket.
    if (topology == ChannelTopology::FanIn && transport == Transport::Shm)
    {
        LOGGER_ERROR(
            "hub::Queue::create_reader — fan-in topology is not compatible "
            "with SHM transport (HEP-CORE-0017 §3.3.0 gate 1).  SHM is "
            "physically single-producer; fan-in requires N producers to "
            "the same consumer, which only ZMQ PULL-bind supports.");
        return nullptr;
    }

    switch (transport)
    {
    case Transport::Zmq:
    {
        ZmqQueue::RxCreateOptions zmq_opts;
        zmq_opts.endpoint          = std::move(opts.endpoint_hint);
        zmq_opts.server_pubkey     = std::move(opts.server_pubkey);
        zmq_opts.schema            = std::move(opts.slot_schema);
        zmq_opts.packing           = std::move(opts.slot_packing);
        zmq_opts.identity_key_name = opts.identity_key_name;
        zmq_opts.max_buffer_depth  = opts.max_buffer_depth;
        zmq_opts.schema_tag        = opts.schema_tag;
        zmq_opts.instance_id       = std::move(opts.instance_id);
        return ZmqQueue::create_reader(topology, std::move(zmq_opts));
    }
    case Transport::Shm:
    {
        ShmQueue::RxCreateOptions shm_opts;
        shm_opts.channel_name   = std::move(opts.channel_name);
        shm_opts.slot_schema    = std::move(opts.slot_schema);
        shm_opts.slot_packing   = std::move(opts.slot_packing);
        shm_opts.consumer_uid   = std::move(opts.consumer_uid);
        shm_opts.consumer_name  = std::move(opts.consumer_name);
        shm_opts.verify_slot    = opts.verify_slot;
        shm_opts.verify_fz      = opts.verify_fz;
        return ShmQueue::create_reader(topology, std::move(shm_opts));
    }
    }
    LOGGER_ERROR("hub::Queue::create_reader — unknown Transport enum value.");
    return nullptr;
}

// ─── Writer ────────────────────────────────────────────────────────

std::unique_ptr<QueueWriter>
Queue::create_writer(ChannelTopology topology,
                     Transport       transport,
                     TxOptions       opts)
{
    // Gate 1: same as reader — fan-in producer via SHM is disallowed.
    if (topology == ChannelTopology::FanIn && transport == Transport::Shm)
    {
        LOGGER_ERROR(
            "hub::Queue::create_writer — fan-in topology is not compatible "
            "with SHM transport (HEP-CORE-0017 §3.3.0 gate 1).");
        return nullptr;
    }

    switch (transport)
    {
    case Transport::Zmq:
    {
        ZmqQueue::TxCreateOptions zmq_opts;
        zmq_opts.endpoint              = std::move(opts.endpoint_hint);
        zmq_opts.server_pubkey         = std::move(opts.server_pubkey);
        zmq_opts.schema                = std::move(opts.slot_schema);
        zmq_opts.packing               = std::move(opts.slot_packing);
        zmq_opts.identity_key_name     = opts.identity_key_name;
        zmq_opts.zap_domain            = std::move(opts.zap_domain);
        zmq_opts.schema_tag            = opts.schema_tag;
        zmq_opts.sndhwm                = opts.sndhwm;
        zmq_opts.send_buffer_depth     = opts.send_buffer_depth;
        zmq_opts.overflow_policy       = opts.overflow_policy;
        zmq_opts.send_retry_interval_ms = opts.send_retry_interval_ms;
        zmq_opts.instance_id           = std::move(opts.instance_id);
        return ZmqQueue::create_writer(topology, std::move(zmq_opts));
    }
    case Transport::Shm:
    {
        ShmQueue::TxCreateOptions shm_opts;
        shm_opts.channel_name          = std::move(opts.channel_name);
        shm_opts.slot_schema           = std::move(opts.slot_schema);
        shm_opts.slot_packing          = std::move(opts.slot_packing);
        shm_opts.fz_schema             = std::move(opts.fz_schema);
        shm_opts.fz_packing            = std::move(opts.fz_packing);
        shm_opts.ring_buffer_capacity  = opts.ring_buffer_capacity;
        shm_opts.page_size             = opts.page_size;
        shm_opts.policy                = opts.policy;
        shm_opts.sync_policy           = opts.sync_policy;
        shm_opts.checksum_policy       = opts.checksum_policy;
        shm_opts.checksum_slot         = opts.checksum_slot;
        shm_opts.checksum_fz           = opts.checksum_fz;
        shm_opts.always_clear_slot     = opts.always_clear_slot;
        shm_opts.hub_uid               = std::move(opts.hub_uid);
        shm_opts.hub_name              = std::move(opts.hub_name);
        shm_opts.slot_schema_info      = opts.slot_schema_info;
        shm_opts.fz_schema_info        = opts.fz_schema_info;
        shm_opts.producer_uid          = std::move(opts.producer_uid);
        shm_opts.producer_name         = std::move(opts.producer_name);
        return ShmQueue::create_writer(topology, std::move(shm_opts));
    }
    }
    LOGGER_ERROR("hub::Queue::create_writer — unknown Transport enum value.");
    return nullptr;
}

} // namespace pylabhub::hub
