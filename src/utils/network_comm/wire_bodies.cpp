#include "utils/wire_bodies.hpp"

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
}  // namespace

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
    if (it == body.end()) return {};
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
    if (it == body.end()) return 0;
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
        case JsonKind::String: ok = it->is_string(); break;
        case JsonKind::U32:    ok = it->is_number_unsigned(); break;
        case JsonKind::U64:    ok = it->is_number_unsigned(); break;
        case JsonKind::Object: ok = it->is_object(); break;
        case JsonKind::Array:  ok = it->is_array(); break;
    }
    if (!ok)
    {
        throw WireBodyError(mkerr(field, "wrong JSON kind"));
    }
}

void require_envelope_hash(const nlohmann::json &body)
{
    // WireEnvelope::parse enforces this on inbound; body-class constructors
    // enforce again as a defense-in-depth so a body constructed outside the
    // envelope path (e.g., in a test) still fails loud on missing hash.
    require(body, "envelope_hash", JsonKind::String);
}

void require_security_triple(const nlohmann::json &body)
{
    require_envelope_hash(body);
    require(body, "client_nonce", JsonKind::String);
    require(body, "client_wall_ts", JsonKind::U64);
}

}  // namespace pylabhub::wire::detail

namespace pylabhub::wire
{

// ── Constructor implementations ───────────────────────────────────────
//
// Each constructor validates the fields ITS msg_type carries.  REG-family
// bodies additionally require the security triple; every body requires
// envelope_hash.  Constructor stores the json by value (moved in), so
// subsequent accessor calls read from the cached body_ member.

namespace d = detail;

RegReqBody::RegReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name",     d::JsonKind::String);
    d::require(body_, "role_uid",         d::JsonKind::String);
    d::require(body_, "role_type",        d::JsonKind::String);
    d::require(body_, "role_name",        d::JsonKind::String);
    d::require(body_, "data_transport",   d::JsonKind::String);
    d::require(body_, "zmq_pubkey",       d::JsonKind::String);
    d::require(body_, "broker_proto",     d::JsonKind::U32);
    d::require(body_, "schema_hash",      d::JsonKind::String);
    d::require(body_, "schema_version",   d::JsonKind::U32);
    d::require(body_, "abi_fingerprint",  d::JsonKind::Object);
    d::require_security_triple(body_);
}

EndpointUpdateReqBody::EndpointUpdateReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name",  d::JsonKind::String);
    d::require(body_, "endpoint_type", d::JsonKind::String);
    d::require(body_, "endpoint",      d::JsonKind::String);
    d::require_security_triple(body_);
}

ChannelAuthAppliedReqBody::ChannelAuthAppliedReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name",     d::JsonKind::String);
    d::require(body_, "role_uid",         d::JsonKind::String);
    d::require(body_, "applied_version",  d::JsonKind::U64);
    d::require(body_, "instance_id",      d::JsonKind::U64);
    d::require_security_triple(body_);
}

DeregReqBody::DeregReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid",     d::JsonKind::String);
    d::require_security_triple(body_);
}

RegAckBody::RegAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status",                d::JsonKind::String);
    d::require(body_, "channel_name",          d::JsonKind::String);
    d::require(body_, "heartbeat",             d::JsonKind::Object);
    d::require(body_, "initial_allowlist",     d::JsonKind::Array);
    d::require(body_, "broker_abi_fingerprint",d::JsonKind::Object);
    d::require_envelope_hash(body_);
}

EndpointUpdateAckBody::EndpointUpdateAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

GetChannelAuthReqBody::GetChannelAuthReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid",     d::JsonKind::String);
    d::require_envelope_hash(body_);
}

GetChannelAuthAckBody::GetChannelAuthAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status",    d::JsonKind::String);
    d::require(body_, "allowlist", d::JsonKind::Array);
    d::require_envelope_hash(body_);
}

ChannelAuthAppliedAckBody::ChannelAuthAppliedAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

DeregAckBody::DeregAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

HeartbeatReqBody::HeartbeatReqBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid",     d::JsonKind::String);
    d::require_envelope_hash(body_);
}

HeartbeatAckBody::HeartbeatAckBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "status", d::JsonKind::String);
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
    d::require(body_, "role_uid",     d::JsonKind::String);
    d::require(body_, "role_type",    d::JsonKind::String);
    d::require(body_, "phase",        d::JsonKind::String);
    d::require_envelope_hash(body_);
}

ChannelClosingNotifyBody::ChannelClosingNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

ConsumerDiedNotifyBody::ConsumerDiedNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "channel_name", d::JsonKind::String);
    d::require(body_, "role_uid",     d::JsonKind::String);
    d::require_envelope_hash(body_);
}

BandJoinNotifyBody::BandJoinNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "band",      d::JsonKind::String);
    d::require(body_, "role_uid",  d::JsonKind::String);
    d::require(body_, "role_name", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

BandLeaveNotifyBody::BandLeaveNotifyBody(nlohmann::json body)
{
    body_ = std::move(body);
    d::require(body_, "band",      d::JsonKind::String);
    d::require(body_, "role_uid",  d::JsonKind::String);
    d::require(body_, "role_name", d::JsonKind::String);
    d::require_envelope_hash(body_);
}

}  // namespace pylabhub::wire
