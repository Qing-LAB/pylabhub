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
 *
 * DEFERRED 2026-06-30 (#154 AUTH-6 batch-2a C6, audit
 * `REVIEW_AUTH6_TestDisposition_2026-06-27.md` File 9): the HELLO /
 * TARGETED_MSG / PEER_BYE control-plane handlers these tests pin are
 * gated on the federation design (#105 "Federation protocol design +
 * cross-hub reg/comm verification").  Tests are DEFERRED — not retired
 * — because the test surfaces themselves are correct; only the broker-
 * side implementation is missing.  Each TEST_F below calls
 * `GTEST_SKIP` with a #105 citation.  When #105 ships, drop the
 * GTEST_SKIP lines and re-enable the workers + the CMakeLists.txt
 * source entries.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class BrokerFederationTest : public IsolatedProcessTest
{
};

TEST_F(BrokerFederationTest, HelloHandshake_FiresOnHubConnected)
{
    GTEST_SKIP() << "DEFERRED — federation HELLO handler depends on "
                    "task #105 (federation protocol design + cross-hub "
                    "reg/comm verification).  Re-enable when #105 ships.";
}

TEST_F(BrokerFederationTest, TargetedMessage_FiresOnHubMessage)
{
    GTEST_SKIP() << "DEFERRED — federation TARGETED_MSG handler depends "
                    "on task #105.  Re-enable when #105 ships.";
}

TEST_F(BrokerFederationTest, PeerBye_TriggersOnHubDisconnected)
{
    GTEST_SKIP() << "DEFERRED — federation PEER_BYE handler depends on "
                    "task #105.  Re-enable when #105 ships.";
}
