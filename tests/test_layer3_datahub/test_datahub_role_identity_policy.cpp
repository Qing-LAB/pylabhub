/**
 * @file test_datahub_role_identity_policy.cpp
 * @brief Role identity policy broker enforcement tests — placeholder
 *        mechanism per HEP-CORE-0035 §1.
 *
 * Suite 2 only.  Suite 1 (`RoleIdentityPolicyEnumTest`) — the pure
 * `_to_str` / `_from_str` helper tests — was RE-LAYERED to L2 on
 * 2026-06-30 as `tests/test_layer2_service/test_role_identity_policy.cpp`
 * (#154 AUTH-6 batch-2a C6, audit
 * `REVIEW_AUTH6_TestDisposition_2026-06-27.md` File 10 Suite 1).  The
 * helpers themselves stay in production for use by WARN logs + error
 * responses in `broker_service.cpp::check_role_identity`.
 *
 * Suite 2 (`RoleIdentityPolicyBrokerTest`) — broker enforcement of the
 * four `RoleIdentityPolicy` modes + per-channel glob override.
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern to Pattern 3 (subprocess per TEST_F).
 * Worker bodies live in `workers/role_identity_policy_workers.cpp` —
 * see that file's header for the placeholder-mechanism rationale
 * (real-HubHost wiring is structurally blocked because
 * `HubBrokerConfig` deliberately omits the auth fields pending
 * HEP-0035).
 *
 * SCHEDULED FOR DELETION 2026-06-30 (audit File 10 Suite 2): the
 * placeholder `RoleIdentityPolicy` mechanism retires with task #152
 * (HEP-CORE-0035 §8 Phase 6).  Suite 2 + its workers retire as part of
 * the same change.  Until #152 ships, the file stays MASKED in
 * `tests/test_layer3_datahub/CMakeLists.txt` so it compiles cleanly
 * against the post-strict-CURVE surface only when the surface still
 * exists.
 *
 * Renamed 2026-05-13 from `test_datahub_channel_access_policy.cpp` —
 * the legacy name was misleading: the mechanism verifies role identity
 * at registration, not channel access.
 *
 * @see HEP-CORE-0035 §1 (status: placeholder pending retirement)
 * @see HEP-CORE-0035 §8 Phase 6 (deletion plan — task #152)
 */

#include "test_patterns.h"

#include <gtest/gtest.h>

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
