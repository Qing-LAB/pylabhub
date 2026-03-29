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

// ============================================================================
// default_loop_timing_policy (period_us: double)
// ============================================================================

TEST(LoopTimingPolicyTest, Default_Period0_MaxRate)
{
    EXPECT_EQ(pylabhub::default_loop_timing_policy(0.0), LoopTimingPolicy::MaxRate);
}

TEST(LoopTimingPolicyTest, Default_PeriodPositive_FixedRate)
{
    EXPECT_EQ(pylabhub::default_loop_timing_policy(100000.0), LoopTimingPolicy::FixedRate);
    EXPECT_EQ(pylabhub::default_loop_timing_policy(500.0), LoopTimingPolicy::FixedRate);
}
