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
    std::string role_tag;    ///< "producer" or "processor" (HEP-0024 role tag).
    bool        has_shm{false};

    /// True when the producer publishes via ZMQ (HEP-CORE-0021).  When
    /// set, `zmq_node_endpoint` must also be populated.
    bool        is_zmq_transport{false};
    std::string zmq_node_endpoint;
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

    /// Consumer's CURVE pubkey (Z85, 40 chars).  Sent on the wire as
    /// `zmq_pubkey` per broker_proto 5→6 (HEP-CORE-0036 §6.5 amended
    /// 2026-06-04) so the broker can add it to the channel's
    /// authorized-consumer allowlist via
    /// `HubState::_on_consumer_authorized`.  Empty is accepted (allowlist
    /// stays empty for that consumer — legal deny-all per HEP-CORE-0035
    /// §4.8.4); callers in the role-host should pass the live
    /// `BrokerRequestComm::Config::client_pubkey` value.
    std::string zmq_pubkey;
};

/// Build the producer-side REG_REQ payload (channel/identity/transport
/// fields, no schema).  Equivalent to the manual JSON construction
/// duplicated in producer_role_host.cpp / processor_role_host.cpp
/// pre-Phase-5b.
inline nlohmann::json build_producer_reg_payload(const ProducerRegInputs &in)
{
    nlohmann::json reg;
    reg["channel_name"]      = in.channel;
    reg["pattern"]           = "PubSub";
    reg["has_shared_memory"] = in.has_shm;
    reg["producer_pid"]      = pylabhub::platform::get_pid();
    reg["shm_name"]          = in.channel;
    reg["role_uid"]          = in.role_uid;
    reg["role_name"]         = in.role_name;
    reg["role_type"]         = in.role_tag;
    // zmq_ctrl_endpoint / zmq_data_endpoint placeholder fields retired
    // in Wave M2.5 step 2c (2026-05-10).  zmq_pubkey kept on the wire
    // (broker still parses it into ChannelEntry.zmq_pubkey →
    // RoleEntry.pubkey_z85); producer-side wire layer sends empty
    // string today because the role's CURVE keypair is not yet
    // surfaced here.  See REVIEW_WaveM2.5_2026-05-10.md F7 + F16.
    reg["zmq_pubkey"]        = "";

    if (in.is_zmq_transport)
    {
        reg["data_transport"]    = "zmq";
        reg["zmq_node_endpoint"] = in.zmq_node_endpoint;
    }
    return reg;
}

/// Build the consumer-side CONSUMER_REG_REQ payload (channel/identity,
/// no schema).
inline nlohmann::json build_consumer_reg_payload(const ConsumerRegInputs &in)
{
    // broker_proto 4→5 (audit R3.5b, 2026-05-19): wire keys unified —
    // CONSUMER_REG_REQ now sends `role_uid`/`role_name` instead of
    // `consumer_uid`/`consumer_name`.  Role tag is embedded in the
    // uid value (HEP-CORE-0033 §G2.2.0b).
    //
    // broker_proto 5→6 (HEP-CORE-0036 §6.5 amended 2026-06-04 +
    // PeerAdmission D3): wires `zmq_pubkey` — consumer's CURVE pubkey
    // for the channel-scope allowlist.  Mirrors the producer-side
    // `zmq_pubkey` field already on REG_REQ.
    nlohmann::json reg;
    reg["channel_name"] = in.channel;
    reg["role_uid"]     = in.role_uid;
    reg["role_name"]    = in.role_name;
    reg["consumer_pid"] = pylabhub::platform::get_pid();
    reg["zmq_pubkey"]   = in.zmq_pubkey;
    return reg;
}

} // namespace pylabhub::hub
