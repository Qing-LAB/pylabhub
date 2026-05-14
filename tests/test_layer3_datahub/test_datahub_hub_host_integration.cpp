/**
 * @file test_datahub_hub_host_integration.cpp
 * @brief Pattern 3 driver — HubHost ↔ BrokerService L3 integration tests.
 *
 * Three scenarios verify the broker spawned through `HubHost` actually
 * serves protocol traffic.  Worker bodies live in
 * `workers/hub_host_integration_workers.cpp`.
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

TEST_F(HubHostIntegrationTest, HubHost_Shutdown_BreaksClientConnection)
{
    auto w = SpawnWorker("hub_host_integration.hubhost_shutdown_breaksclientconnection");
    ExpectWorkerOk(w);
}
