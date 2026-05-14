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

#include "utils/zmq_poll_loop.hpp"  // pylabhub::scripting::PeriodicTask

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
    PeriodicTask task([&] { ++fire_count; }, 1000,
                      [&] { return iter; });

    iter = 1;
    task.tick();
    EXPECT_EQ(fire_count, 1);
}

TEST_F(PeriodicTaskTest, NoProgressMeansNoFire)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 1000,
                      [&] { return iter; });

    task.tick();
    EXPECT_EQ(fire_count, 0);

    task.tick();
    EXPECT_EQ(fire_count, 0);
}

TEST_F(PeriodicTaskTest, ProgressButIntervalNotElapsedSkips)
{
    int fire_count = 0;
    uint64_t iter = 0;
    PeriodicTask task([&] { ++fire_count; }, 60000,
                      [&] { return iter; });

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
    PeriodicTask task([&] { ++fire_count; }, 10,
                      [&] { return iter; });

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
    PeriodicTask task([&] { ++fire_count; }, 0,
                      [&] { return iter; });

    EXPECT_EQ(task.interval.count(), 2000);

    iter = 1;
    task.tick();
    EXPECT_EQ(fire_count, 1);
}

TEST_F(PeriodicTaskTest, NegativeIntervalUsesDefault)
{
    int fire_count = 0;
    PeriodicTask task([&] { ++fire_count; }, -5,
                      [] { return uint64_t{1}; });

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
    PeriodicTask task([&] { ++fire_count; }, 10,
                      [&] { return iter; });

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
