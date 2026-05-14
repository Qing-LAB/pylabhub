/**
 * @file test_datahub_channel_access_policy.cpp
 * @brief Connection access policy tests — placeholder mechanism per
 *        HEP-CORE-0035 §1 (pending HEP-0035 Phase 6 retirement).
 *
 * Two suites:
 *
 *   Suite 1 (`ConnectionPolicyEnumTest`) — pure enum/string conversion.
 *     Stays Pattern 1 (in-process `::testing::Test`): no `LOGGER_*`,
 *     no lifecycle modules, no broker.  Guards the
 *     `connection_policy_to_str` helper that production WARN log
 *     lines + error responses still consume
 *     (`broker_service.cpp:2532, :2535`).
 *
 *   Suite 2 (`ConnectionPolicyBrokerTest`) — broker enforcement of
 *     the four `ConnectionPolicy` modes + per-channel glob override.
 *     Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 *     `LifecycleGuard` antipattern to Pattern 3 (subprocess per
 *     TEST_F).  Worker bodies live in
 *     `workers/channel_access_policy_workers.cpp` — see that file's
 *     header for the placeholder-mechanism rationale (real-HubHost
 *     wiring is structurally blocked because `HubBrokerConfig`
 *     deliberately omits the auth fields pending HEP-0035).
 *
 * @see HEP-CORE-0035 §1 (status: placeholder pending retirement)
 * @see HEP-CORE-0035 §8 Phase 6 (deletion plan — happens after HEP-0035
 *      replacement Layers 1+2 are functional)
 */

#include "test_patterns.h"
#include "utils/channel_access_policy.hpp"

#include <gtest/gtest.h>

using pylabhub::broker::ConnectionPolicy;
using pylabhub::broker::connection_policy_from_str;
using pylabhub::broker::connection_policy_to_str;

// ============================================================================
// Suite 1 — Enum conversion (Pattern 1; pure unit tests, no broker)
// ============================================================================

class ConnectionPolicyEnumTest : public ::testing::Test
{
};

TEST_F(ConnectionPolicyEnumTest, ToStrAllValues)
{
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Open),     "open");
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Tracked),  "tracked");
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Required), "required");
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Verified), "verified");
}

TEST_F(ConnectionPolicyEnumTest, FromStrKnownValues)
{
    EXPECT_EQ(connection_policy_from_str("open"),     ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("tracked"),  ConnectionPolicy::Tracked);
    EXPECT_EQ(connection_policy_from_str("required"), ConnectionPolicy::Required);
    EXPECT_EQ(connection_policy_from_str("verified"), ConnectionPolicy::Verified);
}

TEST_F(ConnectionPolicyEnumTest, FromStrUnknownFallsToOpen)
{
    EXPECT_EQ(connection_policy_from_str("verfied"),  ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("REQUIRED"), ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str(""),         ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("open "),    ConnectionPolicy::Open);
}

TEST_F(ConnectionPolicyEnumTest, ToStrFromStrRoundTrip)
{
    for (auto pol : {ConnectionPolicy::Open, ConnectionPolicy::Tracked,
                     ConnectionPolicy::Required, ConnectionPolicy::Verified})
    {
        EXPECT_EQ(connection_policy_from_str(connection_policy_to_str(pol)),
                  pol);
    }
}

// ============================================================================
// Suite 2 — Broker policy enforcement (Pattern 3; subprocess per TEST_F)
// ============================================================================

using pylabhub::tests::IsolatedProcessTest;

class ConnectionPolicyBrokerTest : public IsolatedProcessTest
{
};

TEST_F(ConnectionPolicyBrokerTest, OpenPolicyAcceptsAnonymous)
{
    auto w = SpawnWorker("channel_access_policy.open_policy_accepts_anonymous");
    ExpectWorkerOk(w);
}

TEST_F(ConnectionPolicyBrokerTest, OpenPolicyAcceptsWithIdentity)
{
    auto w = SpawnWorker(
        "channel_access_policy.open_policy_accepts_with_identity");
    ExpectWorkerOk(w);
}

TEST_F(ConnectionPolicyBrokerTest, RequiredPolicyRejectsAnonymous)
{
    auto w = SpawnWorker(
        "channel_access_policy.required_policy_rejects_anonymous");
    ExpectWorkerOk(w);
}

TEST_F(ConnectionPolicyBrokerTest, RequiredPolicyAcceptsWithIdentity)
{
    auto w = SpawnWorker(
        "channel_access_policy.required_policy_accepts_with_identity");
    ExpectWorkerOk(w);
}

TEST_F(ConnectionPolicyBrokerTest, VerifiedPolicyRejectsUnknownRole)
{
    auto w = SpawnWorker(
        "channel_access_policy.verified_policy_rejects_unknown_role");
    ExpectWorkerOk(w);
}

TEST_F(ConnectionPolicyBrokerTest, VerifiedPolicyAcceptsKnownRole)
{
    auto w = SpawnWorker(
        "channel_access_policy.verified_policy_accepts_known_role");
    ExpectWorkerOk(w);
}

TEST_F(ConnectionPolicyBrokerTest, PerChannelGlobOverrideRestrictsChannel)
{
    auto w = SpawnWorker(
        "channel_access_policy.per_channel_glob_override_restricts_channel");
    ExpectWorkerOk(w);
}
