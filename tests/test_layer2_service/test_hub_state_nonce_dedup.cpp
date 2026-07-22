/**
 * @file test_hub_state_nonce_dedup.cpp
 * @brief L2 tests for HubState::nonce_seen — the anti-replay dedup delegation.
 *
 * HubState::nonce_seen delegates to the shared `ReplayGuard`, which owns its
 * own trusted monotonic clock (no caller timestamp — task #67).  These L2
 * tests therefore pin only the TIME-INDEPENDENT delegation contract against a
 * real HubState (HEP-CORE-0046 §8.1 / HEP-CORE-0027 §3.6):
 *   - Fresh nonce accepted (returns true, recorded in the per-role window)
 *   - Reused nonce rejected (returns false, no double-record)
 *   - Different role_uids don't collide (independent windows; key = uid+nonce)
 *   - Empty inputs rejected as invalid (fail-closed)
 *
 * The TIME-dependent behavior — window expiry, the window >= 2*skew soundness
 * bound — is pinned on the primitive itself with an injected clock in
 * test_replay_guard.cpp (L1); it cannot be driven deterministically through
 * HubState because the guard reads a real steady clock here (driving it would
 * require real sleeps, which are flaky and forbidden).
 */

#include "utils/hub_state.hpp"
#include "utils/logger.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "binary_lifecycle.h"

#include <gtest/gtest.h>

PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule(),
                             pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

using pylabhub::hub::HubState;

namespace
{
// Any window larger than the microsecond real-time gap between successive
// calls; every reuse below happens far inside it, so these tests are
// deterministic without controlling the clock.
constexpr std::uint64_t kWindowMs = 30'000ULL;
} // namespace

TEST(HubStateNonceDedup, FreshNonceAccepted)
{
    HubState hub;
    EXPECT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", kWindowMs));
}

TEST(HubStateNonceDedup, ReusedNonceRejected)
{
    HubState hub;
    ASSERT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", kWindowMs));
    // Same (uid, nonce) again (microseconds later, far inside the window)
    // ⇒ collision.
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "nonce-A", kWindowMs));
}

TEST(HubStateNonceDedup, DifferentRolesIndependentWindows)
{
    HubState hub;
    ASSERT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", kWindowMs));
    // Same nonce value under a different role_uid must not collide —
    // the dedup key is (role_uid, nonce), not nonce alone.  A pubkey-
    // scoped attacker can't force cross-role reject spamming.
    EXPECT_TRUE(hub.nonce_seen("prod.uid2", "nonce-A", kWindowMs));
}

TEST(HubStateNonceDedup, EmptyInputsRejected)
{
    HubState hub;
    EXPECT_FALSE(hub.nonce_seen("", "nonce-A", kWindowMs));
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "", kWindowMs));
}

TEST(HubStateNonceDedup, TwoDistinctNoncesBothAccepted)
{
    HubState hub;
    EXPECT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", kWindowMs));
    EXPECT_TRUE(hub.nonce_seen("prod.uid1", "nonce-B", kWindowMs));
    // Both are recorded; reusing either is now rejected.
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "nonce-A", kWindowMs));
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "nonce-B", kWindowMs));
}
