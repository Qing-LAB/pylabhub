/**
 * @file test_datahub_role_state_machine.cpp
 * @brief HEP-CORE-0023 §2.5 — broker role-liveness state machine tests.
 *
 * Exercises the two-pass timeout state machine (Ready -> Pending -> deregistered)
 * and the matching RoleStateMetrics counters. Uses explicit short timeout
 * overrides so the full cycle completes in well under one second.
 */

#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubRoleStateMachineTest : public IsolatedProcessTest
{
};

TEST_F(DatahubRoleStateMachineTest, MetricsCounters_ReclaimCycle)
{
    // Full Ready -> Pending -> deregistered cycle, verified via
    // BrokerService::query_role_state_metrics() counters (no wall-clock
    // assertions — protocol-level observability per HEP-CORE-0019).
    auto proc = SpawnWorker("broker_role_state.metrics_reclaim_cycle", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, PendingRecoversToReady_OnHeartbeat)
{
    // Transient network flicker: role demoted Ready -> Pending, then recovers
    // via heartbeat. Counter pending_to_ready_total increments.
    auto proc = SpawnWorker("broker_role_state.pending_recovers_to_ready", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, StuckInPending_ReclaimedAfterPendingTimeout)
{
    // Role sends REG_REQ but never heartbeats (crash-before-Ready).
    // After pending_timeout, broker deregisters + sends CHANNEL_CLOSING_NOTIFY.
    auto proc = SpawnWorker("broker_role_state.stuck_in_pending_reclaimed", {});
    ExpectWorkerOk(proc);
}

// BandMembership_CleanedOnRoleClose MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (Pattern4BrokerProtocolTest.Band_MembershipCleanedOnProducerDereg) —
// task #52 DirectBrokerHandle sweep.  The effect (band cleanup on
// producer dereg via on_channel_closed) is observed entirely over the
// wire (BAND_MEMBERS count 2→1), so the in-process co-host is retired.

TEST_F(DatahubRoleStateMachineTest, RoleEntry_TerminalCleanup_OnLastPresenceDisconnect)
{
    // Wave M3 step 5b (2026-05-11) — pins the H1 wiring contract.
    // Without `_dispatch_role_disconnected_if_dead` fired from
    // `_on_channel_closed`, the role entry would linger in
    // `HubState::pImpl->roles` after the last presence transitions
    // Disconnected (the "stale residue" concern from Wave M2.5 §6.2).
    //
    // Scenario:
    //   1. Producer registers a channel (last-and-only producer).
    //   2. Producer DEREGs → broker `_on_producer_dropped` (last) →
    //      `_on_channel_closed` → marks producer-presence Disconnected
    //      → dispatches terminal cleanup.
    //   3. Worker asserts `hub_state.role(uid)` returns nullopt within
    //      a short poll window (broker thread races with the worker
    //      thread for the entry erase).
    //
    // A mutation that disables the dispatch must make this test fail.
    auto proc = SpawnWorker("broker_role_state.role_entry_terminal_cleanup_on_last_presence_dereg", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleEntry_TerminalCleanup_OnConsumerLeftLast)
{
    // Wave M3 step 5b (2026-05-11) — pins dispatch from
    // `_on_consumer_left`.  Consumer registers (its only presence) and
    // DEREGs; role entry must be erased.
    auto proc = SpawnWorker("broker_role_state.role_entry_terminal_cleanup_on_consumer_left_last", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, ConsumerHeartbeatTimeout_FiresConsumerDiedNotify)
{
    // Wave-B M2 (3/3) — broker sweep over consumer-presences.  A
    // consumer that stops heartbeating must transition Connected →
    // Pending → Disconnected on the broker, and the broker must
    // emit CONSUMER_DIED_NOTIFY (reason="heartbeat_timeout") to
    // every producer on the channel.  Producer + channel are unaffected:
    // consumer-presence disconnect never tears down channels per
    // HEP-CORE-0023 §2.1.1.
    auto proc = SpawnWorker(
        "broker_role_state.consumer_heartbeat_timeout_fires_consumer_died_notify", {});
    ExpectWorkerOk(proc);
}

// RE-LAYERED 2026-06-27 — 4 `RoleHandler_*` tests moved to L2
// `tests/test_layer2_service/test_role_handler.cpp` per the AUTH-6
// layer-fit audit (`docs/code_review/REVIEW_AUTH6_TestDisposition_2026-06-27.md`
// §2 addendum).  They pin RoleHandler single-class state-flag +
// pointer-identity behavior; `BRC::is_connected()` reads the BRC's
// internal `connected` flag (set at end of `connect()`, before any
// wire handshake), so the broker that lived here was scaffolding
// only and the L3 layer was incorrect.  Removed:
//   - RoleHandler_Connections_StartStop_Smoke   → RoleHandlerLifecycle.StartStop_Smoke_SinglePresence
//   - RoleHandler_Connections_DualHub           → RoleHandlerLifecycle.StartStop_DualHub_BothConnectionsConnected
//   - RoleHandler_Connections_DoubleStart_Rejected → RoleHandlerLifecycle.DoubleStart_Rejected_StateNotCleared
//   - RoleHandler_BrcForX_PostStart_PointerIdentity → RoleHandlerRouting.BrcForX_PostStart_PointerIdentity

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_StartHandlerThreads_E2E)
{
    // Wave-B M4c — end-to-end test for the handler-mode ctrl thread
    // path.  Verifies:
    //  - start_handler_threads succeeds against a real broker.
    //  - api.handler() returns non-null + connections_started().
    //  - Atomicity guard: second start refused without disturbing state.
    //  - REG_REQ via the legacy fallback view (pImpl->broker_channel
    //    set to handler->connections()[0].brc) reaches the broker.
    //  - Broker HubState shows the role registered.
    //  - stop_handler_threads cleanly drains + clears state +
    //    fallback view (api.handler() == nullptr).
    //  - Second stop is idempotent.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_start_handler_threads_e2e", {});
    ExpectWorkerOk(proc);
}

// RETIRED 2026-06-29 under #154 C3 — 7 dual-broker tests
// (StartHandlerThreads_DualHub_E2E, HubDead_{Peer,Transitions,Master},
// WaitForRole_DualHub_Fallthrough, SourceHubUid_Disambiguates_DualHub,
// DualHub_Heartbeat_PerPresence) deleted from this file: two in-process
// `BrokerService` instances violate the HEP-CORE-0036 §7.1 single-pumper
// invariant (`ZapRouter::pump_one` PANICs on concurrent entry from two
// broker poll loops sharing one ZapRouter).  Absorbed contracts tracked
// in `docs/todo/TESTING_TODO.md` § "Test retirements / cross-layer
// migrations" 2026-06-29 rows; L4 successors blocked on #298
// (Pattern4Setup multi-hub extension).

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_BandNotify_WireField_And_Routing)
{
    // Audit B1 + B2 regression test (2026-05-17) — pins TWO things
    // against a REAL broker emission of BAND_JOIN_NOTIFY:
    //
    //  (B1) HEP-CORE-0030 §5.1 wire conformance — body carries the
    //       band identifier under key `band`, not the legacy
    //       `channel` (leftover from before the 2026-04-11 rename
    //       refactor `8d3ee1e` that missed wire payload keys).
    //  (B2) RoleHandler::find_presence_from_notification resolves
    //       the real body to the joined presence — pre-fix it
    //       looked for the never-emitted `band_name` key invented
    //       in Wave-B M4b (`8c3994c`) and always returned nullptr.
    //
    // Why 2000+ tests missed both bugs:
    //  - L2 `test_role_handler.cpp` synthesized `body["band_name"]`
    //    in the test fixture, matching the broken dispatcher rather
    //    than real wire data.
    //  - the L3 band tests (now `test_pattern4_channel_group.cpp`) use
    //    raw wire `on_notification`/`drain_for` capture that bypasses
    //    `find_presence_from_notification` entirely.
    //  - No test inspected the wire payload key against the HEP.
    //
    // This test closes both gaps in one round-trip.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_band_notify_wire_field_and_routing",
        {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_RegistrationFSM_Transitions)
{
    // Audit S1+O4 (2026-05-17) — pins the per-Presence registration
    // FSM (Unregistered → RegRequestPending → Registered →
    // Deregistered).  Pre-S1 the role-side had no enumerable
    // registration state; this test exercises each transition
    // against a real broker.
    //
    // Why 1925 tests missed the gap: there was no FSM to test.  The
    // pre-S1 state was carried by string non-emptiness on
    // `Impl::Shared::producer_channel`, which is internal to
    // role_api_base.cpp and not externally observable.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_registration_fsm_transitions",
        {});
    ExpectWorkerOk(proc);
}

// Broker_Band_RejectsInvalidIdentifier (Audit R3.5, 2026-05-17) MIGRATED
// to tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (Pattern4BrokerProtocolTest.Band_RejectsInvalidIdentifier) — task #52
// DirectBrokerHandle sweep.  Pure wire assertions (JOIN/LEAVE/MEMBERS all
// return {status:error, error_code:INVALID_BAND_NAME} for a non-`!` band
// name; valid `!`-prefixed name still succeeds), so the in-process
// co-host is retired.

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_BandJoin_HandlerMode_Bootstrap)
{
    // A0 regression test (2026-05-17) — protects the bootstrap case
    // of api.band_join() under handler-mode.  Post-M4f the legacy
    // broker_channel fallback was deleted, but resolve_bc_for_band
    // still relies on a "fall back to any connection" path for the
    // first-time-join chicken-and-egg case (band_index_ is empty
    // until on_band_joined fires AFTER a successful broker RPC).
    // Without the fix, api.band_join returns std::nullopt without
    // ever reaching the broker.  Pre-fix this test fails on the
    // join_resp.has_value() assertion; post-fix the full round-trip
    // succeeds + band_index_ is populated for subsequent band_*
    // calls.  Coverage of api.band_join was lost during M4f when the
    // channel-group band tests (since retired to Pattern 4,
    // `test_pattern4_channel_group.cpp`) were migrated to call
    // bc->band_join directly; this restores it at the public
    // RoleAPIBase surface.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_band_join_handler_mode", {});
    ExpectWorkerOk(proc);
}
