/**
 * @file test_loop_timing_policy.cpp
 * @brief Unit tests for loop_timing_policy.hpp — shared utilities used by all three roles.
 *
 * Tests:
 *   - parse_loop_timing_policy(): valid values, cross-field constraints, error messages.
 *   - default_loop_timing_policy(): implicit derivation from target_period_ms.
 *   - compute_slot_acquire_timeout(): derivation table, boundary cases.
 */

#include "utils/loop_timing_policy.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

using pylabhub::LoopTimingPolicy;

// ============================================================================
// parse_loop_timing_policy
// ============================================================================

TEST(LoopTimingPolicyTest, Parse_MaxRate_Period0_OK)
{
    EXPECT_EQ(pylabhub::parse_loop_timing_policy("max_rate", 0, "test"),
              LoopTimingPolicy::MaxRate);
}

TEST(LoopTimingPolicyTest, Parse_FixedRate_PeriodPositive_OK)
{
    EXPECT_EQ(pylabhub::parse_loop_timing_policy("fixed_rate", 100, "test"),
              LoopTimingPolicy::FixedRate);
}

TEST(LoopTimingPolicyTest, Parse_FixedRateWithCompensation_PeriodPositive_OK)
{
    EXPECT_EQ(pylabhub::parse_loop_timing_policy("fixed_rate_with_compensation", 50, "test"),
              LoopTimingPolicy::FixedRateWithCompensation);
}

TEST(LoopTimingPolicyTest, Parse_MaxRate_PeriodPositive_Throws)
{
    EXPECT_THROW(pylabhub::parse_loop_timing_policy("max_rate", 100, "ctx"),
                 std::runtime_error);
}

TEST(LoopTimingPolicyTest, Parse_FixedRate_Period0_Throws)
{
    EXPECT_THROW(pylabhub::parse_loop_timing_policy("fixed_rate", 0, "ctx"),
                 std::runtime_error);
}

TEST(LoopTimingPolicyTest, Parse_FixedRateWithCompensation_Period0_Throws)
{
    EXPECT_THROW(pylabhub::parse_loop_timing_policy("fixed_rate_with_compensation", 0, "ctx"),
                 std::runtime_error);
}

TEST(LoopTimingPolicyTest, Parse_InvalidString_Throws)
{
    EXPECT_THROW(pylabhub::parse_loop_timing_policy("turbo", 0, "ctx"),
                 std::runtime_error);
}

// ============================================================================
// default_loop_timing_policy
// ============================================================================

TEST(LoopTimingPolicyTest, Default_Period0_MaxRate)
{
    EXPECT_EQ(pylabhub::default_loop_timing_policy(0), LoopTimingPolicy::MaxRate);
}

TEST(LoopTimingPolicyTest, Default_PeriodPositive_FixedRate)
{
    EXPECT_EQ(pylabhub::default_loop_timing_policy(100), LoopTimingPolicy::FixedRate);
    EXPECT_EQ(pylabhub::default_loop_timing_policy(1), LoopTimingPolicy::FixedRate);
}

// ============================================================================
// compute_slot_acquire_timeout
// ============================================================================

TEST(SlotAcquireTimeoutTest, Explicit_Zero_NonBlocking)
{
    // explicit=0 always returns 0 regardless of period.
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(0, 0), 0);
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(0, 100), 0);
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(0, 1), 0);
}

TEST(SlotAcquireTimeoutTest, Explicit_Positive_PassThrough)
{
    // explicit>0 returns that value directly.
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(100, 0), 100);
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(100, 200), 100);
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(5000, 0), 5000);
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(1, 1000), 1);
}

TEST(SlotAcquireTimeoutTest, Derive_PeriodPositive_HalfPeriod)
{
    // -1 + period>0 → max(period/2, 1)
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-1, 200), 100);
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-1, 100), 50);
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-1, 10), 5);
}

TEST(SlotAcquireTimeoutTest, Derive_PeriodSmall_MinClamp)
{
    // -1 + period=1 → max(1/2, 1) = max(0, 1) = 1
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-1, 1), 1);
    // -1 + period=2 → max(2/2, 1) = max(1, 1) = 1
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-1, 2), 1);
    // -1 + period=3 → max(3/2, 1) = max(1, 1) = 1
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-1, 3), 1);
}

TEST(SlotAcquireTimeoutTest, Derive_Period0_MaxRateDefault)
{
    // -1 + period=0 → 50 ms (kMaxRateDefaultMs)
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-1, 0), 50);
}

TEST(SlotAcquireTimeoutTest, Derive_NegativeExplicit_TreatedAsDerived)
{
    // Any negative value (not just -1) is treated as "derive".
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-2, 0), 50);   // MaxRate default
    EXPECT_EQ(pylabhub::compute_slot_acquire_timeout(-99, 200), 100); // period/2
}

TEST(SlotAcquireTimeoutTest, ResultAlwaysNonNegative)
{
    // Verify the contract: return value is always >= 0.
    for (int explicit_ms : {-1, 0, 1, 50, 5000})
    {
        for (int period_ms : {0, 1, 2, 10, 100, 1000})
        {
            EXPECT_GE(pylabhub::compute_slot_acquire_timeout(explicit_ms, period_ms), 0)
                << "explicit_ms=" << explicit_ms << " period_ms=" << period_ms;
        }
    }
}
