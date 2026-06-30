/**
 * @file test_role_identity_policy.cpp
 * @brief Pattern 1 unit tests for the `role_identity_policy_to_str` /
 *        `role_identity_policy_from_str` enum↔string helpers.
 *
 * RE-LAYERED 2026-06-30 from `tests/test_layer3_datahub/` (#154
 * AUTH-6 batch-2a C6 — audit
 * `REVIEW_AUTH6_TestDisposition_2026-06-27.md` File 10 Suite 1).
 *
 * Why L2.  These are pure value-conversion tests against helpers in
 * `src/utils/role_identity_policy.{hpp,cpp}` (no broker, no
 * `LifecycleGuard`, no `LOGGER_*`).  Their L3-aggregate placement was
 * historical — they belong at L2 alongside the other utility-API tests
 * (`test_role_reg_payload`, `test_z85_public_key`, etc.).
 *
 * Why the helpers stay in production.  Even after the
 * `RoleIdentityPolicy` mechanism retires under HEP-CORE-0035 §8
 * Phase 6 (task #152), the `_to_str` / `_from_str` helpers themselves
 * are still consumed by production WARN log messages + JSON error
 * responses in `broker_service.cpp::check_role_identity` (string-form
 * error codes survive longer than the policy enum because they
 * cross the wire).  These tests guard the helpers.
 *
 * @see HEP-CORE-0035 §1 (status: placeholder pending retirement)
 * @see HEP-CORE-0035 §8 Phase 6 (retirement plan — task #152)
 */

#include "utils/role_identity_policy.hpp"

#include <gtest/gtest.h>

using pylabhub::broker::RoleIdentityPolicy;
using pylabhub::broker::role_identity_policy_from_str;
using pylabhub::broker::role_identity_policy_to_str;

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
