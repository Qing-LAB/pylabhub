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
