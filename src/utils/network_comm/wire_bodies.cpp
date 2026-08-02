#include "utils/wire_bodies.hpp"

#include "utils/schema_utils.hpp" // hub::parse_schema_json — the ONE inbox-schema parser (HEP-0046 B.2)

#include <nlohmann/json.hpp>

#include <string>

namespace pylabhub::wire::detail
{

namespace
{
// Local helper — build the WireBodyError message with the offending field
// name inlined so the broker's rejection log identifies the exact issue.
std::string mkerr(const char *field, const char *want)
{
    std::string s = "wire body: field '";
    s += field;
    s += "' ";
    s += want;
    return s;
}
} // namespace

std::string read_string(const nlohmann::json &body, const char *field)
{
    auto it = body.find(field);
    if (it == body.end() || !it->is_string())
    {
        throw WireBodyError(mkerr(field, "missing or not string"));
    }
    return it->get<std::string>();
}

std::string read_string_or_empty(const nlohmann::json &body, const char *field)
{
    auto it = body.find(field);
    if (it == body.end())
        return {};
    if (!it->is_string())
    {
        throw WireBodyError(mkerr(field, "present but not string"));
    }
    return it->get<std::string>();
}

std::uint32_t read_u32(const nlohmann::json &body, const char *field)
{
    auto it = body.find(field);
    if (it == body.end() || !it->is_number_unsigned())
    {
        throw WireBodyError(mkerr(field, "missing or not unsigned number"));
    }
    return it->get<std::uint32_t>();
}

std::uint64_t read_u64(const nlohmann::json &body, const char *field)
{
    auto it = body.find(field);
    if (it == body.end() || !it->is_number_unsigned())
    {
        throw WireBodyError(mkerr(field, "missing or not unsigned number"));
    }
    return it->get<std::uint64_t>();
}

std::uint64_t read_u64_or_zero(const nlohmann::json &body, const char *field)
{
    auto it = body.find(field);
    if (it == body.end())
        return 0;
    if (!it->is_number_unsigned())
    {
        throw WireBodyError(mkerr(field, "present but not unsigned number"));
    }
    return it->get<std::uint64_t>();
}

const nlohmann::json &read_object(const nlohmann::json &body, const char *field)
{
    auto it = body.find(field);
    if (it == body.end() || !it->is_object())
    {
        throw WireBodyError(mkerr(field, "missing or not object"));
    }
    return *it;
}

void require(const nlohmann::json &body, const char *field, JsonKind kind)
{
    auto it = body.find(field);
    if (it == body.end())
    {
        throw WireBodyError(mkerr(field, "missing"));
    }
    bool ok = false;
    switch (kind)
    {
    case JsonKind::String:
        ok = it->is_string();
        break;
    case JsonKind::U32:
        ok = it->is_number_unsigned();
        break;
    case JsonKind::U64:
        ok = it->is_number_unsigned();
        break;
    case JsonKind::Object:
        ok = it->is_object();
        break;
    case JsonKind::Array:
        ok = it->is_array();
        break;
    }
    if (!ok)
    {
        throw WireBodyError(mkerr(field, "wrong JSON kind"));
    }
}

// Validate-if-present: the field is OPTIONAL, but when the caller
// includes it the type MUST match.  Missing is allowed; wrong-typed
// is rejected (HEP-0046 §14.3 pairing rule).
void validate_if_present(const nlohmann::json &body, const char *field, JsonKind kind)
{
    auto it = body.find(field);
    if (it == body.end())
        return; // absent → OK per contract
    bool ok = false;
    switch (kind)
    {
    case JsonKind::String:
        ok = it->is_string();
        break;
    case JsonKind::U32:
        ok = it->is_number_unsigned();
        break;
    case JsonKind::U64:
        ok = it->is_number_unsigned();
        break;
    case JsonKind::Object:
        ok = it->is_object();
        break;
    case JsonKind::Array:
        ok = it->is_array();
        break;
    }
    if (!ok)
    {
        throw WireBodyError(mkerr(field, "present but wrong JSON kind"));
    }
}

void require_envelope_hash(const nlohmann::json &body)
{
    // WireEnvelope::parse enforces this on inbound; body-class constructors
    // enforce again as a defense-in-depth so a body constructed outside the
    // envelope path (e.g., in a test) still fails loud on missing hash.
    require(body, "envelope_hash", JsonKind::String);
}

void refuse_field(const nlohmann::json &body, const char *field, const char *why)
{
    if (body.find(field) != body.end())
    {
        throw WireBodyError(mkerr(field, why));
    }
}

void require_security_triple(const nlohmann::json &body)
{
    require_envelope_hash(body);
    require(body, "client_nonce", JsonKind::String);
    require(body, "client_wall_ts", JsonKind::U64);
}

} // namespace pylabhub::wire::detail

namespace pylabhub::wire
{

// ── Constructor implementations ───────────────────────────────────────
//
// Each constructor validates the fields ITS msg_type carries.  REG-family
// bodies additionally require the security triple; every body requires
// envelope_hash.  Constructor stores the json by value (moved in), so
// subsequent accessor calls read from the cached body_ member.

namespace d = detail;

// ProducerRegReqBody — REG_REQ per HEP-CORE-0036 §5b.4 + HEP-CORE-0034
// §10.1.  Required: identity quintuple + transport + pubkey + abi
// fingerprint + security triple.  Schema fields are OPTIONAL at the
// wire-shape level (schema_id="" means anonymous per HEP-0034 §10.1);
// when schema_id is non-empty the broker's admission handler
// enforces the schema field trio (hash + blds + packing) per §10.1.
// No broker_proto scalar (C3: abi_fingerprint canonical per HEP-0032
// §8).  No schema_version wire field (C2: version rides inside
// schema_id `$name.v<N>` per HEP-CORE-0033 §G2.2.0b).
ProducerRegReqBody::ProducerRegReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    // Per HEP-CORE-0046 §7.1 required-field table: channel_name,
    // role_uid, role_type, zmq_pubkey, data_transport.  role_name is
    // OPTIONAL — role_uid already embeds a name component via
    // HEP-CORE-0033 §G2.2.0b grammar, so role_name is a redundant
    // display-only label (HEP-CORE-0023 §2.5.4).  Accessor returns
    // empty string when absent.
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require(body_, "role_type", d::JsonKind::String);
    d::require(body_, "data_transport", d::JsonKind::String);
    d::require(body_, "zmq_pubkey", d::JsonKind::String);
    d::require(body_, "abi_fingerprint", d::JsonKind::Object);
    // Validate-if-present (§14.3 pairing rule): every OPTIONAL field is
    // shape-checked at construction — absent is OK, present-but-wrong-
    // typed is BODY_SCHEMA_VIOLATION at parse, never a mid-handler
    // throw.  role_name gets string-type checking ONLY: it is an
    // untrusted display-only label with no grammar validation at any
    // layer — the validated name is the component inside role_uid
    // (HEP-CORE-0023 §2.5.4, ratified 2026-07-24).
    d::validate_if_present(body_, "role_name", d::JsonKind::String);
    d::validate_if_present(body_, "channel_topology", d::JsonKind::String);
    d::validate_if_present(body_, "schema_id", d::JsonKind::String);
    d::validate_if_present(body_, "schema_hash", d::JsonKind::String);
    d::validate_if_present(body_, "schema_blds", d::JsonKind::String);
    d::validate_if_present(body_, "schema_packing", d::JsonKind::String);
    d::validate_if_present(body_, "schema_owner", d::JsonKind::String);
    d::validate_if_present(body_, "flexzone_blds", d::JsonKind::String);
    d::validate_if_present(body_, "flexzone_packing", d::JsonKind::String);
    d::validate_if_present(body_, "zmq_node_endpoint", d::JsonKind::String);
    d::validate_if_present(body_, "shm_capability_endpoint", d::JsonKind::String);
    d::validate_if_present(body_, "producer_pid", d::JsonKind::U64);
    d::validate_if_present(body_, "producer_hostname", d::JsonKind::String);
    d::validate_if_present(body_, "metadata", d::JsonKind::Object);
    d::validate_if_present(body_, "build_id", d::JsonKind::String);
    d::validate_if_present(body_, "inbox_endpoint", d::JsonKind::String);
    d::validate_if_present(body_, "inbox_schema_json", d::JsonKind::String);
    d::validate_if_present(body_, "inbox_checksum", d::JsonKind::String);
    // HEP-0046 B.2 — the doubly-encoded inbox schema is parsed ONCE here
    // at the envelope boundary via the canonical hub::parse_schema_json
    // (HEP-0034 §6.2: canonical OBJECT form, in-object `packing`
    // REQUIRED).  Malformed content — non-JSON, bare array, missing /
    // invalid packing, bad field defs — is a BODY_SCHEMA_VIOLATION at
    // parse; no handler or discovery reader ever re-parses the string.
    if (const std::string inbox_s = d::read_string_or_empty(body_, "inbox_schema_json");
        !inbox_s.empty())
    {
        try
        {
            inbox_schema_ = ::pylabhub::hub::parse_schema_json(nlohmann::json::parse(inbox_s));
        }
        catch (const std::exception &e)
        {
            throw WireBodyError(std::string{"wire body: field 'inbox_schema_json' invalid: "} +
                                e.what());
        }
    }
    d::require_security_triple(body_);
}

// ConsumerRegReqBody — CONSUMER_REG_REQ per HEP-CORE-0036 §5b.6 +
// HEP-CORE-0034 §10.2.  Required: identity quintuple + transport +
// pubkey + abi fingerprint + security triple.  Schema-citation
// fields (`expected_schema_*` per §10.2 `expected_` prefix rule)
// are OPTIONAL at the wire-shape level; the citation mode
// (named / anonymous / empty) determines which of them are
// required, checked by the broker's admission handler.
// `data_transport` REQUIRED per C9 resolution.
ConsumerRegReqBody::ConsumerRegReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    // role_name OPTIONAL — see ProducerRegReqBody ctor for rationale
    // (HEP-CORE-0046 §7.1 + HEP-CORE-0023 §2.5.4).
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require(body_, "role_type", d::JsonKind::String);
    d::require(body_, "data_transport", d::JsonKind::String);
    d::require(body_, "zmq_pubkey", d::JsonKind::String);
    d::require(body_, "abi_fingerprint", d::JsonKind::Object);
    // Validate-if-present (§14.3 pairing rule) — parallel to
    // ProducerRegReqBody: every OPTIONAL field shape-checked at
    // construction.
    d::validate_if_present(body_, "role_name", d::JsonKind::String);
    d::validate_if_present(body_, "channel_topology", d::JsonKind::String);
    d::validate_if_present(body_, "expected_schema_id", d::JsonKind::String);
    d::validate_if_present(body_, "expected_schema_hash", d::JsonKind::String);
    d::validate_if_present(body_, "expected_schema_blds", d::JsonKind::String);
    d::validate_if_present(body_, "expected_schema_packing", d::JsonKind::String);
    d::validate_if_present(body_, "expected_flexzone_blds", d::JsonKind::String);
    d::validate_if_present(body_, "expected_flexzone_packing", d::JsonKind::String);
    d::validate_if_present(body_, "expected_schema_owner", d::JsonKind::String);
    d::validate_if_present(body_, "consumer_pid", d::JsonKind::U64);
    d::validate_if_present(body_, "consumer_hostname", d::JsonKind::String);
    d::validate_if_present(body_, "build_id", d::JsonKind::String);
    d::validate_if_present(body_, "inbox_endpoint", d::JsonKind::String);
    d::validate_if_present(body_, "inbox_schema_json", d::JsonKind::String);
    d::validate_if_present(body_, "inbox_checksum", d::JsonKind::String);
    // HEP-0046 B.2 — the doubly-encoded inbox schema is parsed ONCE here
    // at the envelope boundary via the canonical hub::parse_schema_json
    // (HEP-0034 §6.2: canonical OBJECT form, in-object `packing`
    // REQUIRED).  Malformed content — non-JSON, bare array, missing /
    // invalid packing, bad field defs — is a BODY_SCHEMA_VIOLATION at
    // parse; no handler or discovery reader ever re-parses the string.
    if (const std::string inbox_s = d::read_string_or_empty(body_, "inbox_schema_json");
        !inbox_s.empty())
    {
        try
        {
            inbox_schema_ = ::pylabhub::hub::parse_schema_json(nlohmann::json::parse(inbox_s));
        }
        catch (const std::exception &e)
        {
            throw WireBodyError(std::string{"wire body: field 'inbox_schema_json' invalid: "} +
                                e.what());
        }
    }
    d::require_security_triple(body_);
}

EndpointUpdateReqBody::EndpointUpdateReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "endpoint_type", d::JsonKind::String);
    d::require(body_, "endpoint", d::JsonKind::String);
    d::require_security_triple(body_);
}

// ChannelAuthAppliedReqBody — HEP-CORE-0042 §5.5.2.  `role_type` is the
// REQUIRED binding-side discriminator ("producer" | "consumer"); the
// 2026-07-11 amendment's absent→"producer" default was migration-window
// text for pre-amendment producer emitters and is retired — every live
// sender (BRC channel_auth_applied) has always written it.
ChannelAuthAppliedReqBody::ChannelAuthAppliedReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require(body_, "role_type", d::JsonKind::String);
    d::require(body_, "applied_version", d::JsonKind::U64);
    // Always present on the single wire shape: producers echo the
    // REG_ACK shift number (nonzero, §5.5.3); consumers send 0 and the
    // broker's consumer branch ignores it (no instance[C] state).
    d::require(body_, "instance_id", d::JsonKind::U64);
    d::require_security_triple(body_);
}

DeregReqBody::DeregReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require_security_triple(body_);
}

RegAckBody::RegAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "heartbeat", d::JsonKind::Object);
    d::require(body_, "initial_allowlist", d::JsonKind::Array);
    d::require(body_, "broker_abi_fingerprint", d::JsonKind::Object);
    d::validate_if_present(body_, "error_code", d::JsonKind::String);
    d::validate_if_present(body_, "message", d::JsonKind::String);
    d::validate_if_present(body_, "instance_id", d::JsonKind::U64);
    d::validate_if_present(body_, "snapshot_version", d::JsonKind::U64);
    d::validate_if_present(body_, "broker_build_id", d::JsonKind::String);
    d::validate_if_present(body_, "broker_observer_pubkey_z85", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

// ConsumerRegAckBody — success-path shape per HEP-CORE-0036 §5b/§6.4.
// Splits from RegAckBody 2026-07-15 (task #45 erratum on §14.3
// catalog): CONSUMER_REG_ACK does NOT carry `initial_allowlist`; it
// carries `producers[]` + `data_transport` (unified transport-
// discriminator shape from B-4 audit).  Broker's ERROR path emits
// msg_type "ERROR", so this class covers only the success shape.
ConsumerRegAckBody::ConsumerRegAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "data_transport", d::JsonKind::String);
    d::require(body_, "heartbeat", d::JsonKind::Object);
    d::require(body_, "producers", d::JsonKind::Array);
    d::require(body_, "broker_abi_fingerprint", d::JsonKind::Object);
    d::validate_if_present(body_, "broker_build_id", d::JsonKind::String);
    // Schema-at-establishment (HEP-CORE-0034 §10.3a): all five optional —
    // absent axes are simply not established on the channel record.
    d::validate_if_present(body_, "schema_id", d::JsonKind::String);
    d::validate_if_present(body_, "schema_owner", d::JsonKind::String);
    d::validate_if_present(body_, "blds", d::JsonKind::String);
    d::validate_if_present(body_, "flexzone_blds", d::JsonKind::String);
    d::validate_if_present(body_, "schema_hash", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

EndpointUpdateAckBody::EndpointUpdateAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::validate_if_present(body_, "message", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

GetChannelAuthReqBody::GetChannelAuthReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

GetChannelAuthAckBody::GetChannelAuthAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require(body_, "allowlist", d::JsonKind::Array);
    d::validate_if_present(body_, "channel_version", d::JsonKind::U64);
    d::require_envelope_hash(body_);
}

ChannelAuthAppliedAckBody::ChannelAuthAppliedAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::validate_if_present(body_, "applied_version", d::JsonKind::U64);
    d::require_envelope_hash(body_);
}

DeregAckBody::DeregAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

// HeartbeatNotifyBody — renamed from HeartbeatReqBody per C13 for
// taxonomy consistency with HEP-CORE-0046 §I-MSG-TYPE-TAXONOMY
// (`_NOTIFY` suffix = fire-and-forget; heartbeat has no ACK path).
// `role_type` REQUIRED per HEP-CORE-0023 §2.5.2.
HeartbeatNotifyBody::HeartbeatNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require(body_, "role_type", d::JsonKind::String);
    d::validate_if_present(body_, "producer_pid", d::JsonKind::U64);
    d::validate_if_present(body_, "metrics", d::JsonKind::Object);
    d::require_envelope_hash(body_);
}

ChannelBroadcastSendBody::ChannelBroadcastSendBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "target_channel", d::JsonKind::String);
    d::refuse_field(body_, "sender_uid",
                    "is not carried by this message: the broker stamps the sender "
                    "from the key the connection proved (HEP-CORE-0035 §4.2.2)");
    d::require_envelope_hash(body_);
}

DiscReqBody::DiscReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

DiscAckBody::DiscAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

ChannelAuthChangedNotifyBody::ChannelAuthChangedNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require(body_, "role_type", d::JsonKind::String);
    d::require(body_, "phase", d::JsonKind::String);
    d::validate_if_present(body_, "channel_version", d::JsonKind::U64);
    d::require_envelope_hash(body_);
}

ChannelClosingNotifyBody::ChannelClosingNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::validate_if_present(body_, "reason", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

ConsumerDiedNotifyBody::ConsumerDiedNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::validate_if_present(body_, "reason", d::JsonKind::String);
    d::validate_if_present(body_, "target_role", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

BandJoinNotifyBody::BandJoinNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "band", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require(body_, "role_name", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

BandLeaveNotifyBody::BandLeaveNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "band", d::JsonKind::String);
    d::require(body_, "role_uid", d::JsonKind::String);
    d::require(body_, "role_name", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

// ── Admin console family (HEP-CORE-0033 §11) ─────────────────────────

AdminHelloReqBody::AdminHelloReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "token", d::JsonKind::String);
    d::require(body_, "label", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

AdminHelloAckBody::AdminHelloAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "session_id", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

AdminPingReqBody::AdminPingReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "session_id", d::JsonKind::String);
    d::require_security_triple(body_); // + client_nonce/client_wall_ts (in-session replay, §11.0.5)
}

AdminPingAckBody::AdminPingAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

AdminCloseChannelReqBody::AdminCloseChannelReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "session_id", d::JsonKind::String);
    d::require(body_, "channel", d::JsonKind::String);
    d::require_security_triple(body_); // + client_nonce/client_wall_ts (in-session replay)
}

AdminCloseChannelAckBody::AdminCloseChannelAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

AdminSessionReqBody::AdminSessionReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "session_id", d::JsonKind::String);
    d::require_security_triple(body_); // + client_nonce/client_wall_ts (in-session replay)
}

AdminNamedReqBody::AdminNamedReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "session_id", d::JsonKind::String);
    d::require(body_, "name", d::JsonKind::String);
    d::require_security_triple(body_); // + client_nonce/client_wall_ts (in-session replay)
}

AdminBroadcastChannelReqBody::AdminBroadcastChannelReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "session_id", d::JsonKind::String);
    d::require(body_, "channel", d::JsonKind::String);
    d::require(body_, "message", d::JsonKind::String);
    d::validate_if_present(body_, "data", d::JsonKind::String);
    d::require_security_triple(body_); // + client_nonce/client_wall_ts (in-session replay)
}

AdminQueryMetricsReqBody::AdminQueryMetricsReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "session_id", d::JsonKind::String);
    d::require(body_, "filter", d::JsonKind::Object); // {} = all categories
    d::require_security_triple(body_); // + client_nonce/client_wall_ts (in-session replay)
}

AdminResultAckBody::AdminResultAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "result", d::JsonKind::Object);
    d::require_envelope_hash(body_);
}

AdminStatusAckBody::AdminStatusAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

AdminErrorBody::AdminErrorBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "code", d::JsonKind::String);
    d::require(body_, "message", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

} // namespace pylabhub::wire
