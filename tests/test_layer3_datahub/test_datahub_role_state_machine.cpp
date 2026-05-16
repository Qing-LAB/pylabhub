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
