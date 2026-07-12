#include "utils/admission_gates.hpp"

#include "utils/wire_envelope.hpp"

#include <cstring>
#include <string>

// HEP-CORE-0033 §G2.2.0b role_uid / channel_name grammar validator lives
// on the broker side today (validate_identity_fields helpers).  Rather
// than pull those into this TU, gate_grammar delegates to a minimal
// local check that matches the wire-shape contract:
//   - non-empty
//   - length <= 128
//   - all characters in [A-Za-z0-9._\-] plus optional tag prefix
// The broker's richer grammar validator runs downstream as part of
// protocol admission (topology-side); gate_grammar is the wire-boundary
// sanity check.  Any handler that needs the fully-qualified grammar
// still calls validate_identity_fields as it does today.

namespace pylabhub::admission
{

namespace
{

// Match [A-Za-z0-9._-] — the intersection of what all downstream
// identifier consumers accept.  Rejects control chars, embedded nulls,
// whitespace, path separators, quote chars — all classes of input that
// have caused wire-boundary bugs historically.
bool is_id_char(char c) noexcept
{
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return c == '.' || c == '_' || c == '-';
}

bool grammar_ok(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 128) return false;
    for (char c : s)
    {
        if (!is_id_char(c)) return false;
    }
    return true;
}

// Compose a RejectDetail with a machine code and a human-friendly
// message.  Field name is optional.  Kept as a local helper so gate
// bodies stay focused on the check, not on error-detail plumbing.
RejectDetail make(RejectCode code, std::string field, std::string msg)
{
    RejectDetail d;
    d.code    = code;
    d.field   = std::move(field);
    d.message = std::move(msg);
    return d;
}

}  // namespace

std::string_view to_wire_string(RejectCode code) noexcept
{
    switch (code)
    {
        case RejectCode::envelope_tampered:       return "ENVELOPE_TAMPERED";
        case RejectCode::unsupported_proto:       return "UNSUPPORTED_PROTO";
        case RejectCode::body_schema_violation:   return "BODY_SCHEMA_VIOLATION";
        case RejectCode::identity_mismatch:       return "IDENTITY_MISMATCH";
        case RejectCode::invalid_request:         return "INVALID_REQUEST";
        case RejectCode::unknown_role:            return "UNKNOWN_ROLE";
        case RejectCode::pubkey_mismatch:         return "PUBKEY_MISMATCH";
        case RejectCode::uid_conflict:            return "UID_CONFLICT";
        case RejectCode::key_rotation_required:
            return "KEY_ROTATION_REQUIRES_DEREG";
        case RejectCode::replay_or_skew:          return "REPLAY_OR_SKEW";
        case RejectCode::broker_internal_error:   return "BROKER_INTERNAL_ERROR";
    }
    return "UNKNOWN";  // unreachable under complete switch
}

// ── Individual gates ──────────────────────────────────────────────────

std::optional<RejectDetail>
gate_supported_proto(const RegFamilyBodyView &body,
                      const AdmissionContext  &ctx) noexcept
{
    if (body.broker_proto != ctx.broker_proto)
    {
        return make(RejectCode::unsupported_proto, "broker_proto",
                    "broker_proto=" + std::to_string(body.broker_proto) +
                        " does not match broker threshold " +
                        std::to_string(ctx.broker_proto));
    }
    return std::nullopt;
}

std::optional<RejectDetail>
gate_identity_match(const ::pylabhub::wire::WireEnvelope &env,
                     const RegFamilyBodyView              &body) noexcept
{
    // I-DEALER-IDENTITY: routing_id captured in Frame 0 must equal the
    // self-declared role_uid.  Bytewise compare — role_uid grammar
    // guarantees ASCII; identity is opaque bytes matching the DEALER's
    // routing_id set at connect().
    if (env.identity() != body.role_uid)
    {
        return make(RejectCode::identity_mismatch, "role_uid",
                    "ROUTER identity does not equal payload role_uid "
                    "(I-DEALER-IDENTITY)");
    }
    return std::nullopt;
}

std::optional<RejectDetail>
gate_grammar(const RegFamilyBodyView &body) noexcept
{
    // §14.5 gate 4: HEP-CORE-0033 §G2.2.0b grammar on the universal
    // identifiers.  role_uid + channel_name run the identifier grammar;
    // zmq_pubkey is a fixed-length Z85 encoding (charset outside the id
    // grammar), so only length is checked here.  ZAP handshake proves
    // pubkey cryptographic validity downstream.
    //
    // Note on role_name: design §14.5 lists role_name in gate 4, but
    // role_name only appears on `RegReqBody` (REG_REQ / CONSUMER_REG_REQ);
    // other REG-family bodies (EndpointUpdateReqBody,
    // ChannelAuthAppliedReqBody, DeregReqBody, HeartbeatReqBody,
    // GetChannelAuthReqBody) do not carry it.  This shared gate covers
    // the universal fields; role_name grammar is validated at the
    // REG_REQ-specific admission step by the commit callback (which
    // knows the specific body class).
    if (!grammar_ok(body.role_uid))
    {
        return make(RejectCode::invalid_request, "role_uid",
                    "role_uid fails identifier grammar "
                    "(HEP-CORE-0033 §G2.2.0b)");
    }
    if (!grammar_ok(body.channel_name))
    {
        return make(RejectCode::invalid_request, "channel_name",
                    "channel_name fails identifier grammar "
                    "(HEP-CORE-0033 §G2.2.0b)");
    }
    if (body.zmq_pubkey.size() != 40)
    {
        return make(RejectCode::invalid_request, "zmq_pubkey",
                    "zmq_pubkey length is " +
                        std::to_string(body.zmq_pubkey.size()) +
                        ", expected 40 (Z85-encoded CURVE25519)");
    }
    return std::nullopt;
}

std::optional<RejectDetail>
gate_known_role_binding(const RegFamilyBodyView &body,
                         const AdmissionContext  &ctx) noexcept
{
    if (!ctx.cb || !ctx.cb->lookup_known_role)
    {
        // Programmer error: pipeline invoked without callbacks bound.
        return make(RejectCode::broker_internal_error, "",
                    "internal: known_roles callback not bound");
    }
    const auto result = ctx.cb->lookup_known_role(body.role_uid,
                                                    body.zmq_pubkey);
    switch (result)
    {
        case KnownRoleLookup::binding_matches:
            return std::nullopt;
        case KnownRoleLookup::uid_unknown:
            return make(RejectCode::unknown_role, "role_uid",
                        "role_uid not present in known_roles");
        case KnownRoleLookup::pubkey_mismatch:
            return make(RejectCode::pubkey_mismatch, "zmq_pubkey",
                        "zmq_pubkey does not match known_roles entry "
                        "for this role_uid");
    }
    return std::nullopt;  // unreachable
}

std::optional<RejectDetail>
gate_key_rotation(const RegFamilyBodyView &body,
                   const AdmissionContext  &ctx) noexcept
{
    if (!ctx.cb || !ctx.cb->check_key_rotation)
    {
        return make(RejectCode::invalid_request, "",
                    "internal: key_rotation callback not bound");
    }
    const auto result = ctx.cb->check_key_rotation(body.role_uid,
                                                    body.zmq_pubkey);
    switch (result)
    {
        case KeyRotationCheck::not_yet_registered:
        case KeyRotationCheck::matches_current:
            return std::nullopt;
        case KeyRotationCheck::rotation_attempted:
            return make(RejectCode::key_rotation_required, "zmq_pubkey",
                        "in-band key rotation not supported "
                        "(I-KEY-ROTATION-VIA-DEREG); DEREG the role, "
                        "update known_roles config, then re-REG");
    }
    return std::nullopt;
}

std::optional<RejectDetail>
gate_replay_bound(const RegFamilyBodyView &body,
                   const AdmissionContext  &ctx) noexcept
{
    if (!ctx.cb || !ctx.cb->record_and_check_nonce || !ctx.cb->wall_now_ms)
    {
        return make(RejectCode::invalid_request, "",
                    "internal: replay-check callback not bound");
    }
    // Wall-clock skew check runs first so a clock-off client fails fast
    // before its nonce enters the dedup map (avoids polluting the map
    // with entries an operator has to interpret).
    const std::uint64_t broker_now = ctx.cb->wall_now_ms();
    const std::uint64_t client_ts  = body.client_wall_ts;
    const std::uint64_t delta_ms   = broker_now > client_ts
                                          ? broker_now - client_ts
                                          : client_ts - broker_now;
    if (delta_ms > ctx.skew_tolerance_ms)
    {
        return make(RejectCode::replay_or_skew, "client_wall_ts",
                    "wall-clock skew " + std::to_string(delta_ms) +
                        " ms exceeds tolerance " +
                        std::to_string(ctx.skew_tolerance_ms) + " ms");
    }
    // Nonce dedup — records the (role_uid, nonce) pair with the wall_ts
    // and returns true if fresh, false if a duplicate is already in
    // the sliding window.
    if (!ctx.cb->record_and_check_nonce(body.role_uid, body.client_nonce,
                                          body.client_wall_ts))
    {
        return make(RejectCode::replay_or_skew, "client_nonce",
                    "client_nonce reused within "
                    "replay window (I-REPLAY-BOUND)");
    }
    return std::nullopt;
}

// ── Pipeline runner ───────────────────────────────────────────────────

std::optional<RejectDetail>
run_reg_family_gates(const ::pylabhub::wire::WireEnvelope &env,
                      const RegFamilyBodyView              &body,
                      const AdmissionContext               &ctx) noexcept
{
    // §14.5 gate order.  Gate 1 (envelope hash) already ran at
    // WireEnvelope::parse; if we're here, hash is valid.
    if (auto r = gate_supported_proto(body, ctx))         return r;
    if (auto r = gate_identity_match(env, body))          return r;
    if (auto r = gate_grammar(body))                       return r;
    if (auto r = gate_known_role_binding(body, ctx))       return r;
    if (auto r = gate_key_rotation(body, ctx))             return r;
    if (auto r = gate_replay_bound(body, ctx))             return r;
    return std::nullopt;  // Admitted; caller runs topology/cardinality/schema.
}

}  // namespace pylabhub::admission
