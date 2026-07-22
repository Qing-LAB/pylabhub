/**
 * @file test_periodic_task.cpp
 * @brief L1 unit tests for `pylabhub::scripting::PeriodicTask`.
 *
 * Subject: a stack-local struct with a `tick()` method.  Pure
 * function-level logic — gate by iteration counter callback +
 * elapsed-time interval; fire a callback when both conditions are
 * met.  No LOGGER_*, no lifecycle modules, no static state.
 *
 * Pattern 1 (PureApiTest) — no `LifecycleGuard`, no
 * `LogCaptureFixture`, no subprocess workers.  Each test
 * constructs a stack-local `PeriodicTask` and asserts on
 * counter state.
 *
 * Moved 2026-05-14 out of `tests/test_layer3_datahub/test_datahub_zmq_poll_loop.cpp`
 * (where it was historically grouped with ZmqPollLoop tests despite
 * being L1) to its proper L1 location.  The sibling `ZmqPollLoop`
 * tests — which DO touch Logger and require a lifecycle module —
 * live at L2 in `tests/test_layer2_service/test_zmq_poll_loop.cpp`.
 */

#include "utils/zmq_poll_loop.hpp" // pylabhub::scripting::PeriodicTask

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using pylabhub::scripting::PeriodicTask;

class PeriodicTaskTest : public ::testing::Test
{
};

TEST_F(PeriodicTaskTest, FirstTickFiresImmediatelyOnProgress)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 1000, [&] { return iter; });

    iter = 1;
    task.tick();
    EXPECT_EQ(fire_count, 1);
}

TEST_F(PeriodicTaskTest, NoProgressMeansNoFire)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 1000, [&] { return iter; });

    task.tick();
    EXPECT_EQ(fire_count, 0);

    task.tick();
    EXPECT_EQ(fire_count, 0);
}

TEST_F(PeriodicTaskTest, ProgressButIntervalNotElapsedSkips)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 60000, [&] { return iter; });

    iter = 1;
    task.tick();
    EXPECT_EQ(fire_count, 1);

    iter = 2;
    task.tick();
    EXPECT_EQ(fire_count, 1);
}

TEST_F(PeriodicTaskTest, FiresAgainAfterIntervalElapsed)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 10, [&] { return iter; });

    iter = 1;
    task.tick();
    EXPECT_EQ(fire_count, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{15});

    iter = 2;
    task.tick();
    EXPECT_EQ(fire_count, 2);
}

TEST_F(PeriodicTaskTest, DefaultIntervalUsedWhenZero)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 0, [&] { return iter; });

    EXPECT_EQ(task.interval.count(), 2000);

    iter = 1;
    task.tick();
    EXPECT_EQ(fire_count, 1);
}

TEST_F(PeriodicTaskTest, NegativeIntervalUsesDefault)
{
    int fire_count = 0;
    PeriodicTask task([&] { ++fire_count; }, -5, [] { return uint64_t{1}; });

    EXPECT_EQ(task.interval.count(), 2000);
}

TEST_F(PeriodicTaskTest, MultiplePeriodicTasks)
{
    int fire_a = 0, fire_b = 0;
    uint64_t iter = 0;
    PeriodicTask a([&] { ++fire_a; }, 10, [&] { return iter; });
    PeriodicTask b([&] { ++fire_b; }, 10, [&] { return iter; });

    iter = 1;
    a.tick();
    b.tick();
    EXPECT_EQ(fire_a, 1);
    EXPECT_EQ(fire_b, 1);

    a.tick();
    b.tick();
    EXPECT_EQ(fire_a, 1);
    EXPECT_EQ(fire_b, 1);
}

TEST_F(PeriodicTaskTest, LargeIterationJumpStillFires)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 10, [&] { return iter; });

    iter = 1000;
    task.tick();
    EXPECT_EQ(fire_count, 1);
}

TEST_F(PeriodicTaskTest, TimeOnlyMode_FiresWithoutIteration)
{
    int fire_count = 0;
    PeriodicTask task([&] { ++fire_count; }, 10);

    task.tick();
    EXPECT_EQ(fire_count, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    task.tick();
    EXPECT_EQ(fire_count, 2);
}

TEST_F(PeriodicTaskTest, TimeOnlyMode_IntervalNotElapsed)
{
    int fire_count = 0;
    PeriodicTask task([&] { ++fire_count; }, 60000);

    task.tick();
    EXPECT_EQ(fire_count, 1);

    task.tick();
    EXPECT_EQ(fire_count, 1);
}

// ============================================================================
// Coverage additions (2026-05-14) — pin `tick()` return-value contract
//
// The original 10 tests above assert `fire_count` side effects only.
// `tick()` ALSO returns `std::chrono::milliseconds` — "time until next
// fire" — which `ZmqPollLoop::run()` consumes (zmq_poll_loop.hpp:203)
// to compute the poll timeout.  A regression breaking this contract
// would silently degrade polling cadence in production (poll-spin or
// over-block) without affecting fire-count assertions.
//
// Each of `tick()`'s four reachable branches is pinned below:
//   - fire-now path returns `interval` exactly (iter-gated + time-only)
//   - non-fire path returns ~remaining (iter-stalled within interval,
//     time-only within interval)
//   - iter-stalled past-interval clamps to 0
// ============================================================================

TEST_F(PeriodicTaskTest, TickReturnsIntervalAfterFiring_IterGated)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 1000, [&] { return iter; });

    iter = 1;
    auto wait = task.tick();
    ASSERT_EQ(fire_count, 1);
    EXPECT_EQ(wait.count(), 1000) << "fire-now branch must return `interval` exactly (the value "
                                  << "consumed by ZmqPollLoop::run() to size its next poll wait)";
}

TEST_F(PeriodicTaskTest, TickReturnsIntervalAfterFiring_TimeOnly)
{
    int fire_count = 0;
    PeriodicTask task([&] { ++fire_count; }, 1000);

    auto wait = task.tick();
    ASSERT_EQ(fire_count, 1);
    EXPECT_EQ(wait.count(), 1000) << "time-only fire-now branch must return `interval` exactly";
}

TEST_F(PeriodicTaskTest, TickReturnsRemainingWhenIterStalled)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 1000, [&] { return iter; });

    // First tick advances iter from 0 → 1 and fires; last_fired = now.
    iter = 1;
    task.tick();
    ASSERT_EQ(fire_count, 1);

    // Second tick: iter unchanged → stalled path.  elapsed since fire
    // is ~microseconds → cast-to-ms = 0 → remaining = interval.
    auto wait = task.tick();
    EXPECT_EQ(fire_count, 1);
    // Tolerate a few ms of jitter (CI scheduler) but pin within
    // the interval — proves the stalled path computed remaining
    // correctly rather than returning 0 (no-time-left) or interval
    // (full reset).
    EXPECT_GE(wait.count(), 990);
    EXPECT_LE(wait.count(), 1000);
}

TEST_F(PeriodicTaskTest, TickReturnsRemainingWhenWithinInterval_TimeOnly)
{
    int fire_count = 0;
    PeriodicTask task([&] { ++fire_count; }, 1000);

    task.tick(); // first fire
    ASSERT_EQ(fire_count, 1);

    // Immediate re-tick: elapsed ~= 0, return ~= interval.
    auto wait = task.tick();
    EXPECT_EQ(fire_count, 1);
    EXPECT_GE(wait.count(), 990);
    EXPECT_LE(wait.count(), 1000);
}

TEST_F(PeriodicTaskTest, TickReturnsZeroWhenIterStalledPastInterval)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 10, [&] { return iter; });

    // Stall iter at 0; wait well past the 10 ms interval.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    auto wait = task.tick();
    EXPECT_EQ(fire_count, 0);
    EXPECT_EQ(wait.count(), 0) << "iter-stalled past-interval must clamp remaining to 0 "
                               << "(std::max with milliseconds{0}); otherwise ZmqPollLoop "
                               << "could be told to wait a negative duration";
}
