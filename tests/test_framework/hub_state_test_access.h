#pragma once
/**
 * @file hub_state_test_access.h
 * @brief Shared `HubStateTestAccess` shim for L2/L3 hub tests.
 *
 * `HubState`'s capability-op methods (`_on_channel_registered`,
 * `_on_role_registered`, `_on_band_joined`, …) are private — only the
 * broker is supposed to call them in production.  Tests need direct
 * access so they can fire events without spinning up a real broker
 * thread; HubState has a `friend struct ::pylabhub::hub::test::HubStateTestAccess`
 * declaration for exactly this purpose (see hub_state.hpp:389).
 *
 * This header is the canonical definition of that access struct.  Both
 * `test_layer2_service/test_hub_state.cpp` (full coverage) and any
 * L3 test that needs to inject HubState mutations include it; each
 * static method is inline so multiple TUs with the same definition
 * link cleanly.
 *
 * If you need an op not yet here, add a forwarder; do NOT duplicate
 * the access struct in another file.
 */

#include "utils/hub_state.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace pylabhub::hub::test
{

/// Friend shim for `HubState`.  Forwards to the private `_set_*` /
/// `_on_*` mutators so tests can mutate state without going through
/// the broker.
struct HubStateTestAccess
{
    // ── Direct setters (state primitives) ─────────────────────────────
    static void set_channel_opened(HubState &s, ChannelEntry e)
    {
        s._set_channel_opened(std::move(e));
    }
    static void set_channel_status(HubState &s, const std::string &name,
                                    ChannelStatus st)
    {
        s._set_channel_status(name, st);
    }
    static void set_channel_closed(HubState &s, const std::string &name)
    {
        s._set_channel_closed(name);
    }
    static void add_consumer(HubState &s, const std::string &ch, ConsumerEntry c)
    {
        s._add_consumer(ch, std::move(c));
    }
    static void remove_consumer(HubState &s, const std::string &ch,
                                const std::string &uid)
    {
        s._remove_consumer(ch, uid);
    }
    static void set_role_registered(HubState &s, RoleEntry e)
    {
        s._set_role_registered(std::move(e));
    }
    static void update_role_heartbeat(HubState                             &s,
                                      const std::string                    &uid,
                                      std::chrono::steady_clock::time_point when)
    {
        s._update_role_heartbeat(uid, when);
    }
    static void set_role_disconnected(HubState &s, const std::string &uid)
    {
        s._set_role_disconnected(uid);
    }
    static void set_band_joined(HubState &s, const std::string &b, BandMember m)
    {
        s._set_band_joined(b, std::move(m));
    }
    static void set_band_left(HubState &s, const std::string &b,
                              const std::string &uid)
    {
        s._set_band_left(b, uid);
    }
    static void set_peer_connected(HubState &s, PeerEntry e)
    {
        s._set_peer_connected(std::move(e));
    }
    static void set_peer_disconnected(HubState &s, const std::string &uid)
    {
        s._set_peer_disconnected(uid);
    }
    static void set_shm_block(HubState &s, ShmBlockRef r)
    {
        s._set_shm_block(std::move(r));
    }
    static void set_channel_closing_deadline(
        HubState &s, const std::string &name,
        std::chrono::steady_clock::time_point deadline)
    {
        s._set_channel_closing_deadline(name, deadline);
    }
    static void set_channel_zmq_node_endpoint(
        HubState &s, const std::string &name, std::string endpoint)
    {
        s._set_channel_zmq_node_endpoint(name, std::move(endpoint));
    }
    static void bump_counter(HubState &s, const std::string &k, uint64_t n = 1)
    {
        s._bump_counter(k, n);
    }

    // ── Capability-operation forwarders (HEP-0033 §G2) ────────────────
    //
    // These match the broker's call sites; call them to fire the
    // subscriber dispatch chain (channel/role/band/peer subscribers
    // → HubScriptRunner enqueue lambdas → IncomingMessage → script
    // callbacks).
    static void on_channel_registered(HubState &s, ChannelEntry e)
    {
        s._on_channel_registered(std::move(e));
    }
    static void on_channel_closed(HubState &s, const std::string &n,
                                  ChannelCloseReason why)
    {
        s._on_channel_closed(n, why);
    }
    static void on_consumer_joined(HubState &s, const std::string &ch,
                                   ConsumerEntry c)
    {
        s._on_consumer_joined(ch, std::move(c));
    }
    static void on_consumer_left(HubState &s, const std::string &ch,
                                 const std::string &uid)
    {
        s._on_consumer_left(ch, uid);
    }
    static void on_heartbeat(HubState                                 &s,
                             const std::string                        &ch,
                             const std::string                        &uid,
                             std::chrono::steady_clock::time_point     when,
                             const std::optional<nlohmann::json>      &m)
    {
        s._on_heartbeat(ch, uid, when, m);
    }
    static void on_heartbeat_timeout(HubState          &s,
                                     const std::string &ch,
                                     const std::string &uid)
    {
        s._on_heartbeat_timeout(ch, uid);
    }
    static void on_pending_timeout(HubState &s, const std::string &ch)
    {
        s._on_pending_timeout(ch);
    }
    static void on_metrics_reported(HubState                             &s,
                                    const std::string                    &ch,
                                    const std::string                    &uid,
                                    nlohmann::json                        m,
                                    std::chrono::system_clock::time_point when)
    {
        s._on_metrics_reported(ch, uid, std::move(m), when);
    }
    static void on_role_registered(HubState &s, RoleEntry e)
    {
        s._set_role_registered(std::move(e));
    }
    static void on_role_disconnected(HubState &s, const std::string &uid)
    {
        s._set_role_disconnected(uid);
    }
    static void on_band_joined(HubState &s, const std::string &band,
                               BandMember m)
    {
        s._on_band_joined(band, std::move(m));
    }
    static void on_band_left(HubState &s, const std::string &band,
                             const std::string &uid)
    {
        s._on_band_left(band, uid);
    }
    static void on_peer_connected(HubState &s, PeerEntry p)
    {
        s._on_peer_connected(std::move(p));
    }
    static void on_peer_disconnected(HubState &s, const std::string &uid)
    {
        s._on_peer_disconnected(uid);
    }
    static void on_message_processed(HubState          &s,
                                     const std::string &msg_type,
                                     std::size_t        in,
                                     std::size_t        out)
    {
        s._on_message_processed(msg_type, in, out);
    }

    // ── Schema-registry forwarders (HEP-CORE-0034 §11) ────────────────
    static ::pylabhub::schema::SchemaRegOutcome
    on_schema_registered(HubState &s, ::pylabhub::schema::SchemaRecord rec)
    {
        return s._on_schema_registered(std::move(rec));
    }
    static std::size_t on_schemas_evicted_for_owner(HubState          &s,
                                                    const std::string &owner_uid)
    {
        return s._on_schemas_evicted_for_owner(owner_uid);
    }
    static ::pylabhub::schema::CitationOutcome validate_schema_citation(
        HubState                       &s,
        const std::string              &citer_uid,
        const std::string              &channel_producer_uid,
        const std::string              &cited_owner,
        const std::string              &cited_id,
        const std::array<uint8_t, 32>  &expected_hash,
        const std::string              &expected_packing)
    {
        return s._validate_schema_citation(citer_uid, channel_producer_uid,
                                           cited_owner, cited_id,
                                           expected_hash, expected_packing);
    }
};

} // namespace pylabhub::hub::test
