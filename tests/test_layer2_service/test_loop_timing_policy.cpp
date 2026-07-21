/**
 * @file test_loop_timing_policy.cpp
 * @brief Unit tests for loop_timing_policy.hpp — shared utilities used by all three roles.
 *
 * Tests:
 *   - parse_loop_timing_policy(): valid values, cross-field constraints, error messages.
 *   - default_loop_timing_policy(): implicit derivation from period_us.
 *   - compute_short_timeout(): short timeout calculation for acquire.
 *   - compute_next_deadline(): deadline computation for all three policies.
 */

#include "utils/loop_timing_policy.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <stdexcept>

using pylabhub::LoopTimingPolicy;

// ============================================================================
// parse_loop_timing_policy (period_us: double, in microseconds)
// ============================================================================

TEST(LoopTimingPolicyTest, Parse_MaxRate_OK)
{
    EXPECT_EQ(pylabhub::parse_loop_timing_policy("max_rate", "test"),
              LoopTimingPolicy::MaxRate);
}

TEST(LoopTimingPolicyTest, Parse_FixedRate_OK)
{
    EXPECT_EQ(pylabhub::parse_loop_timing_policy("fixed_rate", "test"),
              LoopTimingPolicy::FixedRate);
}

TEST(LoopTimingPolicyTest, Parse_FixedRateWithCompensation_OK)
{
    EXPECT_EQ(pylabhub::parse_loop_timing_policy("fixed_rate_with_compensation", "test"),
              LoopTimingPolicy::FixedRateWithCompensation);
}

TEST(LoopTimingPolicyTest, Parse_InvalidString_Throws)
{
    EXPECT_THROW(pylabhub::parse_loop_timing_policy("turbo", "ctx"),
                 std::runtime_error);
}

// Cross-field validation (MaxRate+period, FixedRate+no period, etc.) is now
// enforced by parse_timing_config() — tested in test_role_config.cpp.

// default_loop_timing_policy() removed — loop_timing is now required in config.

// ============================================================================
// compute_short_timeout — io-wait timeout = max(period * ratio, 10µs floor)
// ============================================================================

TEST(LoopTimingComputeTest, ShortTimeout_MaxRate_FloorsAt10us)
{
    // period=0 (MaxRate) → 0 * ratio = 0 → clamped to the 10µs floor.
    EXPECT_EQ(pylabhub::compute_short_timeout(0.0, 0.1),
              std::chrono::microseconds{10});
}

TEST(LoopTimingComputeTest, ShortTimeout_FixedRate_RatioOfPeriod)
{
    // period=1000µs, ratio=0.1 → 100µs (above the floor).
    EXPECT_EQ(pylabhub::compute_short_timeout(1000.0, 0.1),
              std::chrono::microseconds{100});
}

TEST(LoopTimingComputeTest, ShortTimeout_SmallPeriod_ClampsToFloor)
{
    // period=50µs, ratio=0.1 → 5µs → clamped to the 10µs floor.
    EXPECT_EQ(pylabhub::compute_short_timeout(50.0, 0.1),
              std::chrono::microseconds{10});
}

// ============================================================================
// compute_next_deadline — three-branch timing core (HEP-CORE-0008)
//
// CI-robustness: the FixedRate-overrun branch is the ONLY one that reads
// steady_clock::now() (to reset from now).  We NEVER assert its absolute value
// — only RELATIONAL invariants — and we place prev_deadline a full HOUR in the
// past/future so scheduling jitter (even whole seconds) cannot cross a branch
// boundary or invert an inequality.  Deterministic branches are pinned exactly.
// ============================================================================

namespace
{
using Clock                    = std::chrono::steady_clock;
constexpr double     kPeriodUs = 1000.0;                    // 1 ms
const std::chrono::microseconds kPeriod{1000};
const auto           kMaxTp    = Clock::time_point::max();
const std::chrono::hours kHour{1};
} // namespace

TEST(LoopTimingComputeTest, NextDeadline_MaxRate_IsTimePointMax)
{
    // MaxRate has no deadline regardless of the other args.
    const auto out = pylabhub::compute_next_deadline(
        LoopTimingPolicy::MaxRate, Clock::now(), Clock::now(), kPeriodUs);
    EXPECT_EQ(out, kMaxTp);
}

TEST(LoopTimingComputeTest, NextDeadline_FirstCycle_FixedRate_IsCycleStartPlusPeriod)
{
    // prev == max signals the first cycle → cycle_start + period (exact).
    const auto start = Clock::now();
    const auto out   = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, kMaxTp, start, kPeriodUs);
    EXPECT_EQ(out, start + kPeriod);
}

TEST(LoopTimingComputeTest, NextDeadline_FirstCycle_Compensation_IsCycleStartPlusPeriod)
{
    const auto start = Clock::now();
    const auto out   = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRateWithCompensation, kMaxTp, start, kPeriodUs);
    EXPECT_EQ(out, start + kPeriod);
}

TEST(LoopTimingComputeTest, NextDeadline_FixedRate_OnTime_AdvancesFromPrevDeadline)
{
    // prev_deadline 1h in the FUTURE → internal now() is well below prev+period
    // (1h margin) → on-time branch → advance to prev+period EXACTLY (the result
    // does not read now()).
    const auto prev  = Clock::now() + kHour;
    const auto start = Clock::now();  // irrelevant on this branch
    const auto out   = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, prev, start, kPeriodUs);
    EXPECT_EQ(out, prev + kPeriod);
}

TEST(LoopTimingComputeTest, NextDeadline_FixedRate_Overrun_ResetsForwardFromNow)
{
    // prev_deadline 1h in the PAST → internal now() is well above prev+period
    // (1h margin) → overrun branch → reset to now+period.  Assert only the
    // RELATIONAL invariant: the new deadline is strictly FORWARD of the stale
    // schedule prev+period (no absolute now() value asserted).
    const auto prev  = Clock::now() - kHour;
    const auto start = Clock::now();
    const auto out   = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, prev, start, kPeriodUs);
    EXPECT_GT(out, prev + kPeriod)
        << "FixedRate overrun must reset FORWARD (now+period), not stay on the "
           "stale schedule prev+period";
}

TEST(LoopTimingComputeTest,
     NextDeadline_Compensation_Overrun_AdvancesFromPrevDeadline_ExactCatchUp)
{
    // Even under heavy overrun (prev 1h in the past), Compensation advances by
    // EXACTLY one period from prev_deadline (catch-up / steady average rate) and
    // NEVER reads now() — so this is fully deterministic.  Regression pin for the
    // 2026-05-03 bug (Compensation silently degraded to FixedRate = reset-from-now).
    const auto prev  = Clock::now() - kHour;
    const auto start = Clock::now();  // irrelevant on this branch
    const auto out   = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRateWithCompensation, prev, start, kPeriodUs);
    EXPECT_EQ(out, prev + kPeriod);
}

TEST(LoopTimingComputeTest,
     NextDeadline_Overrun_FixedRateResetsPastCompensationCatchUp)
{
    // Same heavy-overrun input to BOTH policies.  FixedRate resets to now+period
    // (≈ now, ~1h ahead of the stale schedule); Compensation stays at prev+period
    // (the stale schedule, ~1h in the past).  So FixedRate's deadline is strictly
    // LATER.  If the 2026-05-03 regression returned (Compensation → reset-from-now)
    // the two would be EQUAL — this asserts they differ, deterministically (~1h
    // gap, immune to CI jitter).
    const auto prev  = Clock::now() - kHour;
    const auto start = Clock::now();
    const auto fr    = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, prev, start, kPeriodUs);
    const auto comp  = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRateWithCompensation, prev, start, kPeriodUs);
    EXPECT_EQ(comp, prev + kPeriod);   // Compensation: exact catch-up
    EXPECT_GT(fr, comp)
        << "Compensation must NOT degrade to FixedRate's reset-from-now "
           "(2026-05-03 regression)";
}

TEST(LoopTimingComputeTest, NextDeadline_DeterministicBranches_TwoConsecutiveCallsIdentical)
{
    // Compensation and FixedRate-on-time do NOT read now(), so two back-to-back
    // calls with identical inputs return the EXACT same deadline — pinning that
    // those branches are clock-independent (unlike the overrun reset).
    const auto comp_prev = Clock::now() - kHour;
    const auto onT_prev  = Clock::now() + kHour;
    const auto start     = Clock::now();

    const auto c1 = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRateWithCompensation, comp_prev, start, kPeriodUs);
    const auto c2 = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRateWithCompensation, comp_prev, start, kPeriodUs);
    EXPECT_EQ(c1, c2) << "Compensation is clock-independent → identical across calls";

    const auto o1 = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, onT_prev, start, kPeriodUs);
    const auto o2 = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, onT_prev, start, kPeriodUs);
    EXPECT_EQ(o1, o2) << "FixedRate on-time is clock-independent → identical across calls";
}

TEST(LoopTimingComputeTest, NextDeadline_FixedRateOverrun_TwoConsecutiveCallsNonDecreasing)
{
    // The overrun branch DOES read now(), so two back-to-back calls are
    // non-decreasing (steady_clock is monotonic).  Use >= (not >) so the test
    // cannot flake if the clock does not advance between two very fast calls.
    const auto prev  = Clock::now() - kHour;
    const auto start = Clock::now();
    const auto r1    = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, prev, start, kPeriodUs);
    const auto r2    = pylabhub::compute_next_deadline(
        LoopTimingPolicy::FixedRate, prev, start, kPeriodUs);
    EXPECT_GE(r2, r1);
}
