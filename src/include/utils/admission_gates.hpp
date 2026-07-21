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
 *     └──────┬──────┘
 *            ▼
 *     ┌─────────────┐   gate 2: grammar (HEP-CORE-0033 §G2.2.0b)
 *     │ GrammarOk   │           else INVALID_REQUEST
 *     └──────┬──────┘
 *            ▼
 *     ┌─────────────┐   gate 3: role_tag_policy (per-msg-type tag set)
 *     │ RoleTagOk   │           else INVALID_ROLE_TAG
 *     └──────┬──────┘
 *            ▼
 *     ┌─────────────┐   gate 4: verify_known_role_binding(role_uid, zmq_pubkey)
 *     │  BoundOk    │           else UNKNOWN_ROLE / PUBKEY_MISMATCH
 *     └──────┬──────┘         (PUBKEY_MISMATCH also enforces I-KEY-ROTATION-
 *            │                 VIA-DEREG: a running hub only accepts a role's
 *            │                 pinned known_roles pubkey; rotation = edit
 *            ▼                 config + hard reload — HEP-CORE-0046)
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
    envelope_tampered,   ///< I-ENVELOPE-BODY-BINDING: hash mismatch
    body_schema_violation,  ///< Typed body construction threw WireBodyError

    // Identity + binding (gates 1, 2, 4 — identity / grammar / known-role
    // binding; role_tag is gate 3, grouped separately below.  uid_conflict is
    // raised later at state-mutation, not by a gate.)
    identity_mismatch,   ///< I-DEALER-IDENTITY: env.identity != body.role_uid
    invalid_request,     ///< Wire-shape violation: grammar / unknown enum / etc.
    unknown_role,        ///< I-PUBKEY-BINDING: (uid, pubkey) not in known_roles
    pubkey_mismatch,     ///< I-PUBKEY-BINDING: uid known, pubkey does not match
    uid_conflict,        ///< uid already registered (duplicate REG)

    // Anti-replay (gate 5)
    replay_or_skew,      ///< I-REPLAY-BOUND: nonce reuse or wall_ts skew

    // Per-msg-type role-tag policy (HEP-CORE-0033 §G2.2.0b.8 table)
    invalid_role_tag,    ///< role_uid tag not in the allowed set for this
                          ///< msg_type (e.g. a `cons.*` uid on REG_REQ).

    // Server-side conditions (NOT client wire violations)
    broker_internal_error,  ///< Broker misconfiguration or unimplemented path;
                            ///< reflects a bug in the broker, not the client
};

/// Human-readable name of the reject code, suitable for `error_code`
/// wire field.  Stable across the protocol lifetime.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::string_view
to_wire_string(RejectCode code) noexcept;

/// Terminal-rejection outcome from a gate.  `code` is machine-readable
/// (goes into `error_code` on the ERROR envelope).  `message` is
/// human-readable and identifies the offending field / value where
/// helpful.  `field` optionally names the specific field that
/// triggered — used by structured operator logs.
struct PYLABHUB_UTILS_EXPORT RejectDetail
{
    RejectCode  code;
    std::string field;    ///< optional; empty if not field-specific
    std::string message;  ///< always populated

    [[nodiscard]] std::string_view code_wire() const noexcept
    {
        return to_wire_string(code);
    }
};

/// Outcome of a known-roles lookup.  Distinguishes "role_uid absent" from
/// "role_uid present but pubkey differs" so gate 4 (known_role_binding)
/// can report the correct wire error code.  The pubkey_mismatch result
/// is also what enforces I-KEY-ROTATION-VIA-DEREG (an on-the-fly re-REG
/// with a rotated pubkey) — there is no separate key-rotation gate.
enum class KnownRoleLookup
{
    binding_matches,       ///< (uid, pubkey) matches known_roles exactly
    uid_unknown,           ///< role_uid not present in known_roles
    pubkey_mismatch,       ///< role_uid present, pubkey differs from known
};

/// Callbacks the gates invoke against broker state.  Handler binds these
/// once at pipeline construction; gates run against them.  Keeps
/// admission_gates decoupled from HubState / BrokerServiceImpl surface
/// so gates are unit-testable in isolation from broker state.
struct AdmissionCallbacks
{
    /// Look up (role_uid, zmq_pubkey) in known_roles.
    std::function<KnownRoleLookup(std::string_view role_uid,
                                    std::string_view zmq_pubkey)>
        lookup_known_role;

    /// Record the nonce for anti-replay dedup.  Returns true if the
    /// nonce is fresh (accepted) or false if it collided within the
    /// sliding window.  The underlying `ReplayGuard` prunes entries older
    /// than `ctx.nonce_window_ms` against its OWN trusted monotonic clock;
    /// there is deliberately NO timestamp argument, so the client stamp
    /// can never be wired into the dedup window (see ReplayGuard header).
    std::function<bool(std::string_view role_uid,
                        std::string_view client_nonce)>
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
    const AdmissionCallbacks *cb{nullptr};              ///< non-owning
    std::uint64_t             skew_tolerance_ms{30'000ULL};
    /// I-REPLAY-BOUND soundness: MUST be >= 2 * skew_tolerance_ms.  Dedup is
    /// pruned against the TRUSTED broker clock (record_and_check_nonce gets
    /// wall_now_ms(), not the client stamp), so an attacker cannot force
    /// early eviction — but a replay stays skew-acceptable for up to 2*skew
    /// after the original (the tolerance applies to both the original
    /// acceptance and the replay), so the nonce must be remembered that long
    /// or a late-but-skew-valid replay finds its nonce pruned and is wrongly
    /// admitted.  Default = 2 * skew.
    std::uint64_t             nonce_window_ms{60'000ULL};
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
    std::uint64_t    client_wall_ts;
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

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_identity_match(const ::pylabhub::wire::WireEnvelope &env,
                     const RegFamilyBodyView              &body) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_grammar(const RegFamilyBodyView &body) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_known_role_binding(const RegFamilyBodyView &body,
                         const AdmissionContext  &ctx) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_replay_bound(const RegFamilyBodyView &body,
                   const AdmissionContext  &ctx) noexcept;

/// HEP-CORE-0033 §G2.2.0b.8 per-msg-type role-tag policy.  Rejects with
/// `invalid_role_tag` when the leading tag embedded in @p role_uid
/// (`prod`/`cons`/`proc`) is not permitted for @p msg_type.  When
/// `msg_type == "HEARTBEAT_NOTIFY"`, the allowed set is derived from
/// @p role_type_field on the body (a HEARTBEAT declaring
/// `role_type="producer"` must carry a `prod.*` uid, and so on).
/// Precondition: role_uid grammar already validated by `gate_grammar`
/// or the equivalent universal-grammar check.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_role_tag_policy(std::string_view msg_type,
                       std::string_view role_uid,
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
//   - identity_match (I-DEALER-IDENTITY): env.identity() == body.role_uid
//   - grammar (universal fields only): role_uid + channel_name
//   - role_tag_policy (HEP-CORE-0033 §G2.2.0b.8)
//   - replay_bound (I-REPLAY-BOUND): nonce dedup + wall_ts skew
//
// gate_known_role_binding doesn't apply — the role's pubkey was
// already established by the successful REG_REQ that preceded this
// message.
struct AuthenticatedRegFamilyView
{
    std::string_view role_uid;
    std::string_view channel_name;
    std::string_view client_nonce;
    std::uint64_t    client_wall_ts;
};

/// Runs identity + universal grammar + replay for non-REG_REQ REG-family
/// msg_types.  Same short-circuit semantics as `run_reg_family_gates`.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
run_authenticated_reg_family_gates(const ::pylabhub::wire::WireEnvelope &env,
                                     const AuthenticatedRegFamilyView    &body,
                                     const AdmissionContext              &ctx) noexcept;

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
    std::string_view role_uid;      ///< empty if the body doesn't carry it
    std::string_view channel_name;  ///< empty if the body doesn't carry it
    std::string_view role_type;     ///< "producer"|"consumer"|"processor";
                                     ///< populated by HEARTBEAT_NOTIFY per
                                     ///< HEP-0033 §G2.2.0b.8 (tag derived
                                     ///< from this field for that msg_type).
                                     ///< Empty for other control msg_types.
};

/// Runs identity_match if `role_uid` non-empty; grammar on role_uid /
/// channel_name if non-empty; no replay check.  Returns nullopt if all
/// checks pass (or no checks applied).
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
run_control_gates(const ::pylabhub::wire::WireEnvelope &env,
                   const ControlBodyView                &body,
                   const AdmissionContext               &ctx) noexcept;

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
run_reg_family_gates(const ::pylabhub::wire::WireEnvelope &env,
                      const RegFamilyBodyView              &body,
                      const AdmissionContext               &ctx) noexcept;

}  // namespace pylabhub::admission
