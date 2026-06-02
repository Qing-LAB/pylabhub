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
    static void set_role_disconnected(HubState &s, const std::string &uid)
    {
        s._set_role_disconnected(uid);
    }
    static void set_band_joined(HubState &s, const std::string &b, BandMember m)
    {
        s._set_band_joined(b, std::move(m));
    }
    static void set_band_left(HubState &s, const std::string &b,
                              const std::string &uid,
                              const std::string &reason = "test")
    {
        s._set_band_left(b, uid, reason);
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
    // set_channel_zmq_node_endpoint retired Wave M2.5 step 6.5 — use
    // `set_producer_zmq_node_endpoint(uid, ...)` instead.
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
    /// Wave M2.5 step 3 — additive REG_REQ admission entry point.
    /// See `docs/tech_draft/controlled_access_api_design.md` §7.5.
    /// Tests drive multi-producer admission scenarios through this
    /// forwarder; the production REG_REQ handler routes the same
    /// way.
    static ProducerAdmissionResult
    on_producer_added(HubState&                  s,
                       const std::string&         channel_name,
                       ChannelSchemaInvariants    schema,
                       ChannelTransportInvariants transport,
                       ProducerEntry              producer)
    {
        return s._on_producer_added(channel_name,
                                     std::move(schema),
                                     std::move(transport),
                                     std::move(producer));
    }
    /// Wave M2.5 step 4 — additive DEREG_REQ / producer-drop entry
    /// point.  Tests drive multi-producer drop scenarios through this
    /// forwarder; the production DEREG_REQ handler routes the same way.
    static RemoveProducerResult
    on_producer_dropped(HubState&             s,
                         const std::string&    channel_name,
                         const std::string&    role_uid,
                         ChannelCloseReason    reason)
    {
        return s._on_producer_dropped(channel_name, role_uid, reason);
    }
    /// Wave M2.5 step 5 — per-producer ENDPOINT_UPDATE_REQ entry point.
    /// Returns true iff the channel exists AND the producer is admitted.
    static bool
    set_producer_zmq_node_endpoint(HubState&          s,
                                    const std::string& channel_name,
                                    const std::string& role_uid,
                                    std::string        endpoint)
    {
        return s._set_producer_zmq_node_endpoint(channel_name, role_uid,
                                                  std::move(endpoint));
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
    static void on_heartbeat(HubState                             &s,
                             const std::string                    &ch,
                             const std::string                    &uid,
                             const std::string                    &role_type,
                             std::chrono::steady_clock::time_point when,
                             const std::optional<nlohmann::json>  &m)
    {
        s._on_heartbeat(ch, uid, role_type, when, m);
    }
    /// Wave-B M2 (2/3) per-presence Connected → Pending forwarder.
    /// 2-arg overload defaults to producer for backward-compat with
    /// pre-2/3 tests; consumer tests use the 3-arg form.
    static void on_heartbeat_timeout(HubState          &s,
                                     const std::string &ch,
                                     const std::string &uid)
    {
        s._on_heartbeat_timeout(ch, uid, "producer");
    }
    static void on_heartbeat_timeout(HubState          &s,
                                     const std::string &ch,
                                     const std::string &uid,
                                     const std::string &role_type)
    {
        s._on_heartbeat_timeout(ch, uid, role_type);
    }
    /// Wave M2.5 step 6 + Wave-B M2 (2/3): per-presence Pending →
    /// Disconnected forwarder.  2-arg overload defaults to producer for
    /// backward-compat; consumer tests use the 3-arg form.  Returns
    /// the typed RemoveProducerResult — for the consumer path,
    /// `removed` reflects ChannelEntry.consumers[] erase and
    /// `channel_now_empty` is always false (consumer never tears down).
    static RemoveProducerResult
    on_pending_timeout(HubState&         s,
                        const std::string &ch,
                        const std::string &uid)
    {
        return s._on_pending_timeout(ch, uid, "producer");
    }
    static RemoveProducerResult
    on_pending_timeout(HubState&         s,
                        const std::string &ch,
                        const std::string &uid,
                        const std::string &role_type)
    {
        return s._on_pending_timeout(ch, uid, role_type);
    }
    // M1.4 (2026-05-11): `on_metrics_reported` test forwarder deleted
    // alongside `_on_metrics_reported`.  Use `on_heartbeat(s, ch, uid,
    // role_type, when, metrics)` to test the metrics-via-heartbeat
    // path (HEP-CORE-0019 §2.3 Phase 6).
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

    // ── Channel-access forwarders (HEP-CORE-0036 §4.1) ────────────────
    static void on_channel_access_opened(HubState          &s,
                                          const std::string &channel_name,
                                          std::uint64_t      shm_secret)
    {
        s._on_channel_access_opened(channel_name, shm_secret);
    }
    static void on_channel_access_closed(HubState          &s,
                                          const std::string &channel_name)
    {
        s._on_channel_access_closed(channel_name);
    }
    static void on_consumer_authorized(HubState          &s,
                                        const std::string &channel_name,
                                        const std::string &pubkey_z85)
    {
        s._on_consumer_authorized(channel_name, pubkey_z85);
    }
    static void on_consumer_revoked(HubState          &s,
                                     const std::string &channel_name,
                                     const std::string &pubkey_z85)
    {
        s._on_consumer_revoked(channel_name, pubkey_z85);
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
