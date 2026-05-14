/**
 * @file test_datahub_role_identity_policy.cpp
 * @brief Role identity policy tests — placeholder mechanism per
 *        HEP-CORE-0035 §1 (pending HEP-0035 Phase 6 retirement).
 *
 * Two suites:
 *
 *   Suite 1 (`RoleIdentityPolicyEnumTest`) — pure enum/string
 *     conversion.  Stays Pattern 1 (in-process `::testing::Test`):
 *     no `LOGGER_*`, no lifecycle modules, no broker.  Guards the
 *     `role_identity_policy_to_str` helper that production WARN log
 *     lines + error responses still consume
 *     (`broker_service.cpp` `LOGGER_WARN` + `make_error` sites in
 *     `check_role_identity`).
 *
 *   Suite 2 (`RoleIdentityPolicyBrokerTest`) — broker enforcement of
 *     the four `RoleIdentityPolicy` modes + per-channel glob
 *     override.  Migrated 2026-05-13 from the in-process
 *     `SetUpTestSuite`-owned `LifecycleGuard` antipattern to Pattern 3
 *     (subprocess per TEST_F).  Worker bodies live in
 *     `workers/role_identity_policy_workers.cpp` — see that file's
 *     header for the placeholder-mechanism rationale (real-HubHost
 *     wiring is structurally blocked because `HubBrokerConfig`
 *     deliberately omits the auth fields pending HEP-0035).
 *
 * Renamed 2026-05-13 from `test_datahub_channel_access_policy.cpp` —
 * the legacy name was misleading: the mechanism verifies role
 * identity at registration, not channel access.  "Channel" enters
 * only via the per-glob override selector
 * (`ChannelPolicyOverride`), not as the subject of verification.
 *
 * @see HEP-CORE-0035 §1 (status: placeholder pending retirement)
 * @see HEP-CORE-0035 §8 Phase 6 (deletion plan — happens after HEP-0035
 *      replacement Layers 1+2 are functional)
 */

#include "test_patterns.h"
#include "utils/role_identity_policy.hpp"

#include <gtest/gtest.h>

using pylabhub::broker::RoleIdentityPolicy;
using pylabhub::broker::role_identity_policy_from_str;
using pylabhub::broker::role_identity_policy_to_str;

// ============================================================================
// Suite 1 — Enum conversion (Pattern 1; pure unit tests, no broker)
// ============================================================================

class RoleIdentityPolicyEnumTest : public ::testing::Test
{
};

TEST_F(RoleIdentityPolicyEnumTest, ToStrAllValues)
{
    EXPECT_STREQ(role_identity_policy_to_str(RoleIdentityPolicy::Open),     "open");
    EXPECT_STREQ(role_identity_policy_to_str(RoleIdentityPolicy::Tracked),  "tracked");
    EXPECT_STREQ(role_identity_policy_to_str(RoleIdentityPolicy::Required), "required");
    EXPECT_STREQ(role_identity_policy_to_str(RoleIdentityPolicy::Verified), "verified");
}

TEST_F(RoleIdentityPolicyEnumTest, FromStrKnownValues)
{
    EXPECT_EQ(role_identity_policy_from_str("open"),     RoleIdentityPolicy::Open);
    EXPECT_EQ(role_identity_policy_from_str("tracked"),  RoleIdentityPolicy::Tracked);
    EXPECT_EQ(role_identity_policy_from_str("required"), RoleIdentityPolicy::Required);
    EXPECT_EQ(role_identity_policy_from_str("verified"), RoleIdentityPolicy::Verified);
}

TEST_F(RoleIdentityPolicyEnumTest, FromStrUnknownFallsToOpen)
{
    EXPECT_EQ(role_identity_policy_from_str("verfied"),  RoleIdentityPolicy::Open);
    EXPECT_EQ(role_identity_policy_from_str("REQUIRED"), RoleIdentityPolicy::Open);
    EXPECT_EQ(role_identity_policy_from_str(""),         RoleIdentityPolicy::Open);
    EXPECT_EQ(role_identity_policy_from_str("open "),    RoleIdentityPolicy::Open);
}

TEST_F(RoleIdentityPolicyEnumTest, ToStrFromStrRoundTrip)
{
    for (auto pol : {RoleIdentityPolicy::Open, RoleIdentityPolicy::Tracked,
                     RoleIdentityPolicy::Required, RoleIdentityPolicy::Verified})
    {
        EXPECT_EQ(role_identity_policy_from_str(role_identity_policy_to_str(pol)),
                  pol);
    }
}

// ============================================================================
// Suite 2 — Broker policy enforcement (Pattern 3; subprocess per TEST_F)
// ============================================================================

using pylabhub::tests::IsolatedProcessTest;

class RoleIdentityPolicyBrokerTest : public IsolatedProcessTest
{
};

TEST_F(RoleIdentityPolicyBrokerTest, OpenPolicyAcceptsAnonymous)
{
    auto w = SpawnWorker("role_identity_policy.open_policy_accepts_anonymous");
    ExpectWorkerOk(w);
}

TEST_F(RoleIdentityPolicyBrokerTest, OpenPolicyAcceptsWithIdentity)
{
    auto w = SpawnWorker(
        "role_identity_policy.open_policy_accepts_with_identity");
    ExpectWorkerOk(w);
}

TEST_F(RoleIdentityPolicyBrokerTest, RequiredPolicyRejectsAnonymous)
{
    auto w = SpawnWorker(
        "role_identity_policy.required_policy_rejects_anonymous");
    ExpectWorkerOk(w);
}

TEST_F(RoleIdentityPolicyBrokerTest, RequiredPolicyAcceptsWithIdentity)
{
    auto w = SpawnWorker(
        "role_identity_policy.required_policy_accepts_with_identity");
    ExpectWorkerOk(w);
}

TEST_F(RoleIdentityPolicyBrokerTest, VerifiedPolicyRejectsUnknownRole)
{
    auto w = SpawnWorker(
        "role_identity_policy.verified_policy_rejects_unknown_role");
    ExpectWorkerOk(w);
}

TEST_F(RoleIdentityPolicyBrokerTest, VerifiedPolicyAcceptsKnownRole)
{
    auto w = SpawnWorker(
        "role_identity_policy.verified_policy_accepts_known_role");
    ExpectWorkerOk(w);
}

TEST_F(RoleIdentityPolicyBrokerTest, PerChannelGlobOverrideRestrictsChannel)
{
    auto w = SpawnWorker(
        "role_identity_policy.per_channel_glob_override_restricts_channel");
    ExpectWorkerOk(w);
}
