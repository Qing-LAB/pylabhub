/**
 * @file test_replay_guard.cpp
 * @brief L1 unit tests for pylabhub::utils::ReplayGuard (I-REPLAY-BOUND).
 *
 * ReplayGuard is the ONE sliding-window nonce-dedup primitive shared by
 * the inbox, REG, and admin planes (HEP-CORE-0027 §3.6).  The guard OWNS
 * its clock (default: monotonic steady) — there is no per-call timestamp,
 * so a caller cannot feed client-supplied time into the dedup window.
 * These tests inject a controllable `ClockFn` at construction to drive
 * time deterministically and pin:
 *   - basic dedup, per-identity + per-nonce keying, fail-closed on empty;
 *   - prune-on-access window expiry keyed to the guard's own clock;
 *   - the window-sizing invariant behind task #67 — a replay stays skew-
 *     acceptable for up to 2*skew after the original, so the window MUST be
 *     >= 2*skew or a late-but-skew-valid replay is wrongly admitted.
 *
 * The clock-SOURCE half of the #67 fix is now STRUCTURAL: the callback the
 * admission pipeline invokes (record_and_check_nonce) has no timestamp
 * argument at all, so there is nothing for a caller to get wrong — the
 * previous "gate must pass broker time" test is obsolete by construction.
 */

#include "utils/replay_guard.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using pylabhub::utils::ReplayGuard;

namespace
{
constexpr std::uint64_t kSkewMs = 30'000;
constexpr std::uint64_t kWindowMs = 2 * kSkewMs; // the #67 invariant: >= 2*skew

// A ReplayGuard whose clock reads a mutable `now`, so tests advance time
// deterministically.  `now` outlives `g` (declared first), so the captured
// reference in the ClockFn stays valid for the guard's lifetime.
struct FakeClockGuard
{
    std::uint64_t now{1'000'000ULL};
    ReplayGuard g{[this] { return now; }};
};
} // namespace

TEST(ReplayGuard, FreshNonceAccepted)
{
    FakeClockGuard t;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
}

TEST(ReplayGuard, DuplicateWithinWindowRejected)
{
    FakeClockGuard t;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
    // Same (identity, nonce), later but within the window → collision.
    t.now += 30'000;
    EXPECT_FALSE(t.g.check_and_record("role.a", "n1", kWindowMs));
}

TEST(ReplayGuard, DistinctNoncesSameIdentityBothFresh)
{
    FakeClockGuard t;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
    EXPECT_TRUE(t.g.check_and_record("role.a", "n2", kWindowMs));
}

TEST(ReplayGuard, SameNonceDistinctIdentitiesBothFresh)
{
    FakeClockGuard t;
    // Keyed by identity: the same nonce from a different sender is fresh.
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
    EXPECT_TRUE(t.g.check_and_record("role.b", "n1", kWindowMs));
}

TEST(ReplayGuard, EmptyIdentityOrNonceFailsClosed)
{
    FakeClockGuard t;
    EXPECT_FALSE(t.g.check_and_record("", "n1", kWindowMs));
    EXPECT_FALSE(t.g.check_and_record("role.a", "", kWindowMs));
}

TEST(ReplayGuard, NonceForgottenAfterWindowElapses)
{
    FakeClockGuard t;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
    // Clock advances strictly beyond the window → the entry is pruned and
    // the same nonce is fresh again (bounded footprint, no permanent memory).
    t.now += kWindowMs + 1;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
}

TEST(ReplayGuard, EntryAtWindowEdgeStillRemembered)
{
    FakeClockGuard t;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
    // A replay exactly at the window edge is still remembered (not yet pruned).
    t.now += kWindowMs;
    EXPECT_FALSE(t.g.check_and_record("role.a", "n1", kWindowMs));
}

TEST(ReplayGuard, DefaultConstructedUsesRealSteadyClock)
{
    // The default ctor installs a monotonic steady clock (no injection).
    // Two immediate records of the same nonce collide (well within any
    // sane window), proving the default clock is wired and non-null.
    ReplayGuard g; // default steady clock
    EXPECT_TRUE(g.check_and_record("role.a", "n1", kWindowMs));
    EXPECT_FALSE(g.check_and_record("role.a", "n1", kWindowMs));
}

// ── The window-sizing invariant behind task #67 ───────────────────────────
//
// A replay is skew-acceptable for up to 2*skew after the original (the skew
// tolerance bounds BOTH the original's acceptance and the replay's).  These
// two tests pin that a window of 2*skew catches the latest such replay, while
// a window of only 1*skew (the pre-#67 value) would wrongly forget the nonce
// and admit it.

TEST(ReplayGuard, WindowOfTwiceSkewCatchesLatestSkewValidReplay)
{
    FakeClockGuard t;
    const std::uint64_t t0 = t.now;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", kWindowMs));
    // Latest a replay can still be skew-valid: t0 + 2*skew - 1.  With
    // window == 2*skew the nonce is still remembered → replay is caught.
    t.now = t0 + 2 * kSkewMs - 1;
    EXPECT_FALSE(t.g.check_and_record("role.a", "n1", kWindowMs));
}

TEST(ReplayGuard, WindowOfOnlyOneSkewWronglyForgetsLateReplay)
{
    FakeClockGuard t;
    const std::uint64_t t0 = t.now;
    const std::uint64_t undersized_window = kSkewMs; // pre-#67 window == skew
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", undersized_window));
    // A replay at t0 + skew + 1 is still skew-valid but the undersized window
    // has already pruned the nonce → wrongly admitted.  This documents WHY the
    // production windows are set to 2*skew.
    t.now = t0 + kSkewMs + 1;
    EXPECT_TRUE(t.g.check_and_record("role.a", "n1", undersized_window))
        << "documents the pre-#67 hazard: window < 2*skew forgets a "
           "skew-valid replay; production windows are 2*skew to close it";
}
