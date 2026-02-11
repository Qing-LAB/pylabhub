/**
 * @file test_backoff_strategy.cpp
 * @brief CRITICAL Layer 2 tests for backoff_strategy.hpp (BLOCKS DataBlock tests)
 *
 * Tests cover all four backoff strategies:
 * - ExponentialBackoff (3-phase: yield → 1us sleep → exponential)
 * - ConstantBackoff (fixed delay)
 * - NoBackoff (no-op for testing)
 * - AggressiveBackoff (quadratic growth)
 *
 * These strategies are used extensively in:
 * - DataBlock coordination (writer/reader acquisition)
 * - SharedSpinLock (cross-process locking)
 * - FileLock (advisory lock acquisition)
 * - MessageHub (connection retry)
 */
#include "plh_service.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace pylabhub::utils;
using namespace std::chrono_literals;

// ============================================================================
// Timing Measurement Helpers
// ============================================================================

/**
 * Measures the time taken by a backoff operation
 */
template <typename BackoffStrategy>
uint64_t measure_backoff_time_us(BackoffStrategy &strategy, int iteration)
{
    auto start = std::chrono::high_resolution_clock::now();
    strategy(iteration);
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

// ============================================================================
// ExponentialBackoff Tests
// ============================================================================

/**
 * Test ExponentialBackoff Phase 1: yield() for iterations 0-3
 */
TEST(BackoffStrategyTest, Exponential_Phase1_Yield)
{
    ExponentialBackoff backoff;

    for (int i = 0; i < 4; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);

        // Phase 1 should be very fast (just yield, no intentional sleep).
        // Allow up to 20ms: OS scheduler can delay even a yield() significantly.
        EXPECT_LT(time_us, 20000u) << "Iteration " << i << " took too long for Phase 1 (yield)";
    }
}

/**
 * Test ExponentialBackoff Phase 2: 1us sleep for iterations 4-9
 */
TEST(BackoffStrategyTest, Exponential_Phase2_Microsleep)
{
    ExponentialBackoff backoff;

    for (int i = 4; i < 10; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);

        // Phase 2 should sleep ~1us, but OS timer resolution may vary
        // Allow range: 0us (optimized out) to 5ms (slow system)
        EXPECT_LT(time_us, 5000u) << "Iteration " << i << " took too long for Phase 2 (1us sleep)";
    }
}

/**
 * Test ExponentialBackoff Phase 3: exponential growth (iteration * 10us)
 */
TEST(BackoffStrategyTest, Exponential_Phase3_ExponentialGrowth)
{
    ExponentialBackoff backoff;

    // Test specific iterations with known expected times
    struct TestCase
    {
        int iteration;
        uint64_t min_expected_us;
        uint64_t max_expected_us;
    };

    // Upper bounds are generous: OS scheduler adds 1-20ms jitter even for short sleeps.
    // The critical property is that Phase 3 actually sleeps (lower bound) and that
    // later iterations sleep longer than earlier ones (verified separately).
    std::vector<TestCase> test_cases = {
        {10, 5, 50000},    // ~10us minimum, allow up to 50ms for scheduler variance
        {20, 100, 50000},  // ~200us minimum
        {50, 250, 50000},  // ~500us minimum
        {100, 500, 50000}, // ~1000us minimum
    };

    for (const auto &tc : test_cases)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, tc.iteration);

        EXPECT_GE(time_us, tc.min_expected_us) << "Iteration " << tc.iteration << " was too fast";
        EXPECT_LE(time_us, tc.max_expected_us) << "Iteration " << tc.iteration << " was too slow";
    }
}

/**
 * Test ExponentialBackoff is monotonically increasing
 */
TEST(BackoffStrategyTest, Exponential_IsMonotonicallyIncreasing)
{
    // Take the minimum over several runs to suppress OS scheduler jitter.
    // sleep_for() sleeps AT LEAST the requested duration, so the minimum over
    // multiple runs converges to the actual sleep floor rather than scheduler-inflated values.
    auto min_time_us = [](int iter, int runs)
    {
        ExponentialBackoff b;
        uint64_t min = UINT64_MAX;
        for (int r = 0; r < runs; ++r)
        {
            uint64_t t = measure_backoff_time_us(b, iter);
            if (t < min)
                min = t;
        }
        return min;
    };

    // Compare iterations spaced far enough that the expected difference is large.
    // Phase 3 formula: sleep(iter * 10us)
    //   iter=10 -> 100us, iter=20 -> 200us, iter=50 -> 500us
    const int RUNS = 5;
    uint64_t t10 = min_time_us(10, RUNS);
    uint64_t t20 = min_time_us(20, RUNS);
    uint64_t t50 = min_time_us(50, RUNS);

    EXPECT_LT(t10, t20) << "Iteration 20 (200us) should sleep longer than iteration 10 (100us)";
    EXPECT_LT(t20, t50) << "Iteration 50 (500us) should sleep longer than iteration 20 (200us)";
}

/**
 * Test ExponentialBackoff helper function
 */
TEST(BackoffStrategyTest, Exponential_HelperFunction)
{
    // Test the convenience function backoff(iteration)
    auto start = std::chrono::high_resolution_clock::now();
    backoff(50); // Should sleep ~500us
    auto end = std::chrono::high_resolution_clock::now();

    uint64_t time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    EXPECT_GE(time_us, 250u) << "backoff(50) was too fast";
    EXPECT_LE(time_us, 50000u) << "backoff(50) was too slow";
}

// ============================================================================
// ConstantBackoff Tests
// ============================================================================

/**
 * Test ConstantBackoff with default delay (100us)
 */
TEST(BackoffStrategyTest, Constant_DefaultDelay)
{
    ConstantBackoff backoff; // Default: 100us

    uint64_t time_us = measure_backoff_time_us(backoff, 0);

    // Should sleep ~100us (allow OS timer variance)
    EXPECT_GE(time_us, 50u) << "ConstantBackoff default was too fast";
    EXPECT_LE(time_us, 5000u) << "ConstantBackoff default was too slow";
}

/**
 * Test ConstantBackoff with custom delay
 */
TEST(BackoffStrategyTest, Constant_CustomDelay)
{
    ConstantBackoff backoff(200us);

    uint64_t time_us = measure_backoff_time_us(backoff, 0);

    // Should sleep ~200us
    EXPECT_GE(time_us, 100u) << "ConstantBackoff(200us) was too fast";
    EXPECT_LE(time_us, 5000u) << "ConstantBackoff(200us) was too slow";
}

/**
 * Test ConstantBackoff is iteration-independent
 */
TEST(BackoffStrategyTest, Constant_IterationIndependent)
{
    ConstantBackoff backoff(100us);

    // Verify each measurement individually against a wide range rather than comparing
    // measurements to each other. OS scheduler jitter (1-10ms) makes max/min ratio
    // comparisons unreliable, but each call MUST sleep at least ~50us and should not
    // take longer than 20ms on any non-pathological system.
    for (int i = 0; i < 10; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);

        EXPECT_GE(time_us, 50u) << "ConstantBackoff(100us) should sleep at least 50us (iteration "
                                << i << ")";
        EXPECT_LE(time_us, 20000u)
            << "ConstantBackoff(100us) should not exceed 20ms (iteration " << i << ")";
    }
}

// ============================================================================
// NoBackoff Tests
// ============================================================================

/**
 * Test NoBackoff is a true no-op (zero time)
 */
TEST(BackoffStrategyTest, NoBackoff_IsNoOp)
{
    NoBackoff backoff;

    // NoBackoff should be extremely fast (< 10us for overhead)
    for (int i = 0; i < 100; i += 10)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);

        EXPECT_LT(time_us, 10u) << "NoBackoff should have near-zero overhead";
    }
}

/**
 * Test NoBackoff ignores iteration count
 */
TEST(BackoffStrategyTest, NoBackoff_IgnoresIteration)
{
    NoBackoff backoff;

    uint64_t time_small = measure_backoff_time_us(backoff, 1);
    uint64_t time_large = measure_backoff_time_us(backoff, 1000);

    // Both should be fast (no difference based on iteration)
    EXPECT_LT(time_small, 10u);
    EXPECT_LT(time_large, 10u);
}

// ============================================================================
// AggressiveBackoff Tests
// ============================================================================

/**
 * Test AggressiveBackoff Phase 1: yield for iterations 0-1
 */
TEST(BackoffStrategyTest, Aggressive_Phase1_Yield)
{
    AggressiveBackoff backoff;

    for (int i = 0; i < 2; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        // Allow up to 20ms: OS scheduler can delay even a yield() significantly.
        EXPECT_LT(time_us, 20000u) << "AggressiveBackoff Phase 1 (yield) took too long";
    }
}

/**
 * Test AggressiveBackoff Phase 2: 10us sleep for iterations 2-5
 */
TEST(BackoffStrategyTest, Aggressive_Phase2_ShortSleep)
{
    AggressiveBackoff backoff;

    for (int i = 2; i < 6; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        EXPECT_LT(time_us, 5000u) << "AggressiveBackoff Phase 2 took too long";
    }
}

/**
 * Test AggressiveBackoff Phase 3: quadratic growth (iteration^2 * 10us)
 */
TEST(BackoffStrategyTest, Aggressive_Phase3_QuadraticGrowth)
{
    AggressiveBackoff backoff;

    struct TestCase
    {
        int iteration;
        uint64_t min_expected_us;
        uint64_t max_expected_us;
    };

    // Upper bounds are generous: OS scheduler adds 1-20ms jitter even for short sleeps.
    // The critical property is the lower bound (the sleep actually occurs) and that
    // later iterations sleep longer than earlier ones (verified separately).
    std::vector<TestCase> test_cases = {
        {6, 100, 50000},   // 360us minimum; allow up to 50ms for scheduler variance
        {10, 500, 50000},  // 1000us minimum
        {20, 2000, 50000}, // 4000us minimum
    };

    for (const auto &tc : test_cases)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, tc.iteration);

        EXPECT_GE(time_us, tc.min_expected_us)
            << "AggressiveBackoff iteration " << tc.iteration << " was too fast";
        EXPECT_LE(time_us, tc.max_expected_us)
            << "AggressiveBackoff iteration " << tc.iteration << " was too slow";
    }
}

/**
 * Test AggressiveBackoff cap at 100ms
 */
TEST(BackoffStrategyTest, Aggressive_HasMaxCap)
{
    AggressiveBackoff backoff;

    // Test a very large iteration (should be capped at 100ms)
    uint64_t time_us = measure_backoff_time_us(backoff, 1000);

    // Should be capped at 100ms (allow some overhead)
    EXPECT_LE(time_us, 150'000u) << "AggressiveBackoff should cap at 100ms";
}

// ============================================================================
// Comparison Tests
// ============================================================================

/**
 * Test ExponentialBackoff vs AggressiveBackoff growth rates
 */
TEST(BackoffStrategyTest, Comparison_ExponentialVsAggressive)
{
    // Take minimum over several runs to suppress OS scheduler jitter.
    // sleep_for() sleeps at least the requested duration, so minimums converge
    // toward the actual sleep floor rather than scheduler-inflated values.
    const int RUNS = 3;
    uint64_t exp_min = UINT64_MAX, agg_min = UINT64_MAX;
    for (int r = 0; r < RUNS; ++r)
    {
        ExponentialBackoff eb;
        AggressiveBackoff ab;
        uint64_t et = measure_backoff_time_us(eb, 20);
        uint64_t at = measure_backoff_time_us(ab, 20);
        if (et < exp_min)
            exp_min = et;
        if (at < agg_min)
            agg_min = at;
    }

    // Aggressive: 20^2 * 10 = 4000us
    // Exponential: 20 * 10 = 200us
    // Minimum measurements should reflect this 20x ratio reliably.
    EXPECT_GT(agg_min, exp_min) << "AggressiveBackoff(iter=20, ~4000us) should sleep longer than "
                                   "ExponentialBackoff(iter=20, ~200us)";
}

/**
 * Test NoBackoff is significantly faster than ConstantBackoff
 */
TEST(BackoffStrategyTest, Comparison_NoBackoffVsConstant)
{
    NoBackoff no_backoff;
    ConstantBackoff const_backoff(100us);

    uint64_t no_time = measure_backoff_time_us(no_backoff, 0);
    uint64_t const_time = measure_backoff_time_us(const_backoff, 0);

    // NoBackoff should be at least 10x faster
    EXPECT_LT(no_time * 10, const_time)
        << "NoBackoff should be significantly faster than ConstantBackoff";
}

// ============================================================================
// Usage Pattern Tests (as used in DataBlock)
// ============================================================================

/**
 * Test typical DataBlock usage: retry loop with exponential backoff
 */
TEST(BackoffStrategyTest, UsagePattern_RetryLoop)
{
    ExponentialBackoff backoff;
    int max_iterations = 20;
    bool success = false;
    int iteration = 0;

    // Simulate a retry loop (like DataBlock writer acquisition)
    auto start = std::chrono::high_resolution_clock::now();

    while (!success && iteration < max_iterations)
    {
        // Simulate work (would be CAS attempt in real code)
        if (iteration == 10)
        {
            success = true; // Succeed on 10th iteration
        }

        if (!success)
        {
            backoff(iteration++);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    uint64_t total_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    EXPECT_TRUE(success) << "Retry loop should eventually succeed";
    EXPECT_EQ(iteration, 10) << "Should succeed on 10th iteration";

    // Total time should be sum of backoffs for iterations 0-9
    // Approximate: Phase 1 (0-3): ~0, Phase 2 (4-9): ~6us, Phase 3 (10): none
    // Total should be < 50ms (generous upper bound)
    EXPECT_LT(total_time_us, 50'000u) << "Retry loop took too long";
}

/**
 * Test NoBackoff for fast unit tests (as intended)
 */
TEST(BackoffStrategyTest, UsagePattern_FastTests)
{
    NoBackoff backoff;
    int iterations = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        backoff(i);
    }

    auto end = std::chrono::high_resolution_clock::now();
    uint64_t total_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    // 1000 iterations of NoBackoff should be extremely fast (< 1ms)
    EXPECT_LT(total_time_us, 1000u) << "NoBackoff should allow very fast test execution";
}
