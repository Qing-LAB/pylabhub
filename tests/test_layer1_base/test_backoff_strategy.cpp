/**
 * @file test_backoff_strategy.cpp
 * @brief Layer 1 tests for backoff_strategy.hpp — all 4 strategies + free function.
 *
 * BackoffStrategy types live in `pylabhub::utils` (header-only, layer-1
 * utility consumed by DataBlock coordination, SharedSpinLock, FileLock,
 * and MessageHub).  Tests include the utility header directly so the
 * dependency is explicit.
 *
 * Coverage:
 *   - ThreePhaseBackoff (3-phase: yield → 1us sleep → linear)
 *   - ConstantBackoff (fixed delay)
 *   - NoBackoff (no-op for testing)
 *   - AggressiveBackoff (quadratic growth)
 *   - `backoff(iteration)` free function (compile + run smoke)
 *
 * Timing tests use wide tolerances and minimum-over-N-runs aggregation
 * to suppress OS scheduler jitter:
 *   - Windows sleep_for(1us) has ~15.6ms resolution
 *   - Linux scheduler jitter varies by load
 *   - CI runners may have reduced CPU priority
 * Upper bounds are explicitly relaxed under PYLABHUB_CI_BUILD via
 * `ci_upper()`; lower bounds (the "did the sleep actually happen"
 * contract) are unaffected.
 *
 * History: this file consolidates the previous L1 + L2 backoff_strategy
 * test files (the L2 one was a more elaborate parallel copy with one
 * colliding test name — `NoBackoff_IsNoOp`).  BackoffStrategy is a
 * layer-1 utility; its tests belong here, not duplicated at L2.
 */
#include "utils/backoff_strategy.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

using namespace pylabhub::utils;
using namespace std::chrono_literals;

// CI-relaxation factor applied to upper-bound timing assertions.  Loaded
// CI runners (GitHub Actions, etc.) routinely interrupt short sleeps and
// yields with milliseconds of scheduler delay — a tight "< 10us" upper
// bound that's correct on a developer workstation can fail with a 5ms
// schedule slot on a noisy runner.  Lower bounds are unaffected — relaxed
// asymmetrically only when needed and explicitly noted at each site.
//
// PYLABHUB_CI_BUILD is set by tests/CMakeLists.txt when the build is
// configured under a CI environment (CI=1, GITHUB_ACTIONS=1, etc.).
#ifdef PYLABHUB_CI_BUILD
constexpr uint64_t kCiUpperBoundMul = 10;
#else
constexpr uint64_t kCiUpperBoundMul = 1;
#endif

constexpr uint64_t ci_upper(uint64_t us) noexcept
{
    return us * kCiUpperBoundMul;
}

// ============================================================================
// Timing Measurement Helpers
// ============================================================================

template <typename BackoffStrategy>
uint64_t measure_backoff_time_us(BackoffStrategy &strategy, int iteration)
{
    auto start = std::chrono::high_resolution_clock::now();
    strategy(iteration);
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

// ============================================================================
// ThreePhaseBackoff Tests
// ============================================================================

TEST(BackoffStrategyTest, ThreePhase_Phase1_Yield)
{
    ThreePhaseBackoff backoff;
    for (int i = 0; i < 4; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        // Phase 1 should be very fast (just yield, no intentional sleep).
        // Allow up to 20ms baseline; CI runners can spike under load —
        // ci_upper() applies the 10x relaxation when PYLABHUB_CI_BUILD
        // is set (auto-detected from CI=1 / GITHUB_ACTIONS=1 in
        // tests/CMakeLists.txt).
        EXPECT_LT(time_us, ci_upper(20000u))
            << "Iteration " << i << " too long for Phase 1 (yield)";
    }
}

TEST(BackoffStrategyTest, ThreePhase_Phase2_Microsleep)
{
    ThreePhaseBackoff backoff;
    for (int i = 4; i < 10; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        // Phase 2 should sleep ~1us; OS timer resolution may vary.
        // ci_upper() relaxes the upper bound 10x under CI to absorb
        // scheduler stalls (a 5ms baseline becomes 50ms on CI).
        // Flaky-failure report 2026-05-21 motivated this.
        EXPECT_LT(time_us, ci_upper(5000u))
            << "Iteration " << i << " too long for Phase 2 (1us sleep)";
    }
}

TEST(BackoffStrategyTest, ThreePhase_Phase3_LinearGrowth)
{
    ThreePhaseBackoff backoff;
    struct TestCase
    {
        int iteration;
        uint64_t min_expected_us;
        uint64_t max_expected_us;
    };
    // Upper bounds generous: OS scheduler adds 1-20ms jitter even for short sleeps.
    // Critical property is the lower bound (sleep actually occurs) and that later
    // iterations sleep longer than earlier ones (verified separately).
    std::vector<TestCase> test_cases = {
        {10, 5, 50000},    // ~10us minimum, allow up to 50ms for scheduler variance
        {20, 100, 50000},  // ~200us minimum
        {50, 250, 50000},  // ~500us minimum
        {100, 500, 50000}, // ~1000us minimum
    };
    for (const auto &tc : test_cases)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, tc.iteration);
        EXPECT_GE(time_us, tc.min_expected_us) << "Iteration " << tc.iteration << " too fast";
        EXPECT_LE(time_us, tc.max_expected_us) << "Iteration " << tc.iteration << " too slow";
    }
}

TEST(BackoffStrategyTest, ThreePhase_IsMonotonicallyIncreasing)
{
    // Minimum over several runs suppresses OS scheduler jitter.
    // sleep_for() sleeps AT LEAST the requested duration, so the minimum
    // converges to the actual sleep floor rather than scheduler-inflated values.
    auto min_time_us = [](int iter, int runs)
    {
        ThreePhaseBackoff b;
        uint64_t min = UINT64_MAX;
        for (int r = 0; r < runs; ++r)
        {
            uint64_t t = measure_backoff_time_us(b, iter);
            if (t < min)
                min = t;
        }
        return min;
    };

    // Warmup: first precise_sleep call initializes a thread-local timer handle
    // on Windows.  Pay that cost once with a throwaway iteration.
    {
        ThreePhaseBackoff warmup;
        warmup(20);
    }

    const int RUNS = 10;
    uint64_t t50 = min_time_us(50, RUNS);
    uint64_t t200 = min_time_us(200, RUNS);
    uint64_t t500 = min_time_us(500, RUNS);

    EXPECT_LT(t50, t200) << "iter=200 (~2000us) should sleep longer than iter=50 (~500us)";
    EXPECT_LT(t200, t500) << "iter=500 (~5000us) should sleep longer than iter=200 (~2000us)";
}

TEST(BackoffStrategyTest, ThreePhase_HelperFunction)
{
    auto start = std::chrono::high_resolution_clock::now();
    backoff(50); // Should sleep ~500us via the convenience function
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    EXPECT_GE(time_us, 250u) << "backoff(50) too fast";
    EXPECT_LE(time_us, 50000u) << "backoff(50) too slow";
}

// `backoff` free function compile-test — pins the free-function form
// of the helper exists and accepts iteration counts at boundary values.
// Mutation: rename the free function in backoff_strategy.hpp → this
// fails to compile.
TEST(BackoffStrategyTest, BackoffFreeFunction_Compiles)
{
    backoff(0);
    backoff(5);
    backoff(10);
}

// ============================================================================
// ConstantBackoff Tests
// ============================================================================

TEST(BackoffStrategyTest, Constant_DefaultDelay)
{
    ConstantBackoff backoff; // Default: 100us
    uint64_t time_us = measure_backoff_time_us(backoff, 0);
    EXPECT_GE(time_us, 50u) << "ConstantBackoff default too fast";
    EXPECT_LE(time_us, 5000u) << "ConstantBackoff default too slow";
}

TEST(BackoffStrategyTest, Constant_CustomDelay)
{
    ConstantBackoff backoff(200us);
    uint64_t time_us = measure_backoff_time_us(backoff, 0);
    EXPECT_GE(time_us, 100u) << "ConstantBackoff(200us) too fast";
    EXPECT_LE(time_us, 5000u) << "ConstantBackoff(200us) too slow";
}

TEST(BackoffStrategyTest, Constant_IterationIndependent)
{
    ConstantBackoff backoff(100us);
    // Verify each measurement against a wide range rather than comparing
    // measurements to each other.  OS jitter (1-10ms) makes max/min ratios
    // unreliable, but each call MUST sleep ≥50us and should not exceed 20ms.
    for (int i = 0; i < 10; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        EXPECT_GE(time_us, 50u) << "ConstantBackoff(100us) should sleep ≥50us (iter=" << i << ")";
        EXPECT_LE(time_us, 20000u)
            << "ConstantBackoff(100us) should not exceed 20ms (iter=" << i << ")";
    }
}

// ============================================================================
// NoBackoff Tests
// ============================================================================

TEST(BackoffStrategyTest, NoBackoff_IsNoOp)
{
    NoBackoff backoff;
    // NoBackoff should be extremely fast (< 10us overhead) on a quiet
    // workstation; relaxed to ci_upper(10us) on CI runners where a single
    // scheduler preemption between start/end timestamps can blow past 10us.
    for (int i = 0; i < 100; i += 10)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        EXPECT_LT(time_us, ci_upper(10u)) << "NoBackoff should have near-zero overhead";
    }
}

TEST(BackoffStrategyTest, NoBackoff_IgnoresIteration)
{
    NoBackoff backoff;
    uint64_t time_small = measure_backoff_time_us(backoff, 1);
    uint64_t time_large = measure_backoff_time_us(backoff, 1000);
    EXPECT_LT(time_small, ci_upper(10u));
    EXPECT_LT(time_large, ci_upper(10u));
}

// ============================================================================
// AggressiveBackoff Tests
// ============================================================================

TEST(BackoffStrategyTest, Aggressive_Phase1_Yield)
{
    AggressiveBackoff backoff;
    for (int i = 0; i < 2; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        EXPECT_LT(time_us, 20000u) << "AggressiveBackoff Phase 1 (yield) took too long";
    }
}

TEST(BackoffStrategyTest, Aggressive_Phase2_ShortSleep)
{
    AggressiveBackoff backoff;
    for (int i = 2; i < 6; ++i)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, i);
        EXPECT_LT(time_us, 5000u) << "AggressiveBackoff Phase 2 took too long";
    }
}

TEST(BackoffStrategyTest, Aggressive_Phase3_QuadraticGrowth)
{
    AggressiveBackoff backoff;
    struct TestCase
    {
        int iteration;
        uint64_t min_expected_us;
        uint64_t max_expected_us;
    };
    std::vector<TestCase> test_cases = {
        {6, 100, 50000},   // 360us minimum; allow up to 50ms for scheduler variance
        {10, 500, 50000},  // 1000us minimum
        {20, 2000, 50000}, // 4000us minimum
    };
    for (const auto &tc : test_cases)
    {
        uint64_t time_us = measure_backoff_time_us(backoff, tc.iteration);
        EXPECT_GE(time_us, tc.min_expected_us)
            << "AggressiveBackoff iter=" << tc.iteration << " too fast";
        EXPECT_LE(time_us, tc.max_expected_us)
            << "AggressiveBackoff iter=" << tc.iteration << " too slow";
    }
}

TEST(BackoffStrategyTest, Aggressive_HasMaxCap)
{
    AggressiveBackoff backoff;
    uint64_t time_us = measure_backoff_time_us(backoff, 1000);
    EXPECT_LE(time_us, 150'000u) << "AggressiveBackoff should cap at ~100ms";
}

// ============================================================================
// Comparison Tests
// ============================================================================

TEST(BackoffStrategyTest, Comparison_ThreePhaseVsAggressive)
{
    const int RUNS = 3;
    uint64_t exp_min = UINT64_MAX, agg_min = UINT64_MAX;
    for (int r = 0; r < RUNS; ++r)
    {
        ThreePhaseBackoff eb;
        AggressiveBackoff ab;
        uint64_t et = measure_backoff_time_us(eb, 20);
        uint64_t at = measure_backoff_time_us(ab, 20);
        if (et < exp_min)
            exp_min = et;
        if (at < agg_min)
            agg_min = at;
    }
    // Aggressive: 20^2 * 10 = 4000us; ThreePhase: 20 * 10 = 200us.
    EXPECT_GT(agg_min, exp_min) << "AggressiveBackoff(iter=20, ~4000us) should sleep longer than "
                                   "ThreePhaseBackoff(iter=20, ~200us)";
}

TEST(BackoffStrategyTest, Comparison_NoBackoffVsConstant)
{
    NoBackoff no_backoff;
    ConstantBackoff const_backoff(100us);
    // Minimum over several runs suppresses scheduler jitter on the no-backoff
    // side: under CI a stray preempt can blow up a single no-op measurement
    // to milliseconds, breaking the relative comparison.
    constexpr int kRuns = 5;
    uint64_t no_min = UINT64_MAX, const_min = UINT64_MAX;
    for (int r = 0; r < kRuns; ++r)
    {
        uint64_t no_t = measure_backoff_time_us(no_backoff, 0);
        uint64_t const_t = measure_backoff_time_us(const_backoff, 0);
        if (no_t < no_min)
            no_min = no_t;
        if (const_t < const_min)
            const_min = const_t;
    }
    // Under CI we relax 10x → 3x: a runner slow enough to give
    // ConstantBackoff(100us) a 5000us reading is also slow enough to
    // stretch a NoBackoff measurement to ~500us.
#ifdef PYLABHUB_CI_BUILD
    constexpr uint64_t kRatio = 3;
#else
    constexpr uint64_t kRatio = 10;
#endif
    EXPECT_LT(no_min * kRatio, const_min)
        << "NoBackoff should be significantly faster than ConstantBackoff "
           "(no_min="
        << no_min << "us, const_min=" << const_min << "us, required ratio=" << kRatio << "x)";
}

// ============================================================================
// Usage Pattern Tests (as used in DataBlock)
// ============================================================================

TEST(BackoffStrategyTest, UsagePattern_RetryLoop)
{
    ThreePhaseBackoff backoff;
    int max_iterations = 20;
    bool success = false;
    int iteration = 0;
    auto start = std::chrono::high_resolution_clock::now();
    while (!success && iteration < max_iterations)
    {
        if (iteration == 10)
            success = true;
        if (!success)
            backoff(iteration++);
    }
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t total_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    EXPECT_TRUE(success) << "Retry loop should eventually succeed";
    EXPECT_EQ(iteration, 10) << "Should succeed on 10th iteration";
    EXPECT_LT(total_time_us, 50'000u) << "Retry loop took too long";
}

TEST(BackoffStrategyTest, UsagePattern_FastTests)
{
    NoBackoff backoff;
    int iterations = 1000;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i)
        backoff(i);
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t total_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    // 1000 iterations of NoBackoff should be extremely fast (< 1ms on a quiet
    // workstation).  Relax to ci_upper(1ms) for CI runners.
    EXPECT_LT(total_time_us, ci_upper(1000u)) << "NoBackoff should allow very fast test execution";
}
