/**
 * @file test_datahub_hub_host_integration.cpp
 * @brief Pattern 3 driver — HubHost ↔ BrokerService L3 integration tests.
 *
 * Two scenarios verify the broker spawned through `HubHost` actually
 * serves protocol traffic.  Worker bodies live in
 * `workers/hub_host_integration_workers.cpp`.  Cross-process hub-death
 * observability (HEP-CORE-0023 §2.5.3) is tracked at L4 (task #296) —
 * it cannot be expressed in-process.
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class HubHostIntegrationTest : public IsolatedProcessTest
{
};

TEST_F(HubHostIntegrationTest, HubHost_BrokerReachable_AfterStartup)
{
    auto w = SpawnWorker("hub_host_integration.hubhost_brokerreachable_afterstartup");
    ExpectWorkerOk(w);
}

TEST_F(HubHostIntegrationTest, HubHost_RegReq_RoundTripsViaSpawnedBroker)
{
    auto w = SpawnWorker("hub_host_integration.hubhost_regreq_roundtripsviaspawnedbroker");
    ExpectWorkerOk(w);
}

// REMOVED 2026-06-27 — `HubHost_Shutdown_BreaksClientConnection` cannot be
// expressed at L3.  Designer call (archived
// `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`
// §"Phase 1 known bugs surfaced during landing — BRC monitor CURVE
// blindspot"): the contract being pinned is HEP-CORE-0023 §2.5.3
// "disconnect is terminal" cross-process behaviour.  In-process L3
// shares `pylabhub::hub::get_zmq_context()`, hitting a libzmq
// shared-context quirk that suppresses ZMQ_EVENT_DISCONNECTED on
// CURVE-encrypted peer close — a false-negative L3 cannot work around.
// L4 replacement (spawns plh_hub + plh_role as separate processes, kills
// plh_hub, asserts role-side observes disconnect within heartbeat
// timeout) is tracked in `docs/todo/TESTING_TODO.md` as a follow-up
// that must EVALUATE overlap with existing L4 tests before deciding
// merge / revise existing / create new.
