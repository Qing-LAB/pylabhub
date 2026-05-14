/**
 * @file test_datahub_hub_federation.cpp
 * @brief Pattern 3 driver — Hub Federation protocol tests (HEP-CORE-0022).
 *
 * Three scenarios exercise the federation control plane between two
 * in-process brokers; each spins up live `BrokerService` instances and
 * therefore must run in a worker subprocess per
 * `docs/README/README_testing.md` § "Choosing a test pattern".
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/hub_federation_workers.cpp` and register their dispatcher
 * at static-init.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class BrokerFederationTest : public IsolatedProcessTest
{
};

TEST_F(BrokerFederationTest, HelloHandshake_FiresOnHubConnected)
{
    auto w = SpawnWorker("hub_federation.hello_handshake_fires_on_hub_connected");
    ExpectWorkerOk(w);
}

TEST_F(BrokerFederationTest, TargetedMessage_FiresOnHubMessage)
{
    auto w = SpawnWorker("hub_federation.targeted_message_fires_on_hub_message");
    ExpectWorkerOk(w);
}

TEST_F(BrokerFederationTest, PeerBye_TriggersOnHubDisconnected)
{
    auto w = SpawnWorker("hub_federation.peer_bye_triggers_on_hub_disconnected");
    ExpectWorkerOk(w);
}
