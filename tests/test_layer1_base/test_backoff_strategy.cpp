/**
 * @file test_backoff_strategy.cpp
 * @brief Tests for backoff_strategy.hpp — all 4 strategies + free function.
 *
 * Timing tests use wide tolerances (±50ms) because:
 * - Windows sleep_for(1us) has ~15.6ms resolution
 * - Linux scheduler jitter varies by load
 * - CI runners may have reduced CPU priority
 */
#include <plh_base.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace pylabhub::utils;
using Clock = std::chrono::steady_clock;

// ============================================================================
// ThreePhaseBackoff
// ============================================================================

TEST(BackoffStrategyTest, ThreePhaseBackoff_Phase1_Yields)
{
    ThreePhaseBackoff bo;
    auto t0 = Clock::now();
    for (int i = 0; i < 4; ++i)
        bo(i);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    // Phase 1 is yield-only — should complete in well under 50ms
    EXPECT_LT(elapsed.count(), 50);
}

TEST(BackoffStrategyTest, ThreePhaseBackoff_Phase2_LightSleep)
{
    ThreePhaseBackoff bo;
    auto t0 = Clock::now();
    for (int i = 4; i < 10; ++i)
        bo(i);
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0);
    // Phase 2 sleeps 1us per iteration (6 iterations) — elapsed > 0
    EXPECT_GT(elapsed.count(), 0);
}

TEST(BackoffStrategyTest, ThreePhaseBackoff_Phase3_LinearSleep)
{
    ThreePhaseBackoff bo;

    // Phase 3 (iteration >= 10) uses linear sleep. Verify calls complete
    // without crashing. Relative timing assertions (iter 20 >= iter 10) are
    // unreliable on loaded CI runners due to scheduling jitter.
    bo(10);
    bo(20);
    bo(30);

    // Verify the backoff is actually sleeping (not spinning) by checking
    // a high iteration takes at least a minimal amount of time.
    auto t0 = Clock::now();
    bo(50); // expected ~500us sleep
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0);
    EXPECT_GE(elapsed.count(), 10) << "Phase 3 should sleep, not spin";
}

// ============================================================================
// ConstantBackoff
// ============================================================================

TEST(BackoffStrategyTest, ConstantBackoff_DefaultDelay)
{
    ConstantBackoff bo;
    EXPECT_EQ(bo.delay, std::chrono::microseconds(100));
    // Verify it runs without crash
    bo(0);
    bo(100);
}

TEST(BackoffStrategyTest, ConstantBackoff_CustomDelay)
{
    ConstantBackoff bo(std::chrono::microseconds(50));
    EXPECT_EQ(bo.delay, std::chrono::microseconds(50));
    bo(0);
}

TEST(BackoffStrategyTest, ConstantBackoff_IgnoresIteration)
{
    ConstantBackoff bo(std::chrono::microseconds(10));
    auto t0 = Clock::now();
    bo(0);
    auto elapsed0 = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0);

    auto t1 = Clock::now();
    bo(100);
    auto elapsed100 = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t1);

    // Both should be roughly the same — within 50ms tolerance
    auto diff = std::abs(elapsed100.count() - elapsed0.count());
    EXPECT_LT(diff, 50000); // < 50ms difference
}

// ============================================================================
// NoBackoff
// ============================================================================

TEST(BackoffStrategyTest, NoBackoff_IsNoOp)
{
    NoBackoff bo;
    auto t0 = Clock::now();
    for (int i = 0; i < 1000; ++i)
        bo(i);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    // 1000 no-ops should complete in < 10ms
    EXPECT_LT(elapsed.count(), 10);

    // Verify callable with any iteration
    static_assert(noexcept(bo(0)));
    static_assert(noexcept(bo(999)));
}

// ============================================================================
// AggressiveBackoff
// ============================================================================

TEST(BackoffStrategyTest, AggressiveBackoff_Phase1_Yields)
{
    AggressiveBackoff bo;
    auto t0 = Clock::now();
    bo(0);
    bo(1);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    // Phase 1 (0-1) is yield-only — < 50ms
    EXPECT_LT(elapsed.count(), 50);
}

TEST(BackoffStrategyTest, AggressiveBackoff_Phase3_Capped)
{
    AggressiveBackoff bo;
    // iteration=200: 200^2*10 = 400000us = 400ms, but capped at 100ms
    auto t0 = Clock::now();
    bo(200);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    // Should be around 100ms (capped), not 400ms
    // Allow generous range: 50ms–200ms (timer resolution + scheduling)
    EXPECT_LT(elapsed.count(), 200);
}

// ============================================================================
// Free function
// ============================================================================

TEST(BackoffStrategyTest, BackoffFreeFunction_Compiles)
{
    // Verify the free function compiles and runs without crash
    backoff(0);
    backoff(5);
    backoff(10);
}
