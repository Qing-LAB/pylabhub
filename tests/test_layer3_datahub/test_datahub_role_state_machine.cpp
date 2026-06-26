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

TEST_F(DatahubRoleStateMachineTest, BandMembership_CleanedOnRoleClose)
{
    // Two roles join a band; one deregisters; on_channel_closed hook
    // must remove it from the band so the other observes only itself.
    auto proc = SpawnWorker("broker_role_state.band_membership_cleaned_on_role_close", {});
    ExpectWorkerOk(proc);
}

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
    // every producer on the channel.  Symmetric with the PID-death
    // path (CONSUMER_DIED_NOTIFY, reason="process_dead", driven by
    // check_dead_consumers).  Producer + channel are unaffected:
    // consumer-presence disconnect never tears down channels per
    // HEP-CORE-0023 §2.1.1.
    auto proc = SpawnWorker(
        "broker_role_state.consumer_heartbeat_timeout_fires_consumer_died_notify", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleHandler_Connections_StartStop_Smoke)
{
    // Wave-B M4a — state-only verification of RoleHandler's network
    // surface.  start_connections() allocates + connects BRCs;
    // stop_connections() releases.  NO threads are spawned by the
    // handler (state holder); thread management lives at the role
    // host layer and lands in M4c.  Mutation: revert start_connections
    // to leave brc nullptr → smoke test fails.
    auto proc = SpawnWorker(
        "broker_role_state.role_handler_connections_start_stop_smoke", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleHandler_Connections_DualHub)
{
    // Wave-B M4a — dual-broker state shape (the M8-payoff data shape):
    // two HubConnections, two BRCs each connected to a different
    // broker.  Verifies the dedup-by-identity model materialises
    // correct distinct connections.  Still state-only (no threads).
    auto proc = SpawnWorker(
        "broker_role_state.role_handler_connections_dual_hub", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleHandler_Connections_DoubleStart_Rejected)
{
    // Wave-B M4a — `start_connections()` rejects double-call without
    // intervening `stop_connections()`.  Pins the idempotency contract.
    auto proc = SpawnWorker(
        "broker_role_state.role_handler_connections_double_start_rejected", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleHandler_BrcForX_PostStart_PointerIdentity)
{
    // Wave-B M4b — verify routing primitives return the right
    // non-null BRC pointer AFTER start_connections.  L2 tests
    // (Pattern 1+) cover the lookup logic (returns nullptr pre-start
    // / unknown channel / unjoined band).  This L3 test pins the
    // post-start pointer identity: brc_for_channel(ch) ==
    // connections()[0].brc.get(), and brc_for_band routes via the
    // joined presence.
    auto proc = SpawnWorker(
        "broker_role_state.role_handler_brc_for_x_post_start", {});
    ExpectWorkerOk(proc);
}

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

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_StartHandlerThreads_DualHub_E2E)
{
    // Wave-B M4c review follow-up — multi-connection variant.
    // Two brokers, two HubConnections, two ctrl threads (first =
    // MASTER, second = peer per HEP-CORE-0031 §4.2.1).  REG_REQ
    // dispatched per-connection via `handler->brc_for_channel(ch)`
    // (not via the fallback view — that only reaches one of the
    // two BRCs).  Each broker observes its own registration.  Pins
    // the M8-payoff data-flow shape at the network layer: both
    // ctrl threads actually drive their BRCs (not just spawned but
    // running poll loops).
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_start_handler_threads_dual_hub_e2e",
        {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_HubDead_TransitionsPresencesToDeregistered)
{
    // Audit R3.3 (2026-05-17) — on_hub_dead must transition every
    // presence pointing at the dead connection from Registered →
    // Deregistered.  Pre-fix the FSM kept claiming Registered against
    // a broker that had already reaped the role via heartbeat-timeout.
    // Companion to A2 (which establishes that the role keeps running
    // on master after peer death); R3.3 establishes that the FSM
    // truthfully reflects the broker-side reality.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_hub_dead_transitions_presences_to_deregistered",
        {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_HubDead_PeerKeepsRoleAlive)
{
    // A2 (Wave-B M8 prep) — dual-hub fault tolerance: PEER broker
    // death must NOT trigger role-wide shutdown.  Pre-A2 any-of-N
    // on_hub_dead callbacks unconditionally called request_stop, so
    // a dual-hub processor exited if EITHER broker hiccupped —
    // strictly worse than single-hub.  Post-A2: master (i==0) death
    // still triggers role exit (preserves single-hub semantics);
    // peer (i>0) death marks the connection dead in the bitmask
    // accessible via api.is_connection_alive(i) but the role keeps
    // running on the master.  See HEP-CORE-0023 §2.5.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_hub_dead_peer_keeps_role_alive", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_HubDead_MasterExitsRole)
{
    // A2 baseline — master broker death MUST still trigger role-wide
    // shutdown (single-hub semantics preserved).  Twin of the peer-
    // keeps-alive test; mutates broker_a (the master) instead of
    // broker_b.  Asserts core.is_running() flips false within 3s and
    // stop_reason becomes "hub_dead".
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_hub_dead_master_exits_role", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_WaitForRole_DualHub_Fallthrough)
{
    // A3 (Wave-B M8 prep) — Class B (role-bound) queries must fall
    // through across all connections per HEP-CORE-0033 §18.3.
    // Spawns 2 brokers, registers a "target" role on hub-B only,
    // builds a dual-hub querier (conn[0]=hub-A, conn[1]=hub-B), and
    // asserts api.wait_for_role(target_uid) finds it via fall-through.
    // Pre-A3 wait_for_role only polled connection[0] (hub-A) and
    // returned false because hub-A doesn't know about target.
    // Mutation: revert iteration → test fails on EXPECT_TRUE.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_wait_for_role_dual_hub_fallthrough",
        {});
    ExpectWorkerOk(proc);
}

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
    //  - L3 `datahub_channel_group_workers.cpp` band tests use raw
    //    BRC `on_notification` callbacks that bypass
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

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_DualHub_Heartbeat_PerPresence)
{
    // Audit C2 closure (2026-05-19) — pins HEP-CORE-0033 §19.3 step 3:
    // heartbeat is per-presence; a dual-hub processor (Consumer on hub-A
    // + Producer on hub-B) emits ONE heartbeat per presence to its OWN
    // hub.  Validates the role-side refactor that replaced `short_tag`
    // string-branching + legacy `pImpl->channel`/`pImpl->out_channel`
    // fields with `handler_->presences()` iteration in
    // `RoleAPIBase::on_heartbeat_tick_` (`src/utils/service/
    // role_api_base.cpp` line 832ish).
    //
    // Mutation: revert the iteration to a hardcoded
    // `emit(pImpl->channel, "consumer")` for proc-role → hub-B never
    // receives a producer-presence heartbeat → broker_b's RoleEntry's
    // producer-presence row never flips `first_heartbeat_seen=true`
    // → test fails on EXPECT_TRUE.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_dual_hub_heartbeat_per_presence",
        {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, RoleAPIBase_SourceHubUid_Disambiguates_DualHub)
{
    // Audit C3 (2026-05-17) — pins `IncomingMessage::source_hub_uid`
    // as the dual-hub message origin tag (HEP-CORE-0023 §7 +
    // HEP-CORE-0033 §18.3 / §19.4).  Pre-C3 the field didn't exist:
    // a dual-hub processor's script had no way to tell which hub a
    // notification came from without comparing channel/band names
    // back to its own presence list.
    //
    // Test shape: 2 brokers, dual-hub handler, processor joins one
    // band on each hub; external BRC clients also join, triggering
    // BAND_JOIN_NOTIFY fanout from each broker.  Test asserts both
    // notifies arrive with distinct `source_hub_uid` values that
    // match the respective broker endpoints.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_source_hub_uid_disambiguates_dual_hub",
        {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubRoleStateMachineTest, Broker_Band_RejectsInvalidIdentifier)
{
    // Audit R3.5 (2026-05-17) — broker must explicitly reject
    // BAND_*_REQ payloads with invalid band names (HEP-CORE-0030 §3
    // grammar — must start with `!`).  Pre-fix the broker silently
    // bumped a counter via `_on_band_joined`'s validator but the
    // handler ignored the validation outcome, returning
    // `status: success` — phantom-join.  Test verifies that all 3
    // request-reply BAND messages now return
    // `{status: error, error_code: INVALID_BAND_NAME}` for invalid
    // names, while valid names still succeed.
    auto proc = SpawnWorker(
        "broker_role_state.broker_band_rejects_invalid_identifier", {});
    ExpectWorkerOk(proc);
}

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
    // calls.  Coverage of api.band_join was lost during M4f when
    // datahub_channel_group_workers.cpp's tests were migrated to
    // call bc->band_join directly; this restores it at the public
    // RoleAPIBase surface.
    auto proc = SpawnWorker(
        "broker_role_state.role_api_base_band_join_handler_mode", {});
    ExpectWorkerOk(proc);
}
