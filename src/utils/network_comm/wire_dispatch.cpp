/**
 * @file wire_dispatch.cpp
 * @brief Implementation of `receive_and_validate` + `build_error_reply`.
 *
 * Composition: wire_envelope::parse_router_recv → typed body class ctor
 * (per msg_type) → tier-appropriate admission gates → return typed
 * Validated<Body> or RejectedMessage.
 *
 * Adding a new msg_type = add a row in `kDispatchTable` + a
 * `Validated*` alternative in the variant.  No handler code touched.
 */

#include "utils/wire_dispatch.hpp"

#include "utils/admission_gates.hpp"
#include "utils/wire_bodies.hpp"
#include "utils/wire_envelope.hpp"

#include "cppzmq/zmq_addon.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <utility>

namespace pylabhub::wire::dispatch
{

namespace
{

using ::pylabhub::admission::AdmissionContext;
using ::pylabhub::admission::AuthenticatedRegFamilyView;
using ::pylabhub::admission::ControlBodyView;
using ::pylabhub::admission::RegFamilyBodyView;
using ::pylabhub::admission::RejectCode;
using ::pylabhub::admission::RejectDetail;

// ── Envelope-parse error → RejectedMessage ────────────────────────────
//
// parse_router_recv returns std::nullopt on ANY envelope-shape failure
// (frame count, control byte, oversized fields, missing correlation_id,
// missing envelope_hash, hash mismatch, body-not-JSON, body-not-object).
// We build a RejectedMessage without identity/corr_id — the sender is
// not reliably identified when the envelope itself failed to parse.
RejectedMessage parse_failure(::pylabhub::wire::ParseError err)
{
    RejectedMessage r;
    r.code = RejectCode::envelope_tampered;
    switch (err)
    {
        case ::pylabhub::wire::ParseError::frame_count:
            r.message = "envelope frame count != 5";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::frame_type_marker:
            r.message = "Frame 1 marker byte != 'C'";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::msg_type_empty:
            r.message = "Frame 2 msg_type is empty";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::msg_type_too_long:
            r.message = "Frame 2 msg_type exceeds 64 bytes";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::correlation_too_long:
            r.message = "Frame 3 correlation_id exceeds 64 bytes";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::correlation_missing:
            r.message = "Frame 3 correlation_id empty on non-NOTIFY "
                        "(I-CORRELATION-STABLE)";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::body_not_json:
            r.message = "Frame 4 body is not valid JSON";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::body_not_object:
            r.message = "Frame 4 body is not a JSON object";
            r.code    = RejectCode::invalid_request;
            break;
        case ::pylabhub::wire::ParseError::envelope_hash_missing:
            r.message = "body missing envelope_hash "
                        "(I-ENVELOPE-BODY-BINDING)";
            r.code    = RejectCode::envelope_tampered;
            break;
        case ::pylabhub::wire::ParseError::envelope_hash_mismatch:
            r.message = "envelope_hash mismatch "
                        "(I-ENVELOPE-BODY-BINDING)";
            r.code    = RejectCode::envelope_tampered;
            break;
    }
    return r;
}

// ── Rejection with envelope preserved (typed-body ctor threw etc.) ─────
//
// Once the envelope is parsed, we know identity + corr_id + msg_type
// even if downstream validation fails.  Populate the reject fields so
// the caller can build a targeted ERROR reply.
RejectedMessage rejection_with_envelope(
    const ::pylabhub::wire::WireEnvelope &env,
    RejectDetail                          detail)
{
    RejectedMessage r;
    r.identity       = std::string(env.identity());
    r.correlation_id = std::string(env.correlation_id());
    r.msg_type       = std::string(env.msg_type());
    r.code           = detail.code;
    r.field          = std::move(detail.field);
    r.message        = std::move(detail.message);
    return r;
}

// ── Body-view builders (per typed body class) ─────────────────────────
//
// String holders live in the caller's frame so string_views stay valid
// for the duration of gate execution.
struct RegReqHolders
{
    std::string role_uid, channel_name, zmq_pubkey, client_nonce;
};
// Producer + Consumer bodies both expose the same universal
// identity / pubkey / security-triple accessors used by the
// admission gates; the templated helper works for either body
// class.  No `broker_proto` extraction — retired per C3.
template <typename Body>
RegFamilyBodyView view_of_reg(const Body    &body,
                                RegReqHolders &h)
{
    h.role_uid     = body.role_uid();
    h.channel_name = body.channel_name();
    h.zmq_pubkey   = body.zmq_pubkey();
    h.client_nonce = body.client_nonce();
    RegFamilyBodyView v;
    v.role_uid       = h.role_uid;
    v.channel_name   = h.channel_name;
    v.zmq_pubkey     = h.zmq_pubkey;
    v.client_nonce   = h.client_nonce;
    v.client_wall_ts = body.client_wall_ts();
    return v;
}

struct AuthRegHolders
{
    std::string role_uid, channel_name, client_nonce;
};

template <typename Body>
AuthenticatedRegFamilyView auth_view_of(const Body       &body,
                                          AuthRegHolders &h)
{
    h.role_uid     = body.role_uid();
    h.channel_name = body.channel_name();
    h.client_nonce = body.client_nonce();
    AuthenticatedRegFamilyView v;
    v.role_uid       = h.role_uid;
    v.channel_name   = h.channel_name;
    v.client_nonce   = h.client_nonce;
    v.client_wall_ts = body.client_wall_ts();
    return v;
}

// EndpointUpdateReqBody has channel_name + endpoint_type + endpoint,
// no role_uid field.  Identity match uses env.identity() as the role_uid
// authority — for the auth-tier view we substitute env.identity() as
// role_uid so identity check trivially passes (already validated).  The
// replay + grammar-on-channel_name still apply.
AuthenticatedRegFamilyView auth_view_of_endpoint_update(
    const ::pylabhub::wire::EndpointUpdateReqBody &body,
    const ::pylabhub::wire::WireEnvelope          &env,
    AuthRegHolders                                &h)
{
    h.role_uid     = std::string(env.identity());  // authority is Frame 0
    h.channel_name = body.channel_name();
    h.client_nonce = body.client_nonce();
    AuthenticatedRegFamilyView v;
    v.role_uid       = h.role_uid;
    v.channel_name   = h.channel_name;
    v.client_nonce   = h.client_nonce;
    v.client_wall_ts = body.client_wall_ts();
    return v;
}

// ── Per-msg_type validation routines ──────────────────────────────────
//
// Each returns a ReceivedMessage variant.  Called from receive_and_validate
// after envelope parse + msg_type dispatch.  On typed-body ctor failure
// (missing/mistyped field) all catch WireBodyError and produce a
// RejectedMessage with body_schema_violation.

ReceivedMessage validate_producer_reg_req(::pylabhub::wire::WireEnvelope env,
                                           ::nlohmann::json               body_json,
                                           const AdmissionContext        &ctx)
{
    try
    {
        ::pylabhub::wire::ProducerRegReqBody typed(std::move(body_json));
        RegReqHolders                        holders;
        auto view = view_of_reg(typed, holders);
        if (auto r = ::pylabhub::admission::run_reg_family_gates(env, view, ctx))
        {
            return rejection_with_envelope(env, std::move(*r));
        }
        return ValidatedRegReq{std::move(env), std::move(typed)};
    }
    catch (const ::pylabhub::wire::WireBodyError &e)
    {
        RejectDetail d;
        d.code    = RejectCode::body_schema_violation;
        d.field   = "";
        d.message = e.what();
        return rejection_with_envelope(env, std::move(d));
    }
}

ReceivedMessage validate_consumer_reg_req(::pylabhub::wire::WireEnvelope env,
                                           ::nlohmann::json               body_json,
                                           const AdmissionContext        &ctx)
{
    try
    {
        ::pylabhub::wire::ConsumerRegReqBody typed(std::move(body_json));
        RegReqHolders                        holders;
        auto view = view_of_reg(typed, holders);
        if (auto r = ::pylabhub::admission::run_reg_family_gates(env, view, ctx))
        {
            return rejection_with_envelope(env, std::move(*r));
        }
        return ValidatedConsumerRegReq{std::move(env), std::move(typed)};
    }
    catch (const ::pylabhub::wire::WireBodyError &e)
    {
        RejectDetail d;
        d.code    = RejectCode::body_schema_violation;
        d.field   = "";
        d.message = e.what();
        return rejection_with_envelope(env, std::move(d));
    }
}

template <typename BodyClass, typename ValidatedT>
ReceivedMessage validate_auth_reg_family(::pylabhub::wire::WireEnvelope env,
                                           ::nlohmann::json               body_json,
                                           const AdmissionContext        &ctx)
{
    try
    {
        BodyClass       typed(std::move(body_json));
        AuthRegHolders  holders;
        auto view = auth_view_of(typed, holders);
        if (auto r = ::pylabhub::admission::run_authenticated_reg_family_gates(
                env, view, ctx))
        {
            return rejection_with_envelope(env, std::move(*r));
        }
        return ValidatedT{std::move(env), std::move(typed)};
    }
    catch (const ::pylabhub::wire::WireBodyError &e)
    {
        RejectDetail d;
        d.code    = RejectCode::body_schema_violation;
        d.field   = "";
        d.message = e.what();
        return rejection_with_envelope(env, std::move(d));
    }
}

ReceivedMessage validate_endpoint_update(::pylabhub::wire::WireEnvelope env,
                                           ::nlohmann::json               body_json,
                                           const AdmissionContext        &ctx)
{
    try
    {
        ::pylabhub::wire::EndpointUpdateReqBody typed(std::move(body_json));
        AuthRegHolders                          holders;
        auto view = auth_view_of_endpoint_update(typed, env, holders);
        if (auto r = ::pylabhub::admission::run_authenticated_reg_family_gates(
                env, view, ctx))
        {
            return rejection_with_envelope(env, std::move(*r));
        }
        return ValidatedEndpointUpdateReq{std::move(env), std::move(typed)};
    }
    catch (const ::pylabhub::wire::WireBodyError &e)
    {
        RejectDetail d;
        d.code    = RejectCode::body_schema_violation;
        d.field   = "";
        d.message = e.what();
        return rejection_with_envelope(env, std::move(d));
    }
}

// SFINAE probe: does BodyClass expose a role_type() accessor?  Only
// HeartbeatNotifyBody carries it — used to populate ControlBodyView so
// gate_role_tag_policy can enforce HEP-CORE-0033 §G2.2.0b.8 for
// HEARTBEAT_NOTIFY.
template <typename T, typename = void>
struct has_role_type : std::false_type {};
template <typename T>
struct has_role_type<T, std::void_t<decltype(std::declval<const T &>().role_type())>>
    : std::true_type {};

template <typename BodyClass, typename ValidatedT>
ReceivedMessage validate_control_with_role_uid(
    ::pylabhub::wire::WireEnvelope env,
    ::nlohmann::json               body_json,
    const AdmissionContext        &ctx)
{
    try
    {
        BodyClass typed(std::move(body_json));
        std::string role_uid     = typed.role_uid();
        std::string channel_name = typed.channel_name();
        std::string role_type;
        if constexpr (has_role_type<BodyClass>::value)
        {
            role_type = typed.role_type();
        }
        ControlBodyView v;
        v.role_uid     = role_uid;
        v.channel_name = channel_name;
        v.role_type    = role_type;
        if (auto r = ::pylabhub::admission::run_control_gates(env, v, ctx))
        {
            return rejection_with_envelope(env, std::move(*r));
        }
        return ValidatedT{std::move(env), std::move(typed)};
    }
    catch (const ::pylabhub::wire::WireBodyError &e)
    {
        RejectDetail d;
        d.code    = RejectCode::body_schema_violation;
        d.field   = "";
        d.message = e.what();
        return rejection_with_envelope(env, std::move(d));
    }
}

// DISC_REQ carries only channel_name — no role_uid to identity-check
// against.  Envelope hash from parse is the only assurance.
ReceivedMessage validate_disc_req(::pylabhub::wire::WireEnvelope env,
                                    ::nlohmann::json               body_json,
                                    const AdmissionContext        &/*ctx*/)
{
    try
    {
        ::pylabhub::wire::DiscReqBody typed(std::move(body_json));
        return ValidatedDiscReq{std::move(env), std::move(typed)};
    }
    catch (const ::pylabhub::wire::WireBodyError &e)
    {
        RejectDetail d;
        d.code    = RejectCode::body_schema_violation;
        d.field   = "";
        d.message = e.what();
        return rejection_with_envelope(env, std::move(d));
    }
}

// EnvelopeOnly fallback — typed body class doesn't exist yet for this
// msg_type.  Envelope hash IS validated (by parse), so the sender's
// header cannot be spliced.  Body is passed through as-is; downstream
// handler reads with body.value(...) at its own risk.  Future commits
// will replace these with typed body classes as they land.
ReceivedMessage envelope_only(::pylabhub::wire::WireEnvelope env,
                                ::nlohmann::json               body_json)
{
    return ValidatedRawControl{std::move(env), std::move(body_json)};
}

// ── Dispatch table ─────────────────────────────────────────────────────
//
// Adding a new msg_type: add a row here + (if REG-family / typed control)
// a Validated<Body> alternative in the variant + a validate_* fn above.
// EnvelopeOnly rows only need the msg_type row; no per-type validated
// alternative.
enum class Tier
{
    RegReq,                 // producer REG_REQ — ProducerRegReqBody + full gates
    ConsumerRegReq,         // CONSUMER_REG_REQ — ConsumerRegReqBody + full gates
    AuthReg_Dereg,          // authenticated REG-family (identity + replay)
    AuthReg_ConsumerDereg,
    AuthReg_EndpointUpdate, // EndpointUpdateReqBody has no role_uid — special
    AuthReg_ChanAuthApplied,
    Control_HeartbeatNotify,// HEARTBEAT_NOTIFY (renamed from HEARTBEAT_REQ per C13)
    Control_GetChannelAuth,
    Control_Disc,           // no role_uid — envelope-only in effect
    EnvelopeOnly,           // msg_type known but no typed body yet
    // (unknown msg_type → EnvelopeOnly variant + broker rejects at handler)
};

struct DispatchRow
{
    std::string_view msg_type;
    Tier             tier;
};

// Every msg_type the broker handles (per broker_service.cpp process_message).
// Order doesn't matter for correctness; kept alphabetical within a group.
// C11 ERRATUM 2026-07-14: CHANNEL_BROADCAST_SEND_NOTIFY is NOT retired
// (see HEP-CORE-0030 §9.1 coexistence: channel-bound vs band-bound
// broadcast serve different membership axes).  The prior claim that
// it was superseded by BAND_BROADCAST was wrong.  Both live msg_types
// were renamed from `_REQ` to `_SEND_NOTIFY` per HEP-CORE-0046
// I-MSG-TYPE-TAXONOMY (fire-and-forget requires _NOTIFY suffix); the
// broker→recipient fan-out `_NOTIFY` was renamed to `_DELIVER_NOTIFY`
// to disambiguate.
constexpr std::array<DispatchRow, 21> kDispatchTable = {{
    // REG_REQ family — full gates.  Producer + consumer paths use
    // distinct body classes per C1 resolution.
    {"REG_REQ",                 Tier::RegReq},
    {"CONSUMER_REG_REQ",        Tier::ConsumerRegReq},

    // Authenticated REG-family — identity + replay
    {"DEREG_REQ",               Tier::AuthReg_Dereg},
    {"CONSUMER_DEREG_REQ",      Tier::AuthReg_ConsumerDereg},
    {"ENDPOINT_UPDATE_REQ",     Tier::AuthReg_EndpointUpdate},
    {"CHANNEL_AUTH_APPLIED_REQ",Tier::AuthReg_ChanAuthApplied},

    // Control tier (identity check when body carries role_uid)
    // HEARTBEAT_NOTIFY (renamed from HEARTBEAT_REQ per C13) is
    // fire-and-forget per HEP-CORE-0046 §I-MSG-TYPE-TAXONOMY.
    {"HEARTBEAT_NOTIFY",        Tier::Control_HeartbeatNotify},
    {"GET_CHANNEL_AUTH_REQ",    Tier::Control_GetChannelAuth},
    {"DISC_REQ",                Tier::Control_Disc},

    // EnvelopeOnly — typed body class TBD; envelope hash still validated
    {"CHECK_PEER_READY_REQ",    Tier::EnvelopeOnly},
    {"SCHEMA_REQ",              Tier::EnvelopeOnly},
    {"CHANNEL_LIST_REQ",        Tier::EnvelopeOnly},
    {"METRICS_REQ",             Tier::EnvelopeOnly},
    {"SHM_BLOCK_QUERY_REQ",     Tier::EnvelopeOnly},
    {"ROLE_PRESENCE_REQ",       Tier::EnvelopeOnly},
    {"ROLE_INFO_REQ",           Tier::EnvelopeOnly},
    {"BAND_JOIN_REQ",                  Tier::EnvelopeOnly},
    {"BAND_LEAVE_REQ",                 Tier::EnvelopeOnly},
    {"BAND_BROADCAST_SEND_NOTIFY",     Tier::EnvelopeOnly},
    {"BAND_MEMBERS_REQ",               Tier::EnvelopeOnly},
    {"CHANNEL_BROADCAST_SEND_NOTIFY",  Tier::EnvelopeOnly},
}};

std::optional<Tier> lookup_tier(std::string_view msg_type) noexcept
{
    for (const auto &row : kDispatchTable)
    {
        if (row.msg_type == msg_type) return row.tier;
    }
    return std::nullopt;
}

std::string_view tier_name(Tier t) noexcept
{
    switch (t)
    {
        case Tier::RegReq:                       return "RegReq";
        case Tier::ConsumerRegReq:               return "ConsumerRegReq";
        case Tier::AuthReg_Dereg:                return "AuthReg_Dereg";
        case Tier::AuthReg_ConsumerDereg:        return "AuthReg_ConsumerDereg";
        case Tier::AuthReg_EndpointUpdate:       return "AuthReg_EndpointUpdate";
        case Tier::AuthReg_ChanAuthApplied:      return "AuthReg_ChanAuthApplied";
        case Tier::Control_HeartbeatNotify:      return "Control_HeartbeatNotify";
        case Tier::Control_GetChannelAuth:       return "Control_GetChannelAuth";
        case Tier::Control_Disc:                 return "Control_Disc";
        case Tier::EnvelopeOnly:                 return "EnvelopeOnly";
    }
    return "UNKNOWN";
}

}  // anonymous namespace

std::optional<std::string_view>
tier_for_msg_type(std::string_view msg_type) noexcept
{
    if (auto t = lookup_tier(msg_type); t.has_value())
    {
        return tier_name(*t);
    }
    return std::nullopt;
}

std::size_t dispatch_table_size() noexcept
{
    return kDispatchTable.size();
}

ReceivedMessage
receive_and_validate(::zmq::multipart_t                             &&raw,
                      const ::pylabhub::admission::AdmissionContext &admission_ctx)
{
    ::pylabhub::wire::ParseError err = {};
    auto env_opt =
        ::pylabhub::wire::WireEnvelope::parse_router_recv(std::move(raw), &err);
    if (!env_opt.has_value())
    {
        return parse_failure(err);
    }
    ::pylabhub::wire::WireEnvelope env = std::move(*env_opt);

    // Copy body out of the envelope for typed-body construction (RegReqBody
    // etc. take an owning json).  Envelope keeps its own body copy for
    // accessors on the Validated<Body> variant.
    ::nlohmann::json body_copy = env.body();

    const std::string_view msg_type = env.msg_type();
    auto tier = lookup_tier(msg_type);
    if (!tier.has_value())
    {
        // Unknown msg_type — return an EnvelopeOnly variant so the broker
        // handler can dispatch to its UNKNOWN_MSG_TYPE reply path with
        // correlation_id echoed.  Envelope hash was validated at parse.
        return envelope_only(std::move(env), std::move(body_copy));
    }

    switch (*tier)
    {
        case Tier::RegReq:
            return validate_producer_reg_req(std::move(env),
                                              std::move(body_copy),
                                              admission_ctx);
        case Tier::ConsumerRegReq:
            return validate_consumer_reg_req(std::move(env),
                                              std::move(body_copy),
                                              admission_ctx);
        case Tier::AuthReg_Dereg:
            return validate_auth_reg_family<
                ::pylabhub::wire::DeregReqBody, ValidatedDeregReq>(
                std::move(env), std::move(body_copy), admission_ctx);
        case Tier::AuthReg_ConsumerDereg:
            return validate_auth_reg_family<
                ::pylabhub::wire::DeregReqBody, ValidatedConsumerDeregReq>(
                std::move(env), std::move(body_copy), admission_ctx);
        case Tier::AuthReg_EndpointUpdate:
            return validate_endpoint_update(std::move(env), std::move(body_copy),
                                              admission_ctx);
        case Tier::AuthReg_ChanAuthApplied:
            return validate_auth_reg_family<
                ::pylabhub::wire::ChannelAuthAppliedReqBody,
                ValidatedChannelAuthAppliedReq>(
                std::move(env), std::move(body_copy), admission_ctx);
        case Tier::Control_HeartbeatNotify:
            return validate_control_with_role_uid<
                ::pylabhub::wire::HeartbeatNotifyBody,
                ValidatedHeartbeatNotify>(
                std::move(env), std::move(body_copy), admission_ctx);
        case Tier::Control_GetChannelAuth:
            return validate_control_with_role_uid<
                ::pylabhub::wire::GetChannelAuthReqBody,
                ValidatedGetChannelAuthReq>(
                std::move(env), std::move(body_copy), admission_ctx);
        case Tier::Control_Disc:
            return validate_disc_req(std::move(env), std::move(body_copy),
                                       admission_ctx);
        case Tier::EnvelopeOnly:
            return envelope_only(std::move(env), std::move(body_copy));
    }
    // Unreachable given exhaustive switch, but keep a safe fallback.
    return envelope_only(std::move(env), std::move(body_copy));
}

// ── ERROR reply builder ────────────────────────────────────────────────

::zmq::multipart_t build_error_reply(const RejectedMessage &rej)
{
    ::nlohmann::json body = ::nlohmann::json::object();
    body["status"]         = "error";
    body["error_code"]     = std::string(
        ::pylabhub::admission::to_wire_string(rej.code));
    body["message"]        = rej.message;
    if (!rej.field.empty()) body["field"] = rej.field;
    if (!rej.correlation_id.empty())
        body["correlation_id"] = rej.correlation_id;
    // Envelope hash + wire framing handled by build_router_send.
    return ::pylabhub::wire::WireEnvelope::build_router_send(
        rej.identity, "ERROR", rej.correlation_id, std::move(body));
}

}  // namespace pylabhub::wire::dispatch
