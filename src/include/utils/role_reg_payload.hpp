#pragma once
/**
 * @file role_reg_payload.hpp
 * @brief Shared REG_REQ / CONSUMER_REG_REQ payload builders for role hosts.
 *
 * The three role hosts (producer, consumer, processor) duplicate the
 * non-schema portion of the broker registration payload — channel name,
 * role identity, transport endpoints.  These helpers consolidate that
 * boilerplate so a single edit point governs the wire shape.
 *
 * Schema fields (`schema_id`, `schema_blds`, `expected_*`, etc.) are
 * NOT covered here; those use the helpers in `schema_utils.hpp`
 * (`apply_producer_schema_fields` / `apply_consumer_schema_fields`).
 * Callers typically invoke this helper first and then layer the schema
 * fields on top.
 *
 * @see HEP-CORE-0034 §10 (wire protocol)
 * @see HEP-CORE-0021 (ZMQ endpoint registry; data_transport / zmq_node_endpoint)
 * @see HEP-CORE-0023 §2 (role state machine; uid is the key)
 */

#include "utils/json_fwd.hpp"
#include "plh_platform.hpp"  // pylabhub::platform::get_pid()
#include "plh_version_registry.hpp"  // ABI fingerprint (HEP-CORE-0032 §8)

#include <stdexcept>
#include <string>

namespace pylabhub::hub
{

/// Inputs to `build_producer_reg_payload`.  Mirrors the producer-side
/// REG_REQ wire shape (HEP-CORE-0007 §12.3 + HEP-CORE-0021 endpoint
/// registry).  All fields are string-keyed in the resulting JSON.
struct ProducerRegInputs
{
    std::string channel;     ///< Channel name (= shm_name in current shape).
    std::string role_uid;    ///< HEP-CORE-0023 §2 role uid.
    std::string role_name;   ///< Human-readable role name.
    std::string role_type;    ///< "producer" or "processor" (HEP-0024 role tag).
    bool        has_shm{false};

    /// True when the producer publishes via ZMQ (HEP-CORE-0021).  When
    /// set, `zmq_node_endpoint` must also be populated.
    bool        is_zmq_transport{false};
    std::string zmq_node_endpoint;

    /// Producer's CURVE identity pubkey (Z85, 40 chars).  REQUIRED per
    /// HEP-CORE-0036 §4.1 + §5.1 + §6.4 (broker stores it on
    /// `ChannelEntry::producers[i].zmq_pubkey`; consumers receive it
    /// via `CONSUMER_REG_ACK.producers[]` and use it as the data-plane
    /// `curve_serverkey`).  HEP-CORE-0040 §172 — callers in the
    /// role-host pass `key_store().pubkey("role_identity")`; broker
    /// rejects REG_REQ with empty or wrong-length `zmq_pubkey`.
    ///
    /// HEP-CORE-0041 §5.3 — this same pubkey is reused for the
    /// SHM-capability `crypto_box` challenge-response; broker echoes
    /// it back as `producer_pubkey_z85` in CONSUMER_REG_ACK for SHM
    /// channels.
    std::string zmq_pubkey;

    /// HEP-CORE-0041 §5.1 — producer's bound SHM-capability transport
    /// endpoint (Unix socket on POSIX, named pipe on Windows).
    /// REQUIRED for SHM channels (i.e. when `has_shm && !is_zmq_transport`);
    /// empty otherwise.  Producer-side caller computes the canonical
    /// path via `pylabhub::utils::security::default_shm_capability_endpoint`.
    /// Broker stores it on `ProducerEntry::shm_capability_endpoint` and
    /// echoes it back via `CONSUMER_REG_ACK.shm_capability_endpoint`
    /// for the consumer to dial via `attach_shm_capability_consumer`.
    std::string shm_capability_endpoint;
};

/// Inputs to `build_consumer_reg_payload`.  broker_proto 4→5 (audit
/// R3.5b, 2026-05-19) — these were `consumer_uid` / `consumer_name`
/// pre-rename; unified onto `role_uid` / `role_name` for consistency
/// with `ProducerRegInputs`.  The role tag (`cons.` for pure
/// consumers, `proc.` for processors) lives inside the uid value.
struct ConsumerRegInputs
{
    std::string channel;
    std::string role_uid;
    std::string role_name;

    /// Consumer's CURVE pubkey (Z85, exactly 40 chars).  REQUIRED per
    /// HEP-CORE-0036 §6.5: the broker uses it to populate the channel's
    /// authorized-consumer allowlist via
    /// `HubState::_on_consumer_authorized`.  HEP-CORE-0040 §172 —
    /// callers in the role-host pass `key_store().pubkey("role_identity")`;
    /// broker rejects CONSUMER_REG_REQ with empty or wrong-length
    /// `zmq_pubkey`.
    std::string zmq_pubkey;
};

/// Build the producer-side REG_REQ payload (channel/identity/transport
/// fields, no schema).  Equivalent to the manual JSON construction
/// duplicated in producer_role_host.cpp / processor_role_host.cpp
/// pre-Phase-5b.
///
/// **Contract (HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1):** the produced
/// payload ALWAYS carries an explicit `data_transport` field (string,
/// one of `"shm"` or `"zmq"`); the broker rejects REG_REQ with missing
/// or empty `data_transport` as `INVALID_REQUEST` (#281, 2026-06-23).
/// Callers MUST set exactly one of `in.is_zmq_transport` /
/// `in.has_shm` to `true`; the other paired field
/// (`zmq_node_endpoint` for ZMQ, `shm_capability_endpoint` for SHM)
/// MUST be non-empty.  A "no data plane wired" registration shape no
/// longer exists — the wire requires an explicit transport spec.
inline nlohmann::json build_producer_reg_payload(const ProducerRegInputs &in)
{
    // HEP-CORE-0036 §5b.4 canonical REG_REQ shape.  Pre-§5b duplicates
    // `pattern` (constant "PubSub"), `has_shared_memory` (subsumed by
    // `data_transport`), and `shm_name` (duplicate of `channel_name`)
    // have been retired.
    nlohmann::json reg;
    reg["channel_name"]      = in.channel;
    reg["producer_pid"]      = pylabhub::platform::get_pid();
    reg["role_uid"]          = in.role_uid;
    reg["role_name"]         = in.role_name;
    reg["role_type"]         = in.role_type;
    // zmq_pubkey: REQUIRED per HEP-CORE-0036 §4.1 + §5.1 + §6.4.
    // Producer's CURVE identity pubkey, Z85-encoded (40 chars).
    // Broker stores it on ChannelEntry::producers[i].zmq_pubkey, then
    // emits it back via CONSUMER_REG_ACK.producers[] so each consumer
    // knows the producer's curve_serverkey for the data-plane PULL
    // handshake.  Broker rejects empty / non-40-char with
    // INVALID_REQUEST.  HEP-CORE-0035 §2 makes CURVE unconditional —
    // there is no fallback.
    reg["zmq_pubkey"]        = in.zmq_pubkey;

    // HEP-CORE-0032 §8.2 — ABI fingerprint envelope.  The
    // 15-field ComponentVersions object.  Broker verifies via
    // `verify_peer_versions()` on ingest and logs per §8.6.  In slice C
    // (2026-07-03) this is emitted UNCONDITIONALLY; broker's ingest
    // treats absence as verdict='ABSENT' + accept during the roll-out
    // window (§8.5 default lenient policy).  A future MAJOR bump on
    // broker_proto promotes to REQUIRED.
    reg["abi_fingerprint"] = pylabhub::version::to_json_object(
        pylabhub::version::current());
    // Optional sibling: build_id is a per-build identifier, only
    // populated when compiled with build-id support
    // (PYLABHUB_HAVE_BUILD_ID).  Serialized as a bare string next to
    // abi_fingerprint per §8.2 last paragraph so it can be omitted
    // independently of the ComponentVersions block.
    if (const char *bid = pylabhub::version::build_id())
    {
        reg["build_id"] = bid;
    }

    if (in.is_zmq_transport)
    {
        reg["data_transport"]    = "zmq";
        reg["zmq_node_endpoint"] = in.zmq_node_endpoint;
    }
    else if (in.has_shm)
    {
        // HEP-CORE-0041 §5.1 — SHM channels publish their L2
        // capability-transport endpoint.  Substep 1g (#254): explicit
        // `data_transport="shm"` discriminates the branch on the
        // broker side (matches the existing `"zmq"` shape); the
        // endpoint is the URI the consumer dials via
        // `attach_shm_capability_consumer`.
        reg["data_transport"]          = "shm";
        reg["shm_capability_endpoint"] = in.shm_capability_endpoint;
    }
    else
    {
        // HEP-CORE-0036 §5b + HEP-CORE-0041 §5.1: exactly one transport
        // discriminator MUST be set.  Reaching here means the caller's
        // upstream config translation produced `{has_shm=false,
        // is_zmq_transport=false}` — typically `shm.enabled=false` with
        // `transport` defaulted to "shm" (transport_config.hpp:30).
        // Without this guard the payload omits `data_transport`
        // entirely; the broker's strict #281 check rejects with
        // INVALID_REQUEST and the role tears down with a misleading
        // wire-shape diagnostic that points away from the actual
        // config mismatch.  Fail HERE with a config-pointing message.
        throw std::logic_error(
            "build_producer_reg_payload: neither is_zmq_transport nor "
            "has_shm is set on ProducerRegInputs.  Caller config "
            "mismatch — likely shm.enabled=false with transport "
            "defaulted to \"shm\".  Verify transport==\"zmq\" OR "
            "shm.enabled==true in the role config.");
    }
    return reg;
}

/// Build the consumer-side CONSUMER_REG_REQ payload (channel/identity,
/// no schema).
inline nlohmann::json build_consumer_reg_payload(const ConsumerRegInputs &in)
{
    // Wire keys: `role_uid`/`role_name` (role tag embedded in the uid
    // value per HEP-CORE-0033 §G2.2.0b).  `zmq_pubkey` is the
    // consumer's CURVE pubkey for the channel-scope allowlist, mirroring
    // the producer-side `zmq_pubkey` on REG_REQ (HEP-CORE-0036 §6.5).
    nlohmann::json reg;
    reg["channel_name"] = in.channel;
    reg["role_uid"]     = in.role_uid;
    reg["role_name"]    = in.role_name;
    reg["consumer_pid"] = pylabhub::platform::get_pid();
    reg["zmq_pubkey"]   = in.zmq_pubkey;
    // HEP-CORE-0032 §8.2 — same ABI fingerprint envelope as
    // build_producer_reg_payload.  See that function's comment for
    // slice-C roll-out policy (verdict='ABSENT' + accept during
    // migration window).
    reg["abi_fingerprint"] = pylabhub::version::to_json_object(
        pylabhub::version::current());
    if (const char *bid = pylabhub::version::build_id())
    {
        reg["build_id"] = bid;
    }
    return reg;
}

} // namespace pylabhub::hub
