#pragma once
/**
 * @file admission_gates.hpp
 * @brief Reusable admission-gate pipeline for pylabhub REG-family messages.
 *
 * Implements the §14.5 gate sequence from HEP-CORE-0046.
 * Every REG-family handler runs the same 7 gates BEFORE any state mutation;
 * this file owns the single implementation.  No handler re-implements a gate.
 *
 * State machine:
 *
 *     ┌─────────────┐   parse succeeded, dispatch chose typed body
 *     │  Received   │
 *     └──────┬──────┘
 *            │
 *            ▼
 *     ┌─────────────┐   gate 1: envelope↔body hash (already validated at
 *     │  HashOk     │           WireEnvelope::parse; re-check-free)
 *     └──────┬──────┘
 *            │
 *            ▼
 *     ┌─────────────┐   gate 2: broker_proto == kBrokerProtoVersion
 *     │  ProtoOk    │           else UNSUPPORTED_PROTO
 *     └──────┬──────┘
 *            │
 *            ▼
 *     ┌─────────────┐   gate 3: env.identity() == body.role_uid()
 *     │ IdentityOk  │           else IDENTITY_MISMATCH
 *     └──────┬──────┘
 *            │
 *            ▼
 *     ┌─────────────┐   gate 4: grammar (HEP-CORE-0033 §G2.2.0b)
 *     │ GrammarOk   │           else INVALID_REQUEST
 *     └──────┬──────┘
 *            │
 *            ▼
 *     ┌─────────────┐   gate 5: verify_known_role_binding(role_uid, zmq_pubkey)
 *     │  BoundOk    │           else UNKNOWN_ROLE / PUBKEY_MISMATCH
 *     └──────┬──────┘
 *            │
 *            ▼
 *     ┌─────────────┐   gate 6: pubkey == currently-registered pubkey
 *     │ RotationOk  │           else KEY_ROTATION_REQUIRES_DEREG
 *     └──────┬──────┘
 *            │
 *            ▼
 *     ┌─────────────┐   gate 7: nonce dedup + wall_ts skew
 *     │  ReplayOk   │           else REPLAY_OR_SKEW
 *     └──────┬──────┘
 *            │
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
    // Skeleton / typed body integrity (gates 1-2)
    envelope_tampered,   ///< I-ENVELOPE-BODY-BINDING: hash mismatch
    unsupported_proto,   ///< I-WIRE-VERSION-ATOMIC: broker_proto mismatch
    body_schema_violation,  ///< Typed body construction threw WireBodyError

    // Identity + binding (gates 3-6)
    identity_mismatch,   ///< I-DEALER-IDENTITY: env.identity != body.role_uid
    invalid_request,     ///< Wire-shape violation: grammar / unknown enum / etc.
    unknown_role,        ///< I-PUBKEY-BINDING: (uid, pubkey) not in known_roles
    pubkey_mismatch,     ///< I-PUBKEY-BINDING: uid known, pubkey does not match
    uid_conflict,        ///< I-KEY-ROTATION: uid already registered w/ diff pubkey
    key_rotation_required,  ///< I-KEY-ROTATION-VIA-DEREG

    // Anti-replay (gate 7)
    replay_or_skew,      ///< I-REPLAY-BOUND: nonce reuse or wall_ts skew

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
/// "role_uid present but pubkey differs" so gate 5 and gate 6 can report
/// the correct wire error code.
enum class KnownRoleLookup
{
    binding_matches,       ///< (uid, pubkey) matches known_roles exactly
    uid_unknown,           ///< role_uid not present in known_roles
    pubkey_mismatch,       ///< role_uid present, pubkey differs from known
};

/// Outcome of the key-rotation check against currently-registered role
/// state.  Distinguishes "no registration yet" (rotation not applicable)
/// from "already registered with different pubkey" (rotation attempt).
enum class KeyRotationCheck
{
    not_yet_registered,    ///< No live registration — this REG is the first
    matches_current,       ///< Pubkey matches the currently-registered value
    rotation_attempted,    ///< Live registration exists with a different pubkey
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

    /// Compare (role_uid, zmq_pubkey) against the role's currently-
    /// registered pubkey (if any).
    std::function<KeyRotationCheck(std::string_view role_uid,
                                     std::string_view zmq_pubkey)>
        check_key_rotation;

    /// Record the nonce for anti-replay dedup.  Returns true if the
    /// nonce is fresh (accepted) or false if it collided within the
    /// sliding window.  Implementation prunes entries older than
    /// `ctx.nonce_window_ms`.
    std::function<bool(std::string_view role_uid,
                        std::string_view client_nonce,
                        std::uint64_t     client_wall_ts)>
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
    std::uint32_t             broker_proto{0};          ///< set by caller — 0 = any (test-only path)
    std::uint64_t             skew_tolerance_ms{30'000ULL};
    std::uint64_t             nonce_window_ms{10'000ULL};  ///< I-REPLAY-BOUND: 2 * pending_budget_ms
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
/// gate set that skips gate 7.
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
    std::uint32_t    broker_proto;
    std::string_view client_nonce;
    std::uint64_t    client_wall_ts;
};

// ── Individual gates ──────────────────────────────────────────────────
//
// Public for unit testing.  Handlers should prefer `run_reg_family_gates`
// which runs all seven in order.

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_supported_proto(const RegFamilyBodyView &body,
                      const AdmissionContext  &ctx) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_identity_match(const ::pylabhub::wire::WireEnvelope &env,
                     const RegFamilyBodyView              &body) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_grammar(const RegFamilyBodyView &body) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_known_role_binding(const RegFamilyBodyView &body,
                         const AdmissionContext  &ctx) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_key_rotation(const RegFamilyBodyView &body,
                   const AdmissionContext  &ctx) noexcept;

[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
gate_replay_bound(const RegFamilyBodyView &body,
                   const AdmissionContext  &ctx) noexcept;

// ── Pipeline runner ───────────────────────────────────────────────────

/// Runs gates 2-7 (gate 1 already ran inside WireEnvelope::parse) in
/// the §14.5 order.  Returns std::nullopt on all-passed, or the first
/// failing RejectDetail.  Short-circuit semantics: the first failing
/// gate stops the sequence; downstream gates do not run (avoids
/// double-logging + wasted work).
///
/// Handlers use this once per REG-family REQ before any state mutation.
[[nodiscard]] PYLABHUB_UTILS_EXPORT std::optional<RejectDetail>
run_reg_family_gates(const ::pylabhub::wire::WireEnvelope &env,
                      const RegFamilyBodyView              &body,
                      const AdmissionContext               &ctx) noexcept;

}  // namespace pylabhub::admission
