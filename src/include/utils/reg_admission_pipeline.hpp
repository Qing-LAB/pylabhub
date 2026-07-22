#pragma once
/**
 * @file reg_admission_pipeline.hpp
 * @brief REG-family admission pipeline — orchestrates gates → state mutation
 *        → typed outcome for producer / consumer REG_REQ.
 *
 * ⚠ STATUS (2026-07-16): the pre-mutation GATES this runs are LIVE in the
 *   broker (via wire::dispatch::receive_and_validate).  This pipeline's
 *   COMMIT half (run_reg_admission + BrokerRegHandler) is unit-tested but
 *   NOT the live REG path — the broker still commits via the handcrafted
 *   handle_reg_req / handle_consumer_reg_req.  `RegRequest` (below) also
 *   still drops several fields the live handlers use.  Migration + full
 *   parity list: task #57 (HEP-0046 Phase B).  Consumer REG has no pipeline
 *   path here yet (make_request/run_reg_admission take ProducerRegReqBody).
 *
 * Layer above `admission_gates.hpp`.  Adds:
 *   - Delegation to the state-mutation primitive (via typed callback)
 *   - Typed outcome variant (Accepted / Rejected / Pended)
 *
 * The pipeline does NOT touch ZMQ sockets, NOT fire NOTIFYs, and NOT
 * hold HubState locks directly — it delegates the state-mutation step
 * via a `RegCommitFn` callback.  Gate callbacks are supplied through
 * `AdmissionContext::cb`; commit is supplied as a separate function
 * argument.  The broker's `BrokerRegHandler` binds both against
 * HubState primitives; L2 tests bind them to in-process stubs.  Same
 * pipeline is exercised by both paths.
 *
 * State transitions for one REG-family REQ:
 *
 *   Received (envelope + typed body)
 *      │
 *      ▼
 *   Pre-mutation gates (§14.5, gates 2-7 — gate 1 ran at parse)
 *      │ ─── any gate fails ──→ Rejected (typed gate reject)
 *      ▼
 *   Commit callback:
 *     - Runs protocol-level admission (topology / cardinality / schema /
 *       transport) inside HubState under its writer lock
 *     - Returns one of: RegAccepted, RegRejected (protocol), RegPended
 *      │
 *      ▼
 *   Outcome variant returned to caller.  Caller (broker) pattern-matches
 *   and emits wire response + fires the recorded NOTIFY manifest.
 *
 * Each transition is a pure function of its inputs.  Protocol gates and
 * state mutation are NOT separate steps in the code — they happen
 * atomically inside HubState primitives (a single writer-lock scope per
 * §14.5 + I-STATE-MUTATION-ATOMIC).  The diagram above collapses them
 * into one "commit" step to match code shape.
 */

#include "pylabhub_utils_export.h"
#include "utils/admission_gates.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pylabhub::wire
{
class WireEnvelope;
class ProducerRegReqBody;
} // namespace pylabhub::wire

namespace pylabhub::admission
{

// ── Typed protocol-level intermediates ────────────────────────────────
//
// Extracted from ProducerRegReqBody before state mutation.  Distinct from the
// body-class accessors so the state-mutation callback receives a
// well-shaped input rather than a raw JSON reference.  This is the
// interface the caller (broker or test) binds to.

/// Which admission side this REG_REQ is on.  Kept explicit — under the
/// topology model, the same wire (REG_REQ) may put the sender on the
/// binding side (fan-out / one-to-one producer) or the dialing side
/// (fan-in producer).  The pipeline distinguishes so downstream state
/// mutation knows which admission primitive to run.
enum class AdmissionSide
{
    binding, ///< fan-out / one-to-one producer, or fan-in consumer
    dialing, ///< fan-in producer, or fan-out / one-to-one consumer
};

/// The subset of ProducerRegReqBody fields the state-mutation primitive needs.
/// Ordered and typed to match the HubState `_on_producer_added` /
/// `_on_consumer_joined` calling convention.
struct RegRequest
{
    std::string channel_name;
    std::string role_uid;
    std::string role_name;
    std::string role_type; ///< "producer" | "consumer" | "processor"
    std::string zmq_pubkey;
    std::string channel_topology; ///< "" defaults to "one-to-one" downstream
    std::string data_transport;   ///< "zmq" | "shm"
    // Schema invariants — passed through verbatim to _on_producer_added.
    std::string schema_hash;
    // schema_version retired per C2 — version rides inside schema_id
    // (`$name.v<N>`) per HEP-CORE-0033 §G2.2.0b.
    std::string schema_id;
    std::string schema_blds;
    std::string schema_owner;
};

// ── Outcome variants ──────────────────────────────────────────────────
//
// std::variant makes the caller pattern-match; no room for accidentally
// missing a case — the compiler enforces exhaustiveness.

struct PYLABHUB_UTILS_EXPORT RegAccepted
{
    RegRequest request;  ///< echoed for convenience
    AdmissionSide side;  ///< which side this REG landed on
    bool channel_opened; ///< first admission on this channel
    std::uint64_t assigned_instance_id;
    std::uint64_t channel_version; ///< post-mutation snapshot
    // NOTIFY manifest — pipeline records what NOTIFYs the caller should
    // fire, but does NOT fire them itself.  Each entry is
    // (msg_type, target_role_uid, phase, subject_role_uid, subject_role_type).
    // Caller iterates and calls broker's NOTIFY-emit primitive; keeps
    // pipeline decoupled from ZMQ.
    struct NotifyItem
    {
        std::string phase; ///< "admitted" | "live" | "left"
        std::string subject_role_uid;
        std::string subject_role_type; ///< "producer" | "consumer"
    };
    std::vector<NotifyItem> pending_notifies;
};

struct PYLABHUB_UTILS_EXPORT RegRejected
{
    RejectDetail detail;
};

struct PYLABHUB_UTILS_EXPORT RegPended
{
    /// Enum-like reason string; one of the §7.2 pending reasons:
    /// "awaiting_channel_created", "awaiting_binding_side_live",
    /// "awaiting_endpoint_resolved", "awaiting_allowlist_confirmed".
    std::string reason;
    /// R6-condition-specific version the pending entry is watching.
    /// Zero when reason is "awaiting_channel_created".
    std::uint64_t my_version{0};
};

using RegOutcome = std::variant<RegAccepted, RegRejected, RegPended>;

// ── State-mutation callback signature ─────────────────────────────────
//
// Binding-side and dialing-side REGs go through different HubState
// entry points; the pipeline delegates via a small typed callback so
// the state-mutation primitives can be substituted for testing.
//
// In production the broker binds this to a lambda that dispatches on
// `request.role_type` to HubState::_on_producer_added (binding-side
// producer or fan-in consumer) or HubState::_on_consumer_joined
// (fan-in consumer opens channel), plus the pending-queue primitive
// for dialing-side under R6 wait.
//
// The callback returns the same RegAccepted / RegPended payload the
// pipeline will forward to the caller — the pipeline's own contribution
// is running the pre-mutation gates and forwarding the typed request.

using RegCommitFn = std::function<RegOutcome(const RegRequest &)>;

// ── Pipeline runner ───────────────────────────────────────────────────

/// Run the REG-family admission pipeline for a REG_REQ or CONSUMER_REG_REQ.
/// Both msg_types share `ProducerRegReqBody` per §14.3; the pipeline itself is
/// role-type-agnostic.  The commit callback disambiguates producer vs
/// consumer side by reading `RegRequest::role_type` and dispatching to
/// the appropriate HubState primitive (`_on_producer_added` /
/// `_on_consumer_joined`).
///
/// Sequence:
///   1. Extract RegFamilyBodyView from body (typed → typed, no JSON scatter)
///   2. run_reg_family_gates(env, view, gate_ctx) — pre-mutation gates
///   3. Build RegRequest from body (protocol-level typed intermediate)
///   4. commit(request) — state mutation + protocol gates + outcome
///   5. Return the outcome verbatim
///
/// The pipeline does not fire NOTIFYs, does not send wire responses,
/// does not hold HubState locks.  Caller (broker handler) does those
/// against the returned outcome.
///
/// Gate callbacks are supplied via `gate_ctx.cb`; the pipeline uses the
/// same `AdmissionContext` reference the gates read from — a single
/// source of truth, no parallel copy on the pipeline surface.
[[nodiscard]] PYLABHUB_UTILS_EXPORT RegOutcome run_reg_admission(
    const ::pylabhub::wire::WireEnvelope &env, const ::pylabhub::wire::ProducerRegReqBody &body,
    const AdmissionContext &gate_ctx, const RegCommitFn &commit);

} // namespace pylabhub::admission
