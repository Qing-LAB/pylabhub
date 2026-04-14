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
