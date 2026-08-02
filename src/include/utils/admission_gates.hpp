#pragma once
/**
 * @file admission_gates.hpp
 * @brief Reusable admission-gate pipeline for pylabhub REG-family messages.
 *
 * Implements the §14.5 gate sequence from HEP-CORE-0046.
 * Every REG-family handler runs the same gate sequence BEFORE any state
 * mutation; this file owns the single implementation.  No handler
 * re-implements a gate.
 *
 * State machine.  Gate numbers match the HEP-CORE-0046 §5 flowchart RFGG
 * box (which describes this same `run_reg_family_gates`), NOT the §14.5
 * ordered list — §14.5 numbers the full pipeline (parse / proto / … /
 * topology) and so uses different numbers.  Envelope↔body hash is validated
 * earlier at `WireEnvelope::parse` (I-ENVELOPE-BODY-BINDING), so it is a
 * pre-gate here, not a numbered gate.
 *
 *     ┌─────────────┐   parsed + envelope hash validated (WireEnvelope::parse)
 *     │  Received   │
 *     └──────┬──────┘
 *            ▼
 *     ┌─────────────┐   gate 1: env.identity() == body.role_uid()
 *     │ IdentityOk  │           else IDENTITY_MISMATCH
 *     └──────┬──────┘         (a consistency check the broker's reply
 *            │                 routing depends on — both values are
 *            │                 client-chosen, so it authenticates nothing;
 *            ▼                 gate 4 is what decides identity)
 *     ┌─────────────┐   gate 2: grammar (HEP-CORE-0033 §G2.2.0b)
 *     │ GrammarOk   │           else INVALID_REQUEST
 *     └──────┬──────┘
 *            ▼
 *     ┌─────────────┐   gate 3: role_tag_policy (per-msg-type tag set)
 *     │ RoleTagOk   │           else INVALID_ROLE_TAG
 *     └──────┬──────┘
 *            ▼
 *     ┌─────────────┐   gate 4: does this registration belong to the
 *     │  BoundOk    │           connection that carried it?  The claimed
 *     └──────┬──────┘           role_uid and announced zmq_pubkey are
 *            │                  checked against the key the connection
 *            │                  PROVED at handshake — not against the
 *            │                  roster alone, which a copied public key
 *            │                  would satisfy.
 *            │                  else UNAUTHENTICATED / UNKNOWN_ROLE /
 *            │                       PUBKEY_MISMATCH / WRONG_PEER_KIND /
 *            │                       IDENTITY_MISMATCH
 *            ▼                  (PUBKEY_MISMATCH also enforces I-KEY-
 *                               ROTATION-VIA-DEREG: rotation = edit config
 *                               + hard reload — HEP-CORE-0046)
 *     ┌─────────────┐   gate 5: nonce dedup + wall_ts skew
 *     │  ReplayOk   │           else REPLAY_OR_SKEW
 *     └──────┬──────┘
 *            ▼
 *     ┌─────────────┐   all pre-state-mutation gates passed;
 *     │  Admitted   │   caller runs protocol-level admission
 *     └─────────────┘   (topology / cardinality / schema)
 *
 * Any gate failure is a terminal state: `RejectDetail` is returned, caller
 * builds an ERROR envelope, no state mutation occurs.
 */

#include "pylabhub_utils_export.h"
#include "utils/security/pubkey_origin.hpp" // ClaimVerdict, AttestedKey

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pylabhub::wire
{
class WireEnvelope;
}

namespace pylabhub::admission
{

/// Named reject codes emitted by the pre-state-mutation gates.  Broker
/// echoes these as `error_code` on ERROR envelopes; clients switch on
/// them per HEP-CORE-0007 §12.4a taxonomy.
enum class RejectCode
{
    // Skeleton / typed body integrity (parse-time pre-gate — validated
    // inside WireEnvelope::parse, before the numbered gates)
    envelope_tampered,     ///< I-ENVELOPE-BODY-BINDING: hash mismatch
    body_schema_violation, ///< Typed body construction threw WireBodyError

    // Identity + binding (gates 1, 2, 4 — identity / grammar / known-role
    // binding; role_tag is gate 3, grouped separately below.  uid_conflict is
    // raised later at state-mutation, not by a gate.)
    identity_mismatch, ///< Raised by two gates: I-DEALER-IDENTITY
                       ///< (env.identity != body.role_uid), and
                       ///< I-PUBKEY-BINDING when the claimed role_uid is not
                       ///< the subject the proven key belongs to.  The
                       ///< message distinguishes them.
    invalid_request,   ///< Wire-shape violation: grammar / unknown enum / etc.
    unknown_role,      ///< I-PUBKEY-BINDING: the key the connection proved is
                       ///< not one this hub recognises
    pubkey_mismatch,   ///< I-PUBKEY-BINDING: the announced zmq_pubkey is not
                       ///< the key the connection proved at handshake
    uid_conflict,      ///< uid already registered (duplicate REG)

    // Provenance (added 2026-07-29 with the attested-identity gate).  These
    // two exist because the alternatives were factually wrong: a connection
    // that produced no proof is not an `unknown_role` (nothing was looked
    // up), and a federation peer's key IS known — reporting either as
    // `unknown_role` sends an operator to investigate a role that is
    // configured perfectly well.  A denial has to name what actually
    // happened or it costs more than it saves.
    unauthenticated, ///< The connection produced no proof of identity: no
                     ///< enforced handshake, so there is nothing to check a
                     ///< claim against.  Registration requires proof.
    wrong_peer_kind, ///< The attested key belongs to a federation PEER HUB,
                     ///< not a local role.  A peer may carry identities
                     ///< other than its own, but only under the delegation
                     ///< modes of HEP-CORE-0035 §4.3 — which are not built,
                     ///< so it is refused on the registration plane rather
                     ///< than silently permitted.

    // Anti-replay (gate 5)
    replay_or_skew, ///< I-REPLAY-BOUND: nonce reuse or wall_ts skew

    // Per-msg-type role-tag policy (HEP-CORE-0033 §G2.2.0b.8 table)
    invalid_role_tag, ///< role_uid tag not in the allowed set for this
                      ///< msg_type (e.g. a `cons.*` uid on REG_REQ).

    // Server-side conditions (NOT client wire violations)
    broker_internal_error, ///< Broker misconfiguration or unimplemented path;
                           ///< reflects a bug in the broker, not the client
};

/// Human-readable name of the reject code, suitable for `error_code`
/// wire field.  Stable across the protocol lifetime.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string_view to_wire_string(RejectCode code) noexcept;

/// Terminal-rejection outcome from a gate.  `code` is machine-readable
/// (goes into `error_code` on the ERROR envelope).  `message` is
/// human-readable and identifies the offending field / value where
/// helpful.  `field` optionally names the specific field that
/// triggered — used by structured operator logs.
struct PYLABHUB_UTILS_EXPORT RejectDetail
{
    RejectCode code;
    std::string field;   ///< optional; empty if not field-specific
    std::string message; ///< always populated

    [[nodiscard]] std::string_view code_wire() const noexcept { return to_wire_string(code); }
};

/// Callbacks the gates invoke against broker state.  Handler binds these
/// once at pipeline construction; gates run against them.  Keeps
/// admission_gates decoupled from HubState / BrokerServiceImpl surface
/// so gates are unit-testable in isolation from broker state.
struct AdmissionCallbacks
{
    /// Decide whether a registration claim belongs to the connection it
    /// arrived on (HEP-CORE-0035 §4.2).
    ///
    /// @param attested what the transport proved about this connection —
    ///        `nullopt` if no enforced handshake produced anything.
    /// @param role_uid the identity the body CLAIMS.
    /// @param zmq_pubkey the key the body ANNOUNCES.  Nothing trusts it;
    ///        it is a declaration cross-checked against @p attested, and
    ///        every check over it can only deny.
    ///
    /// Returns a verdict, never the roster.  The broker binds this to its
    /// published authority snapshot; the gate learns the outcome and has
    /// no way to enumerate or inspect who is recognised.  The
    /// pubkey-disagreement verdict is also what enforces
    /// I-KEY-ROTATION-VIA-DEREG (an on-the-fly re-REG under a rotated
    /// key) — there is no separate key-rotation gate.
    std::function<::pylabhub::utils::security::ClaimVerdict(
        const std::optional<::pylabhub::utils::security::AttestedKey> &attested,
        std::string_view role_uid, std::string_view zmq_pubkey)>
        check_registration;

    /// Does the connection's proven key own @p role_uid?
    ///
    /// The post-registration form of the question above, for bodies that
    /// carry no `zmq_pubkey` (DEREG / ENDPOINT_UPDATE / CHANNEL_AUTH_
    /// APPLIED — the key was bound to the role at REG time).  Bound to the
    /// same authority snapshot and returns the same verdicts, so a caller
    /// cannot get a different answer to the same question by asking a
    /// different way.
    std::function<::pylabhub::utils::security::ClaimVerdict(
        const std::optional<::pylabhub::utils::security::AttestedKey> &attested,
        std::string_view role_uid)>
        check_role_ownership;

    /// Record the nonce for anti-replay dedup.  Returns true if the
    /// nonce is fresh (accepted) or false if it collided within the
    /// sliding window.  The underlying `ReplayGuard` prunes entries older
    /// than `ctx.nonce_window_ms` against its OWN trusted monotonic clock;
    /// there is deliberately NO timestamp argument, so the client stamp
    /// can never be wired into the dedup window (see ReplayGuard header).
    std::function<bool(std::string_view role_uid, std::string_view client_nonce)>
        record_and_check_nonce;

    /// Return the broker's wall-clock time in milliseconds since epoch.
    /// Injected for testability — tests substitute a fixed value; broker
    /// binds std::chrono::system_clock::now().
    std::function<std::uint64_t()> wall_now_ms;
};

/// Ambient context shared across all gate calls in one admission.  Owned
/// by the handler for the duration of one REG-family REQ; passed to
/// every gate by reference.
///
/// All fields have safe defaults so a caller that forgets to populate
/// still exhibits observable behavior (e.g. non-zero skew tolerance)
/// rather than the silent "0 tolerance rejects everything" trap.  The
/// `cb` pointer is intentionally left nullptr so the nullptr-defense
/// path in the gates surfaces the misconfiguration with a
/// `broker_internal_error`.
struct AdmissionContext
{
    const AdmissionCallbacks *cb{nullptr}; ///< non-owning
    std::uint64_t skew_tolerance_ms{30'000ULL};
    /// I-REPLAY-BOUND soundness: MUST be >= 2 * skew_tolerance_ms.  Dedup is
    /// pruned against the TRUSTED broker clock (record_and_check_nonce gets
    /// wall_now_ms(), not the client stamp), so an attacker cannot force
    /// early eviction — but a replay stays skew-acceptable for up to 2*skew
    /// after the original (the tolerance applies to both the original
    /// acceptance and the replay), so the nonce must be remembered that long
    /// or a late-but-skew-valid replay finds its nonce pruned and is wrongly
    /// admitted.  Default = 2 * skew.
    std::uint64_t nonce_window_ms{60'000ULL};
    // Note: no `broker_proto` field.  C3 resolution retired the
    // scalar-`broker_proto` gate for REG-family REQs; wire-version +
    // ABI compatibility is verified via `abi_fingerprint` per
    // HEP-CORE-0032 §8 in the broker's REG handler, not in the shared
    // admission pipeline.
};

// ── Bundled per-gate call signature ────────────────────────────────────
//
// Every gate below takes (envelope, body_view, ctx) and returns
// std::optional<RejectDetail> — nullopt = passed, else terminal reject.
// Body access uses a lightweight typed view so the gates don't need to
// know which specific body class (RegReqBody vs. EndpointUpdateReqBody
// etc.) is present — they only need the fields common to REG-family
// messages.

/// Read-only view over the fields REG-family bodies share.  Populated
/// by the caller from the typed body class before gate invocation.
/// Keeps gates decoupled from body class layout.
///
/// Populated by REG-family body classes that carry the security triple
/// per I-REPLAY-BOUND (RegReqBody, EndpointUpdateReqBody,
/// ChannelAuthAppliedReqBody, DeregReqBody, CONSUMER_DEREG_REQ body).
/// Non-REG-family control messages (HEARTBEAT_REQ / GET_CHANNEL_AUTH_REQ
/// / DISC_REQ per addendum §14.3) do NOT carry the security triple and
/// therefore cannot populate this view — they go through a lighter
/// gate set that skips the anti-replay gate (local gate 5).
///
/// Includes `channel_name` because every REG-family body carries it —
/// the shared view is the natural home for the universal grammar check
/// §14.5 gate 4 requires.  Msg-type-specific fields (role_name on
/// REG_REQ, applied_version on APPLIED_REQ, etc.) live on the typed
/// body class and are checked by the commit callback, not by the
/// shared pre-mutation gates.
struct RegFamilyBodyView
{
    std::string_view role_uid;
    std::string_view channel_name;
    std::string_view zmq_pubkey;
    std::string_view client_nonce;
    std::uint64_t client_wall_ts;
};

// ── Individual gates ──────────────────────────────────────────────────
//
// Public for unit testing.  Handlers should prefer `run_reg_family_gates`
// which runs all in order.
//
// C3 resolution 2026-07-14: `gate_supported_proto` (scalar
// `broker_proto` equality check) is retired.  Wire-version + ABI
// compatibility for REG-family REQs is verified through
// `abi_fingerprint` per HEP-CORE-0032 §8, using
// `verify_peer_versions()` at the broker's REG handler (not at
// the shared admission pipeline).  The removed gate never fired
// against real production clients (they never stamped the scalar
// field).  See DRAFT_reg_wire_alignment_cleanup_2026-07-13.md §10
// C3.

/// I-DEALER-IDENTITY: the ROUTER-captured routing id must equal the
/// body's `role_uid`.
///
/// This proves NOTHING about authentication — both values are chosen by
/// the client — and the name it used to carry (`gate_identity_match`)
/// invited reading it as an identity check.  It is a consistency
/// requirement the broker depends on mechanically: replies are routed on
/// the routing id, and it is mixed into `envelope_hash`.  Authentication
/// is `gate_attested_binding` below.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_dealer_identity_consistency(const ::pylabhub::wire::WireEnvelope &env,
                                 const RegFamilyBodyView &body) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_grammar(const RegFamilyBodyView &body) noexcept;

/// I-PUBKEY-BINDING: the registration must belong to the connection that
/// carried it (HEP-CORE-0035 §4.2).
///
/// Takes the envelope because the deciding fact rides it: the key this
/// connection PROVED at handshake.  Comparing the body's claimed
/// `role_uid` and announced `zmq_pubkey` against the roster alone admits
/// anyone holding a copy of someone else's published key — proof of
/// possession is what separates the two, and only the envelope carries it.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_attested_binding(const ::pylabhub::wire::WireEnvelope &env, const RegFamilyBodyView &body,
                      const AdmissionContext &ctx) noexcept;

/// I-PUBKEY-BINDING for messages that act on an ALREADY-registered role:
/// the connection must own @p role_uid (HEP-CORE-0035 §4.2).
///
/// Same question as `gate_attested_binding` minus the declared-key
/// cross-check, because these bodies carry no `zmq_pubkey` — the key was
/// bound to the role at REG time.
///
/// Do not mistake `gate_dealer_identity_consistency` for this.  That gate
/// compares two values the client chose; this one compares against what
/// the client PROVED.  A peer that sets both its routing id and its body
/// `role_uid` to a victim's uid satisfies the former and is refused here.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_attested_role_ownership(const ::pylabhub::wire::WireEnvelope &env, std::string_view role_uid,
                             const AdmissionContext &ctx) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_replay_bound(const RegFamilyBodyView &body, const AdmissionContext &ctx) noexcept;

/// HEP-CORE-0033 §G2.2.0b.8 per-msg-type role-tag policy.  Rejects with
/// `invalid_role_tag` when the leading tag embedded in @p role_uid
/// (`prod`/`cons`/`proc`) is not permitted for @p msg_type.  When
/// `msg_type == "HEARTBEAT_NOTIFY"`, the allowed set is derived from
/// @p role_type_field on the body (a HEARTBEAT declaring
/// `role_type="producer"` must carry a `prod.*` uid, and so on).
/// Precondition: role_uid grammar already validated by `gate_grammar`
/// or the equivalent universal-grammar check.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_role_tag_policy(std::string_view msg_type, std::string_view role_uid,
                     std::string_view role_type_field) noexcept;

// ── Authenticated-REG-family view + gate runner ───────────────────────
//
// REG-family messages OTHER than REG_REQ / CONSUMER_REG_REQ (i.e., all
// admission-mutating REQs that arrive AFTER initial registration:
// DEREG_REQ, CONSUMER_DEREG_REQ, ENDPOINT_UPDATE_REQ,
// CHANNEL_AUTH_APPLIED_REQ) do NOT carry `zmq_pubkey` in their body
// — the pubkey was bound to the role at REG time.  They carry the
// security triple (client_nonce + client_wall_ts) and identity, so the
// applicable gates are:
//
//   - dealer_identity_consistency: env.identity() == body.role_uid
//   - grammar (universal fields only): role_uid + channel_name
//   - role_tag_policy (HEP-CORE-0033 §G2.2.0b.8)
//   - attested_role_ownership (I-PUBKEY-BINDING): the connection's PROVEN
//     key must own role_uid
//   - replay_bound (I-REPLAY-BOUND): nonce dedup + wall_ts skew
//
// `gate_attested_binding` itself does not apply — there is no announced
// `zmq_pubkey` on these bodies to cross-check, because the key was bound
// to the role at REG time.  That is NOT a reason to skip the ownership
// half: "the key was already established" says which key belongs to the
// role, not that THIS connection is holding it.  Establishing the binding
// once at REG_REQ says nothing about who sends the DEREG_REQ afterwards.
struct AuthenticatedRegFamilyView
{
    std::string_view role_uid;
    std::string_view channel_name;
    std::string_view client_nonce;
    std::uint64_t client_wall_ts;
};

/// Runs identity + universal grammar + replay for non-REG_REQ REG-family
/// msg_types.  Same short-circuit semantics as `run_reg_family_gates`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
run_authenticated_reg_family_gates(const ::pylabhub::wire::WireEnvelope &env,
                                   const AuthenticatedRegFamilyView &body,
                                   const AdmissionContext &ctx) noexcept;

// ── Control-tier view + gate runner ───────────────────────────────────
//
// Non-mutating control REQs (HEARTBEAT_REQ, GET_CHANNEL_AUTH_REQ,
// CHECK_PEER_READY_REQ, DISC_REQ, BAND_*_REQ, ROLE_*_REQ, etc.) don't
// mutate admission state so I-REPLAY-BOUND doesn't require nonce dedup.
// The only universal check is I-DEALER-IDENTITY when the body carries
// `role_uid` — the identity claim must match the socket identity.
//
// Bodies without role_uid (DISC_REQ, CHANNEL_LIST_REQ) get envelope-only
// enforcement — identity check is skipped when role_uid is empty.
struct ControlBodyView
{
    std::string_view role_uid;     ///< empty if the body doesn't carry it
    std::string_view channel_name; ///< empty if the body doesn't carry it
    std::string_view role_type;    ///< "producer"|"consumer"|"processor";
                                   ///< populated by HEARTBEAT_NOTIFY per
                                   ///< HEP-0033 §G2.2.0b.8 (tag derived
                                   ///< from this field for that msg_type).
                                   ///< Empty for other control msg_types.
};

/// Runs identity_match if `role_uid` non-empty; grammar on role_uid /
/// channel_name if non-empty; no replay check.  Returns nullopt if all
/// checks pass (or no checks applied).
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
run_control_gates(const ::pylabhub::wire::WireEnvelope &env, const ControlBodyView &body,
                  const AdmissionContext &ctx) noexcept;

// ── Pipeline runner (REG_REQ / CONSUMER_REG_REQ) ──────────────────────

/// Runs gates 1-5 (identity, grammar, role_tag, known_role binding, replay;
/// the envelope hash pre-gate already ran inside WireEnvelope::parse) in
/// the §14.5 order.  Returns std::nullopt on all-passed, or the first
/// failing RejectDetail.  Short-circuit semantics: the first failing
/// gate stops the sequence; downstream gates do not run (avoids
/// double-logging + wasted work).
///
/// Handlers use this once per REG_REQ / CONSUMER_REG_REQ (bodies that
/// carry zmq_pubkey) before any state mutation.  Other REG-family
/// msg_types use `run_authenticated_reg_family_gates`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
run_reg_family_gates(const ::pylabhub::wire::WireEnvelope &env, const RegFamilyBodyView &body,
                     const AdmissionContext &ctx) noexcept;

} // namespace pylabhub::admission
