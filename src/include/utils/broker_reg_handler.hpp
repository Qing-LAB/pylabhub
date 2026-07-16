#pragma once
/**
 * @file broker_reg_handler.hpp
 * @brief Broker-side integration adapter — binds the reusable REG admission
 *        pipeline to a specific broker's HubState + known_roles config.
 *
 * ⚠ STATUS (2026-07-16) — NOT THE LIVE PATH YET.  See task #57 (HEP-0046
 *   Phase B) for the migration + the full parity list.
 *   - LIVE + DONE: the wire layer this composes over — WireEnvelope parse,
 *     typed body classes, and the admission gates (identity/grammar/
 *     known-role/rotation/replay) — all run in production today via
 *     `wire::dispatch::receive_and_validate` (broker_proto 7).
 *   - IN PROGRESS: THIS handler is a SKELETON invoked ONLY from tests.  The
 *     live broker REG path is still the handcrafted `handle_reg_req` /
 *     `handle_consumer_reg_req` in broker_service.cpp (reached via the
 *     `to_legacy` down-convert in `dispatch_received`; see the
 *     "BrokerRegHandler placeholder" note at broker_service.cpp:6710).
 *   - What THIS commit still lacks vs the live handlers (task #57 parity
 *     list): producer path is ~15% (no ABI / schema-record / inbox / full
 *     ProducerEntry / REG_ACK build; fan-in producers are HARD-REJECTED at
 *     the R6 pre-check, see the .cpp); CONSUMER path is 0% (handle()
 *     rejects role_type!=producer); reject codes are collapsed to
 *     `invalid_request`.  The handcrafted handlers remain the complete,
 *     tested reference — this is a relocate-and-retire job, no redesign.
 *
 * Single owner of the callback wiring for REG_REQ / CONSUMER_REG_REQ paths.
 * BrokerServiceImpl constructs one instance at startup.  INTENT (post
 * task #57): every REG-family handler runs the same `handle()` entry point,
 * with no per-request callback construction and no scatter of admission
 * logic across handlers.
 *
 * Architecture layers (top-down) — TARGET shape once task #57 wires it in:
 *
 *   BrokerServiceImpl (ROUTER poll loop, wire I/O)
 *      │  parses WireEnvelope, dispatches on msg_type
 *      ▼
 *   BrokerRegHandler (this file, adapter/binding)
 *      │  binds AdmissionCallbacks + RegCommitFn to HubState primitives
 *      ▼
 *   run_reg_admission (reg_admission_pipeline.hpp, orchestration)
 *      │  runs gates → commit → outcome variant
 *      ▼
 *   HubState primitives (state mutation under writer lock)
 *
 * BrokerRegHandler is intentionally small — it does no wire I/O and no
 * NOTIFY fan-out.  Those live at the BrokerServiceImpl layer, driven by
 * the typed outcome the handler returns.
 */

#include "pylabhub_utils_export.h"
#include "utils/admission_gates.hpp"
#include "utils/reg_admission_pipeline.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace pylabhub::hub
{
class HubState;
}

namespace pylabhub::wire
{
class WireEnvelope;
class ProducerRegReqBody;
}

namespace pylabhub::admission
{

/// Known-roles configuration surface consumed by BrokerRegHandler.  In
/// production BrokerServiceImpl binds these to its known_roles.json
/// backing; in tests they bind to in-process maps.  Kept as a small
/// injection surface so the handler itself is HubState-agnostic beyond
/// the state-mutation primitives.
struct KnownRolesConfig
{
    /// Look up the CURVE pubkey registered for a role_uid in
    /// `known_roles`.  Returns empty string when the uid is not present.
    std::function<std::string(std::string_view role_uid)>
        lookup_pubkey_for_uid;
};

/// Broker tunables consumed by the admission gates.  Kept in one struct
/// so BrokerServiceImpl can pass its config through cleanly.
///
/// C3 resolution 2026-07-14: no `broker_proto` scalar field.
/// Wire-version + ABI compatibility is carried by REG-family REQ
/// bodies' `abi_fingerprint` object (7-axis ComponentVersions per
/// HEP-CORE-0032 §8) and verified through
/// `pylabhub::version::verify_peer_versions()` at the broker's REG
/// handler.  The former hardcoded `broker_proto{7U}` field is
/// retired.
struct BrokerAdmissionConfig
{
    std::uint64_t skew_tolerance_ms{30'000ULL}; ///< I-REPLAY-BOUND
    std::uint64_t nonce_window_ms{10'000ULL};   ///< I-REPLAY-BOUND
};

/// Adapter that binds the REG admission pipeline to a specific broker
/// instance.  One instance per BrokerServiceImpl; all REG-family
/// handlers share it.
///
/// Not copyable / movable — held by-value inside BrokerServiceImpl.
class PYLABHUB_UTILS_EXPORT BrokerRegHandler
{
  public:
    /// Constructor binds the callback wiring against long-lived references
    /// (HubState + known_roles config).  Caller guarantees both outlive
    /// this handler.
    BrokerRegHandler(::pylabhub::hub::HubState &hub_state,
                     KnownRolesConfig           known_roles,
                     BrokerAdmissionConfig      config);

    BrokerRegHandler(const BrokerRegHandler &)            = delete;
    BrokerRegHandler &operator=(const BrokerRegHandler &) = delete;
    BrokerRegHandler(BrokerRegHandler &&)                 = delete;
    BrokerRegHandler &operator=(BrokerRegHandler &&)      = delete;

    /// Handle one REG_REQ or CONSUMER_REG_REQ.  Runs the full pipeline:
    ///   1. Pre-mutation gates (§14.5 gates 2-7)
    ///   2. State-mutation commit (dispatches on request.role_type to
    ///      the correct HubState primitive)
    ///   3. Returns typed outcome
    ///
    /// Caller pattern-matches the outcome to emit REG_ACK / ERROR /
    /// pending-queue-park and to fire the recorded NOTIFY manifest.
    [[nodiscard]] RegOutcome
    handle(const ::pylabhub::wire::WireEnvelope &env,
            const ::pylabhub::wire::ProducerRegReqBody    &body);

  private:
    ::pylabhub::hub::HubState &hub_state_;
    KnownRolesConfig           known_roles_;
    BrokerAdmissionConfig      config_;

    /// Callbacks bound ONCE at construction — pipeline reuses across
    /// every REG_REQ.  Non-static because they capture `this` /
    /// `hub_state_` / `known_roles_`.
    AdmissionCallbacks         gate_callbacks_;
};

}  // namespace pylabhub::admission
