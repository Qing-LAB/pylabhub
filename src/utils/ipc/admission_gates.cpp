#include "utils/admission_gates.hpp"

#include "utils/naming.hpp"
#include "utils/wire_envelope.hpp"

#include <array>
#include <cstring>
#include <string>
#include <string_view>

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
        case RejectCode::body_schema_violation:   return "BODY_SCHEMA_VIOLATION";
        case RejectCode::identity_mismatch:       return "IDENTITY_MISMATCH";
        case RejectCode::invalid_request:         return "INVALID_REQUEST";
        case RejectCode::unknown_role:            return "UNKNOWN_ROLE";
        case RejectCode::pubkey_mismatch:         return "PUBKEY_MISMATCH";
        case RejectCode::uid_conflict:            return "UID_CONFLICT";
        case RejectCode::key_rotation_required:
            return "KEY_ROTATION_REQUIRES_DEREG";
        case RejectCode::replay_or_skew:          return "REPLAY_OR_SKEW";
        case RejectCode::invalid_role_tag:        return "INVALID_ROLE_TAG";
        case RejectCode::broker_internal_error:   return "BROKER_INTERNAL_ERROR";
    }
    return "UNKNOWN";  // unreachable under complete switch
}

// ── Individual gates ──────────────────────────────────────────────────

// gate_supported_proto retired per C3.  Wire-version + ABI checked
// through `abi_fingerprint` at the broker's REG handler per
// HEP-CORE-0032 §8.

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

// Shared I-REPLAY-BOUND check (wall-clock skew + nonce dedup).  ONE
// implementation, called by `gate_replay_bound` (REG_REQ / CONSUMER_REG_REQ
// path) AND by `run_authenticated_reg_family_gates` (DEREG / CONSUMER_DEREG /
// ENDPOINT_UPDATE / CHANNEL_AUTH_APPLIED path).  De-duplicated 2026-07-17 so
// the two callers cannot drift (was: a verbatim inline copy in the auth-reg
// runner).  File-local; both callers live in this TU.
static std::optional<RejectDetail>
check_replay_bound(std::string_view        role_uid,
                    std::string_view        client_nonce,
                    std::uint64_t           client_wall_ts,
                    const AdmissionContext &ctx) noexcept
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
    const std::uint64_t delta_ms   = broker_now > client_wall_ts
                                          ? broker_now - client_wall_ts
                                          : client_wall_ts - broker_now;
    if (delta_ms > ctx.skew_tolerance_ms)
    {
        return make(RejectCode::replay_or_skew, "client_wall_ts",
                    "wall-clock skew " + std::to_string(delta_ms) +
                        " ms exceeds tolerance " +
                        std::to_string(ctx.skew_tolerance_ms) + " ms");
    }
    // Nonce dedup — records the (role_uid, nonce) pair with the wall_ts
    // and returns true if fresh, false if a duplicate is already in
    // the sliding window.  NOTE: `ctx.nonce_window_ms` MUST be >=
    // `ctx.skew_tolerance_ms` for soundness (see AdmissionContext).
    if (!ctx.cb->record_and_check_nonce(role_uid, client_nonce, client_wall_ts))
    {
        return make(RejectCode::replay_or_skew, "client_nonce",
                    "client_nonce reused within "
                    "replay window (I-REPLAY-BOUND)");
    }
    return std::nullopt;
}

std::optional<RejectDetail>
gate_replay_bound(const RegFamilyBodyView &body,
                   const AdmissionContext  &ctx) noexcept
{
    return check_replay_bound(body.role_uid, body.client_nonce,
                              body.client_wall_ts, ctx);
}

// ── Per-msg-type role-tag policy (HEP-CORE-0033 §G2.2.0b.8) ───────────
//
// One table.  Rows are the msg_types that get restricted; anything not
// listed falls through to the universal `{prod, cons, proc}` set.
// HEARTBEAT_NOTIFY is special-cased below — its allowed tag is derived
// from the body's `role_type` field rather than a fixed row.
namespace
{

// Sentinel tag values used inside the table.  Kept as compile-time
// constants so the table is a plain array of small string_views.
constexpr std::string_view kTagProd = "prod";
constexpr std::string_view kTagCons = "cons";
constexpr std::string_view kTagProc = "proc";

struct RoleTagRow
{
    std::string_view msg_type;
    std::array<std::string_view, 3> allowed;  ///< empties = unused slot
};

// clang-format off
constexpr std::array<RoleTagRow, 4> kRoleTagTable = {{
    { "REG_REQ",             { kTagProd, kTagProc, {} } },
    { "DEREG_REQ",           { kTagProd, kTagProc, {} } },
    { "CONSUMER_REG_REQ",    { kTagCons, kTagProc, {} } },
    { "CONSUMER_DEREG_REQ",  { kTagCons, kTagProc, {} } },
}};
// clang-format on

bool tag_in(std::string_view                             tag,
             const std::array<std::string_view, 3>       &allowed) noexcept
{
    for (const auto &a : allowed)
    {
        if (!a.empty() && a == tag) return true;
    }
    return false;
}

std::string format_allowed(const std::array<std::string_view, 3> &allowed)
{
    std::string out;
    bool first = true;
    for (const auto &a : allowed)
    {
        if (a.empty()) continue;
        if (!first) out.append(", ");
        out.append(a);
        first = false;
    }
    return out;
}

}  // namespace

std::optional<RejectDetail>
gate_role_tag_policy(std::string_view msg_type,
                      std::string_view role_uid,
                      std::string_view role_type_field) noexcept
{
    // Extract the short tag from the uid.  role_uid grammar ran before us,
    // so a nullopt here is a broker-side bug (grammar accepted a uid
    // whose short tag is unparseable).
    const auto tag_opt = ::pylabhub::hub::extract_short_tag(role_uid);
    if (!tag_opt.has_value())
    {
        return make(RejectCode::broker_internal_error, "role_uid",
                    "internal: role_uid passed grammar but tag "
                    "extraction failed");
    }
    const std::string_view tag = *tag_opt;

    // HEARTBEAT_NOTIFY: allowed tag derived from body's role_type field.
    // A HEARTBEAT declaring role_type="producer" MUST carry a `prod.*` uid,
    // consumer → `cons.*`, processor → `proc.*`.  Empty role_type on
    // HEARTBEAT_NOTIFY is a wire violation caught by body construction;
    // if it slips through, reject here rather than falling through to
    // the universal set.
    if (msg_type == "HEARTBEAT_NOTIFY")
    {
        std::string_view required_tag;
        if      (role_type_field == "producer")  required_tag = kTagProd;
        else if (role_type_field == "consumer")  required_tag = kTagCons;
        else if (role_type_field == "processor") required_tag = kTagProc;
        else
        {
            return make(RejectCode::invalid_request, "role_type",
                        "HEARTBEAT_NOTIFY role_type must be one of "
                        "producer|consumer|processor (HEP-CORE-0033 "
                        "§G2.2.0b.8)");
        }
        if (tag != required_tag)
        {
            return make(RejectCode::invalid_role_tag, "role_uid",
                        std::string{"role_uid tag '"} + std::string{tag} +
                            "' does not match declared role_type '" +
                            std::string{role_type_field} + "' (expected '" +
                            std::string{required_tag} + "')");
        }
        return std::nullopt;
    }

    // Table lookup.  Msg_types not listed fall through to the universal
    // {prod, cons, proc} set — any recognized tag passes.
    for (const auto &row : kRoleTagTable)
    {
        if (row.msg_type == msg_type)
        {
            if (!tag_in(tag, row.allowed))
            {
                return make(RejectCode::invalid_role_tag, "role_uid",
                            std::string{"role_uid tag '"} + std::string{tag} +
                                "' not allowed on " + std::string{msg_type} +
                                " (allowed: " + format_allowed(row.allowed) +
                                ") per HEP-CORE-0033 §G2.2.0b.8");
            }
            return std::nullopt;
        }
    }

    // Universal-set path: prod|cons|proc all admitted.  Unknown tag
    // reaches this branch only if grammar_ok accepted it, so treat as
    // invalid_request (grammar drift) rather than tag-policy failure.
    if (tag != kTagProd && tag != kTagCons && tag != kTagProc)
    {
        return make(RejectCode::invalid_request, "role_uid",
                    std::string{"role_uid tag '"} + std::string{tag} +
                        "' unrecognized (expected prod|cons|proc)");
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
    // gate_supported_proto retired per C3 — wire-version + ABI via
    // `abi_fingerprint` per HEP-CORE-0032 §8, not this gate.
    if (auto r = gate_identity_match(env, body))          return r;
    if (auto r = gate_grammar(body))                       return r;
    // Per-msg-type role-tag policy (HEP-CORE-0033 §G2.2.0b.8).  role_type
    // field is unused for REG_REQ / CONSUMER_REG_REQ / DEREG_REQ /
    // CONSUMER_DEREG_REQ (only HEARTBEAT_NOTIFY reads it).
    if (auto r = gate_role_tag_policy(env.msg_type(), body.role_uid, {}))
        return r;
    if (auto r = gate_known_role_binding(body, ctx))       return r;
    if (auto r = gate_key_rotation(body, ctx))             return r;
    if (auto r = gate_replay_bound(body, ctx))             return r;
    return std::nullopt;  // Admitted; caller runs topology/cardinality/schema.
}

// ── Authenticated-REG-family gate runner ──────────────────────────────
//
// Applied to DEREG_REQ, CONSUMER_DEREG_REQ, ENDPOINT_UPDATE_REQ,
// CHANNEL_AUTH_APPLIED_REQ — bodies that DON'T carry `zmq_pubkey` or
// `broker_proto` (those were bound to the role at REG_REQ time).  Runs
// identity + universal grammar + replay.

std::optional<RejectDetail>
run_authenticated_reg_family_gates(
    const ::pylabhub::wire::WireEnvelope   &env,
    const AuthenticatedRegFamilyView       &body,
    const AdmissionContext                 &ctx) noexcept
{
    // Identity match — envelope's Frame 0 must equal payload role_uid.
    if (env.identity() != body.role_uid)
    {
        return make(RejectCode::identity_mismatch, "role_uid",
                    "ROUTER identity does not equal payload role_uid "
                    "(I-DEALER-IDENTITY)");
    }

    // Universal grammar on role_uid + channel_name (both mandatory on
    // every REG-family body).  No zmq_pubkey check here — the body
    // doesn't carry one for this tier.
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

    // Per-msg-type role-tag policy — applies to DEREG_REQ /
    // CONSUMER_DEREG_REQ / ENDPOINT_UPDATE_REQ / CHANNEL_AUTH_APPLIED_REQ
    // just as it does to REG_REQ.  No role_type field on these bodies
    // (universal set for non-HEARTBEAT msg_types).
    if (auto r = gate_role_tag_policy(env.msg_type(), body.role_uid, {}))
        return r;

    // Replay-bound — I-REPLAY-BOUND (nonce dedup + wall-clock skew).
    // Shared with the REG_REQ path via `check_replay_bound`
    // (de-duplicated 2026-07-17; was a verbatim inline copy).
    return check_replay_bound(body.role_uid, body.client_nonce,
                              body.client_wall_ts, ctx);
}

// ── Control-tier gate runner ──────────────────────────────────────────
//
// Applied to non-mutating control REQs (HEARTBEAT_REQ, GET_CHANNEL_AUTH_REQ,
// DISC_REQ, etc.).  Only enforces identity match + grammar when the body
// carries role_uid / channel_name.  Bodies without role_uid (DISC_REQ,
// CHANNEL_LIST_REQ) skip identity — envelope_hash from parse is the
// only assurance for those.
std::optional<RejectDetail>
run_control_gates(const ::pylabhub::wire::WireEnvelope &env,
                   const ControlBodyView                &body,
                   const AdmissionContext               &/*ctx*/) noexcept
{
    if (!body.role_uid.empty())
    {
        if (env.identity() != body.role_uid)
        {
            // Include both values in the message: the mismatch is
            // silent-fatal (client sees timeout / ERROR envelope with
            // no way to see the two strings) and this is often the
            // symptom of a routing_id ≠ role_uid config drift, which
            // is only diagnosable by seeing both.
            return make(RejectCode::identity_mismatch, "role_uid",
                        std::string{"ROUTER identity '"} +
                            std::string(env.identity()) +
                            "' does not equal payload role_uid '" +
                            std::string(body.role_uid) +
                            "' (I-DEALER-IDENTITY)");
        }
        if (!grammar_ok(body.role_uid))
        {
            return make(RejectCode::invalid_request, "role_uid",
                        "role_uid fails identifier grammar "
                        "(HEP-CORE-0033 §G2.2.0b)");
        }
        // Per-msg-type role-tag policy — HEARTBEAT_NOTIFY reads
        // body.role_type to derive its allowed tag; other control
        // msg_types fall through to the universal set.
        if (auto r = gate_role_tag_policy(env.msg_type(), body.role_uid,
                                            body.role_type))
            return r;
    }
    if (!body.channel_name.empty() && !grammar_ok(body.channel_name))
    {
        return make(RejectCode::invalid_request, "channel_name",
                    "channel_name fails identifier grammar "
                    "(HEP-CORE-0033 §G2.2.0b)");
    }
    return std::nullopt;
}

}  // namespace pylabhub::admission
