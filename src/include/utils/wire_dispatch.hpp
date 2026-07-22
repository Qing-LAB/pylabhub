#pragma once
/**
 * @file wire_dispatch.hpp
 * @brief Unified receive-side entry for pylabhub control-plane messages.
 *
 * Composes the four existing building blocks:
 *   - `wire_envelope`  — 5-frame parse + envelope_hash validation
 *   - `wire_bodies`    — typed body class construction
 *   - `admission_gates`— identity / grammar / known_roles / rotation / replay
 *   - `reg_admission_pipeline` — state-mutation commit + typed outcome
 *
 * into a single call: `receive_and_validate(raw, ctx)` → `ReceivedMessage`.
 *
 * The caller (broker's ROUTER poll loop; BRC's recv path) invokes one
 * function per inbound message and receives a std::variant whose active
 * alternative encodes:
 *   - success (per-msg_type Validated<Body>, guaranteed to have passed all
 *     admission gates appropriate for that msg_type), OR
 *   - failure (`RejectedMessage` with enough info to build the ERROR reply).
 *
 * Handlers downstream of this call never touch raw JSON; never re-derive
 * identity from envelopes; never re-run known-role / replay / rotation
 * checks.  They see a validated typed body and go straight to the domain
 * logic (HubState mutation, wire response body construction).
 *
 * ⚠ STATUS (2026-07-16): this receive-side validation IS live in the broker
 *   — gates run on every message.  BUT the broker's `dispatch_received`
 *   currently down-converts the REG-family Validated* arms BACK to raw JSON
 *   (`to_legacy`) and runs the handcrafted `handle_reg_req` /
 *   `handle_consumer_reg_req`, rather than feeding the typed body to the
 *   admission pipeline (BrokerRegHandler).  Completing that swap — so REG
 *   handlers consume the typed body directly and the JSON handlers are
 *   deleted — is task #57 (HEP-0046 Phase B).  The `to_legacy` bridge is the
 *   transition scaffold, not a permanent layer.
 *
 * ➜ FIRST TYPED PATHWAY (2026-07-19): the admin operator console
 *   (HEP-CORE-0033 §11) is being built natively on this typed envelope from
 *   the start — greenfield admin msg_types + `wire_bodies` bodies, no JSON
 *   `{method,token,params}` REP surface, no `to_legacy` bridge.  It is the
 *   reference implementation of an end-to-end typed path that the #57 broker
 *   REG migration follows: admin proves the receive→typed-body→handler flow
 *   with zero down-conversion.  See HEP-CORE-0033 §11.1.
 *
 * By construction, no code path can reach a handler without first passing
 * every admission gate the msg_type's tier requires.  Adding a new msg_type
 * is one row in the dispatch table + one Validated<Body> variant.
 *
 * Tier definitions (mapped per msg_type in the dispatch table):
 *
 *   Tier `RegFamily`: envelope + body class + identity + grammar +
 *     known_role + key_rotation + replay.  Applied to msg_types that
 *     mutate admission state (REG_REQ / CONSUMER_REG_REQ / DEREG_REQ /
 *     CONSUMER_DEREG_REQ / ENDPOINT_UPDATE_REQ / CHANNEL_AUTH_APPLIED_REQ).
 *
 *   Tier `Control`: envelope + body class + identity match (only when the
 *     body carries role_uid).  Applied to non-mutating REQs
 *     (HEARTBEAT_REQ, GET_CHANNEL_AUTH_REQ, CHECK_PEER_READY_REQ, etc.).
 *     No replay check — repeat is allowed by design (no admission state
 *     changes on a re-query).
 *
 *   Tier `EnvelopeOnly`: envelope only (body kept as raw JSON).  Applied
 *     to msg_types that don't yet have a typed body class but still need
 *     I-ENVELOPE-BODY-BINDING enforcement.  Handlers reading these bodies
 *     still use `body.value(...)` at their own risk; migrating each of
 *     these to a typed body class is a follow-on that doesn't change the
 *     dispatch surface.
 */

#include "pylabhub_utils_export.h"
#include "utils/admission_gates.hpp"
#include "utils/wire_bodies.hpp"
#include "utils/wire_envelope.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace zmq
{
class multipart_t;
}

namespace pylabhub::wire::dispatch
{

// ── Rejected outcome ─────────────────────────────────────────────────────
//
// Carries everything the broker needs to send an ERROR reply back to the
// sender without re-parsing anything.  `identity` is the ROUTER-captured
// Frame 0 (== sender's role_uid per I-DEALER-IDENTITY, when set); may be
// empty if parse failed before identity was recovered (in which case the
// caller drops silently — no reliable destination for the reply).
struct PYLABHUB_UTILS_EXPORT RejectedMessage
{
    /// Sender's DEALER routing_id from Frame 0 (envelope parse) or empty
    /// if parse failed before Frame 0 was extracted.
    std::string identity;
    /// Sender's correlation_id from Frame 3, empty if unavailable.
    std::string correlation_id;
    /// msg_type from Frame 2, empty if parse failed before it was extracted.
    std::string msg_type;
    /// Machine-readable reject code (mapped to `error_code` wire field).
    admission::RejectCode code;
    /// Which field triggered the reject (optional; empty if not field-specific).
    std::string field;
    /// Human-readable message for logs + client diagnostics.
    std::string message;
};

// ── Validated per-msg_type variants ──────────────────────────────────────
//
// Each Validated<Body> carries:
//   - the parsed envelope (identity, msg_type, correlation_id accessors)
//   - the typed body class (msg_type-specific named accessors)
//   - a `correlation_id()` helper mirror of env.correlation_id() as std::string
//     (broker builds ACK bodies with the echoed corr_id from the same value)
//
// Move-only (envelope + body are both move-only).  Constructed inside
// receive_and_validate; consumed by broker handlers via std::visit.

#define PLH_WIRE_VALIDATED(Name, BodyClass)                                                        \
    struct PYLABHUB_UTILS_EXPORT Name                                                              \
    {                                                                                              \
        ::pylabhub::wire::WireEnvelope env;                                                        \
        BodyClass body;                                                                            \
        Name(::pylabhub::wire::WireEnvelope e, BodyClass b) noexcept                               \
            : env(std::move(e)), body(std::move(b))                                                \
        {                                                                                          \
        }                                                                                          \
        Name(Name &&) noexcept = default;                                                          \
        Name &operator=(Name &&) noexcept = default;                                               \
        Name(const Name &) = delete;                                                               \
        Name &operator=(const Name &) = delete;                                                    \
        [[nodiscard]] std::string identity() const { return std::string(env.identity()); }         \
        [[nodiscard]] std::string correlation_id() const                                           \
        {                                                                                          \
            return std::string(env.correlation_id());                                              \
        }                                                                                          \
    }

// REG-family: full admission (identity + grammar + known_role + rotation
//             + replay applied by receive_and_validate).  Producer and
//             consumer use distinct body classes per HEP-CORE-0034 §10.2
//             (`expected_schema_*` prefix on consumer side).
PLH_WIRE_VALIDATED(ValidatedRegReq, ::pylabhub::wire::ProducerRegReqBody);
PLH_WIRE_VALIDATED(ValidatedConsumerRegReq, ::pylabhub::wire::ConsumerRegReqBody);
PLH_WIRE_VALIDATED(ValidatedDeregReq, ::pylabhub::wire::DeregReqBody);
PLH_WIRE_VALIDATED(ValidatedConsumerDeregReq, ::pylabhub::wire::DeregReqBody);
PLH_WIRE_VALIDATED(ValidatedEndpointUpdateReq, ::pylabhub::wire::EndpointUpdateReqBody);
PLH_WIRE_VALIDATED(ValidatedChannelAuthAppliedReq, ::pylabhub::wire::ChannelAuthAppliedReqBody);

// Control (identity-only where role_uid is present).  HEARTBEAT is
// fire-and-forget per C13 rename (msg_type = `HEARTBEAT_NOTIFY`);
// its body class is `HeartbeatNotifyBody` in the NOTIFY section.
PLH_WIRE_VALIDATED(ValidatedHeartbeatNotify, ::pylabhub::wire::HeartbeatNotifyBody);
PLH_WIRE_VALIDATED(ValidatedGetChannelAuthReq, ::pylabhub::wire::GetChannelAuthReqBody);
PLH_WIRE_VALIDATED(ValidatedDiscReq, ::pylabhub::wire::DiscReqBody);

#undef PLH_WIRE_VALIDATED

// ── EnvelopeOnly fallback ────────────────────────────────────────────────
//
// For msg_types that don't yet have a typed body class (BAND_*_REQ,
// ROLE_*_REQ, METRICS_REQ, SCHEMA_REQ, CHANNEL_LIST_REQ,
// CHANNEL_BROADCAST_SEND_NOTIFY, BAND_BROADCAST_SEND_NOTIFY,
// SHM_BLOCK_QUERY_REQ, CHECK_PEER_READY_REQ, legacy
// CONSUMER_ATTACH_REQ_ZMQ/SHM).  Envelope hash IS validated.
// Body is raw JSON — handler reads via `body.value(...)` until a typed
// body class replaces this fallback per msg_type (independent commits).
//
// Also used for the receiver side of any UNKNOWN msg_type inbound so
// the broker can emit the UNKNOWN_MSG_TYPE ERROR reply with proper
// correlation_id echoed.
struct PYLABHUB_UTILS_EXPORT ValidatedRawControl
{
    ::pylabhub::wire::WireEnvelope env;
    ::nlohmann::json body;
    ValidatedRawControl(::pylabhub::wire::WireEnvelope e, ::nlohmann::json b) noexcept
        : env(std::move(e)), body(std::move(b))
    {
    }
    ValidatedRawControl(ValidatedRawControl &&) noexcept = default;
    ValidatedRawControl &operator=(ValidatedRawControl &&) noexcept = default;
    ValidatedRawControl(const ValidatedRawControl &) = delete;
    ValidatedRawControl &operator=(const ValidatedRawControl &) = delete;
    [[nodiscard]] std::string identity() const { return std::string(env.identity()); }
    [[nodiscard]] std::string correlation_id() const { return std::string(env.correlation_id()); }
    [[nodiscard]] std::string msg_type() const { return std::string(env.msg_type()); }
};

// ── The full variant ─────────────────────────────────────────────────────

using ReceivedMessage = std::variant<
    ValidatedRegReq, ValidatedConsumerRegReq, ValidatedDeregReq, ValidatedConsumerDeregReq,
    ValidatedEndpointUpdateReq, ValidatedChannelAuthAppliedReq, ValidatedHeartbeatNotify,
    ValidatedGetChannelAuthReq, ValidatedDiscReq, ValidatedRawControl, RejectedMessage>;

// ── Admission binder ─────────────────────────────────────────────────────
//
// Wires the admission callbacks (known_roles lookup, key-rotation check,
// nonce dedup, wall clock) against real broker state.  BrokerServiceImpl
// owns one as a member; wire_dispatch's `receive_and_validate` uses the
// exposed context.  Same bindings are shared with `BrokerRegHandler` so
// admission logic runs identically whether invoked by the dispatch entry
// or the legacy REG_REQ pipeline.
//
// Callbacks are erased into std::function so this header stays free of
// HubState include dependencies.  BrokerServiceImpl binds them in-line
// against `hub_state_->nonce_seen`, `known_roles.lookup_pubkey_for_uid`,
// etc.
struct PYLABHUB_UTILS_EXPORT AdmissionBinder
{
    ::pylabhub::admission::AdmissionCallbacks callbacks;
    ::pylabhub::admission::AdmissionContext context;

    /// Rebind the context's cb pointer to `&this->callbacks` after any
    /// modification.  Callers should invoke once after populating
    /// `callbacks` with all four std::function fields.
    void finalize() noexcept { context.cb = &callbacks; }
};

// ── Entry ────────────────────────────────────────────────────────────────
//
// Parses the ROUTER-side 5-frame envelope, chooses the tier for the
// msg_type from the built-in dispatch table, constructs the typed body
// class (or raw JSON for EnvelopeOnly tier), runs the tier's gates, and
// returns the variant.
//
// On success: the active alternative is one of the Validated* types.
// The caller can be confident the body has been validated per its tier.
//
// On failure: the active alternative is `RejectedMessage` with the
// specific code + field + message.  The caller sends an ERROR envelope
// (via a companion helper below).
//
// Precondition: `admission_ctx.cb` is bound to non-null callbacks by the
// caller (BrokerServiceImpl at construction).  Missing callbacks produce
// `broker_internal_error` rejects — the pipeline never silently skips a
// gate.
[[nodiscard]] PYLABHUB_UTILS_EXPORT ReceivedMessage receive_and_validate(
    ::zmq::multipart_t &&raw, const ::pylabhub::admission::AdmissionContext &admission_ctx);

// ── Dispatch table introspection (test-facing) ───────────────────────────
//
// Exposes the msg_type → tier mapping so unit tests can pin the table
// contents.  Regression-guards against a msg_type row being silently
// dropped from `kDispatchTable` (which loses envelope-hash gating for
// that msg_type on the receive side).  Also lets tests derive the
// complete msg_type set the broker knows about without hard-coding.

/// Look up the dispatch tier for @p msg_type by name.  Returns
/// std::nullopt if @p msg_type is not in the dispatch table (broker
/// replies UNKNOWN_MSG_TYPE for those).  Tier is returned as its enum
/// spelling ("RegReq", "AuthReg_Dereg", "Control_HeartbeatNotify",
/// "EnvelopeOnly", etc.) so tests can pin without depending on the
/// internal enum layout.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<std::string_view>
tier_for_msg_type(std::string_view msg_type) noexcept;

/// Number of rows in the dispatch table.  Used to pin the table size
/// so a silent add/drop trips the L1 test.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::size_t dispatch_table_size() noexcept;

// ── Reject reply helper ──────────────────────────────────────────────────
//
// Builds the ERROR envelope for a `RejectedMessage`.  Broker uses this
// to send a wire-conformant rejection without re-implementing the reject
// body shape at every callsite.  Result is a 5-frame ROUTER-side wire
// (identity + 'C' + "ERROR" + correlation_id + JSON body) ready to send.
[[nodiscard]] PYLABHUB_UTILS_EXPORT ::zmq::multipart_t
build_error_reply(const RejectedMessage &rej);

} // namespace pylabhub::wire::dispatch
