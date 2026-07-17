/**
 * @file test_hub_state_nonce_dedup.cpp
 * @brief L2 tests for HubState::nonce_seen — the anti-replay dedup primitive.
 *
 * Locks I-REPLAY-BOUND state transitions (HEP-CORE-0046
 * §8.1) against a real HubState instance:
 *   - Fresh nonce accepted (returns true, added to per-role window)
 *   - Reused nonce within window rejected (returns false, no double-record)
 *   - Nonce outside window pruned; a subsequent same-value nonce is fresh
 *   - Different role_uids don't collide (independent windows)
 *   - Empty inputs rejected as invalid
 */

#include "utils/hub_state.hpp"
#include "utils/logger.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "binary_lifecycle.h"

#include <gtest/gtest.h>

PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

using pylabhub::hub::HubState;

TEST(HubStateNonceDedup, FreshNonceAccepted)
{
    HubState hub;
    EXPECT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", /*wall_ts=*/1'000'000ULL,
                                /*window_ms=*/10'000ULL));
}

TEST(HubStateNonceDedup, ReusedNonceWithinWindowRejected)
{
    HubState hub;
    ASSERT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", 1'000'000ULL, 10'000ULL));
    // Same (uid, nonce) within the window ⇒ collision.
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "nonce-A", 1'005'000ULL,
                                 10'000ULL));
}

TEST(HubStateNonceDedup, NonceOutsideWindowPrunedAndReaccepted)
{
    HubState hub;
    ASSERT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", 1'000'000ULL, 10'000ULL));
    // Advance past the window (10 s later).  Prune-on-access drops the
    // old entry; the same nonce value is fresh again.  This matches
    // §8.1's sliding-window semantics — the invariant is that within any
    // window nonces are unique, not that nonces are globally unique.
    EXPECT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", 1'020'000ULL,
                                10'000ULL));
}

// Soundness invariant (I-REPLAY-BOUND): the dedup window MUST be >= the
// broker's skew tolerance.  Otherwise an intervening message prunes a nonce
// while a replay of it is still skew-accepted → wrongly admitted.  With
// window == skew (30 s, the production config as of 2026-07-17), an
// intervening message 15 s later does NOT prune the original, so a replay of
// it is still caught.  Under the prior 10 s window this returned TRUE (the
// bug this pins).
TEST(HubStateNonceDedup, WindowGeSkew_ReplayCaughtDespiteInterveningTraffic)
{
    HubState hub;
    constexpr std::uint64_t window = 30'000ULL;  // == skew_tolerance_ms
    ASSERT_TRUE(hub.nonce_seen("prod.uid1", "orig",  1'000'000ULL, window));
    // Intervening legit message 15 s later advances the prune cutoff; with a
    // 30 s window the original (15 s old) survives.
    ASSERT_TRUE(hub.nonce_seen("prod.uid1", "other", 1'015'000ULL, window));
    // Replay of the original nonce (same wall_ts) is still within the window
    // ⇒ CAUGHT.  Under a window < 15 s this would wrongly return true.
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "orig", 1'000'000ULL, window))
        << "window >= skew must keep the original nonce long enough to catch "
           "a replay that the skew check still accepts";
}

TEST(HubStateNonceDedup, DifferentRolesIndependentWindows)
{
    HubState hub;
    ASSERT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", 1'000'000ULL, 10'000ULL));
    // Same nonce value under a different role_uid must not collide —
    // the dedup key is (role_uid, nonce), not nonce alone.  A pubkey-
    // scoped attacker can't force cross-role reject spamming.
    EXPECT_TRUE(hub.nonce_seen("prod.uid2", "nonce-A", 1'005'000ULL,
                                10'000ULL));
}

TEST(HubStateNonceDedup, EmptyInputsRejected)
{
    HubState hub;
    EXPECT_FALSE(hub.nonce_seen("",         "nonce-A", 1'000'000ULL, 10'000ULL));
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "",       1'000'000ULL, 10'000ULL));
}

TEST(HubStateNonceDedup, TwoDistinctNoncesWithinWindowBothAccepted)
{
    HubState hub;
    EXPECT_TRUE(hub.nonce_seen("prod.uid1", "nonce-A", 1'000'000ULL, 10'000ULL));
    EXPECT_TRUE(hub.nonce_seen("prod.uid1", "nonce-B", 1'001'000ULL, 10'000ULL));
    // Both are recorded; reusing either within the window is now rejected.
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "nonce-A", 1'002'000ULL,
                                 10'000ULL));
    EXPECT_FALSE(hub.nonce_seen("prod.uid1", "nonce-B", 1'002'000ULL,
                                 10'000ULL));
}
