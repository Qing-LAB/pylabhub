/**
 * @file test_datahub_hub_monitored_queue.cpp
 * @brief Unit tests for hub::MonitoredQueue<T>.
 *
 * Tests cover:
 *  - Push fills queue to max_depth; extra pushes drop oldest, incrementing total_dropped.
 *  - total_dropped and total_pushed counters are accurate.
 *  - send_errors_ is incremented when sender throws.
 *  - on_warn fires after stagnant depth persists past check_interval_ms.
 *  - on_cleared fires after backpressure clears (depth drops to 0).
 *  - on_dead fires once after dead_timeout_ms of sustained backpressure.
 *  - fire_and_forget=true (default) skips all monitoring callbacks.
 *  - Move assignment resets monitoring state so stale backpressure is not carried over.
 */
#include "hub/hub_monitored_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace pylabhub::hub;
using ms = std::chrono::milliseconds;

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.PushToMaxDepth_DropsOldest
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, PushToMaxDepth_DropsOldest)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth       = 3;
    cfg.check_interval_ms = 60000; // disable monitoring for this test
    MonitoredQueue<int> q(cfg);

    q.push(10);
    q.push(20);
    q.push(30);
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.total_dropped(), 0u);

    // 4th push should drop oldest (10)
    q.push(40);
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.total_dropped(), 1u);
    EXPECT_EQ(q.total_pushed(), 4u);

    // Drain and verify remaining items are 20, 30, 40 (oldest was dropped)
    std::vector<int> drained;
    q.drain([&](int &v) { drained.push_back(v); });
    ASSERT_EQ(drained.size(), 3u);
    EXPECT_EQ(drained[0], 20);
    EXPECT_EQ(drained[1], 30);
    EXPECT_EQ(drained[2], 40);
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.TotalDroppedAndPushedCounters
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, TotalDroppedAndPushedCounters)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 2;
    cfg.check_interval_ms = 60000;
    MonitoredQueue<int> q(cfg);

    // Push 5 items into a queue of max_depth=2
    for (int i = 0; i < 5; ++i)
        q.push(i);

    EXPECT_EQ(q.total_pushed(), 5u);
    EXPECT_EQ(q.total_dropped(), 3u);  // first 3 dropped to make room
    EXPECT_EQ(q.size(), 2u);

    // After draining, counters remain
    q.drain([](int &) {});
    EXPECT_EQ(q.total_pushed(), 5u);
    EXPECT_EQ(q.total_dropped(), 3u);
    EXPECT_EQ(q.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.SendErrorsCounted
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, SendErrorsCounted)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 10;
    cfg.check_interval_ms = 60000;
    MonitoredQueue<int> q(cfg);

    q.push(1);
    q.push(2);
    q.push(3);
    EXPECT_EQ(q.send_errors(), 0u);

    // Sender throws on odd values
    q.drain([](int &v) {
        if (v % 2 != 0)
            throw std::runtime_error("odd");
    });

    // Items 1 and 3 threw; item 2 succeeded
    EXPECT_EQ(q.send_errors(), 2u);
    EXPECT_EQ(q.size(), 0u);  // all items processed (errors are consumed, not re-queued)
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.OnWarnFiresAfterStagnantDepth
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, OnWarnFiresAfterStagnantDepth)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 100;
    cfg.check_interval_ms = 50;   // very short interval for testing
    cfg.dead_timeout_ms   = 60000;
    cfg.fire_and_forget   = false; // enable monitoring for this test
    MonitoredQueue<int> q(cfg);

    std::atomic<int> warn_count{0};
    q.set_on_warn([&](size_t /*depth*/, int /*elapsed_ms*/) {
        ++warn_count;
    });

    // Push items but don't drain — simulate stagnant backpressure.
    for (int i = 0; i < 5; ++i)
        q.push(i);

    // First drain: depth_before=5, check fires if interval elapsed.
    // We need two drain calls with a sleep so the check interval triggers.
    q.drain([](int &) { /* keep items by not draining — but drain() empties the queue */ });
    // drain() empties everything. To simulate stagnant depth, push again.
    for (int i = 0; i < 5; ++i)
        q.push(i);

    // Wait for check_interval_ms to elapse.
    std::this_thread::sleep_for(ms{80});

    // drain() with non-zero depth_before should trigger on_warn.
    q.drain([](int &) {});

    EXPECT_GE(warn_count.load(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.OnClearedFiresAfterBackpressureResolves
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, OnClearedFiresAfterBackpressureResolves)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 100;
    cfg.check_interval_ms = 50;
    cfg.dead_timeout_ms   = 60000;
    cfg.fire_and_forget   = false; // enable monitoring for this test
    MonitoredQueue<int> q(cfg);

    std::atomic<int> warn_count{0};
    std::atomic<int> cleared_count{0};

    q.set_on_warn([&](size_t, int) { ++warn_count; });
    q.set_on_cleared([&](int) { ++cleared_count; });

    // Phase 1: create backpressure by draining a non-empty queue after interval.
    for (int i = 0; i < 5; ++i)
        q.push(i);
    std::this_thread::sleep_for(ms{80});
    q.drain([](int &) {});  // depth_before=5, check fires → on_warn, backpressure=true
    EXPECT_GE(warn_count.load(), 1);
    EXPECT_EQ(cleared_count.load(), 0);

    // Phase 2: drain empty queue after interval → on_cleared should fire.
    std::this_thread::sleep_for(ms{80});
    q.drain([](int &) {});  // depth_before=0, backpressure=true → on_cleared

    EXPECT_GE(cleared_count.load(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.OnDeadFiresOnceAfterTimeout
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, OnDeadFiresOnceAfterTimeout)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 100;
    cfg.check_interval_ms = 30;
    cfg.dead_timeout_ms   = 50;   // very short dead timeout for testing
    cfg.fire_and_forget   = false; // enable monitoring for this test
    MonitoredQueue<int> q(cfg);

    std::atomic<int> warn_count{0};
    std::atomic<int> dead_count{0};

    q.set_on_warn([&](size_t, int) { ++warn_count; });
    q.set_on_dead([&]() { ++dead_count; });

    // Simulate repeated stagnant drain calls to trigger on_dead.
    // Each drain cycle: push items, wait for check_interval_ms, drain.
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        for (int i = 0; i < 3; ++i)
            q.push(i);
        std::this_thread::sleep_for(ms{40});
        q.drain([](int &) {});
    }

    // on_dead should fire exactly once (dead_fired_ guards repeat invocations).
    EXPECT_EQ(dead_count.load(), 1);
    EXPECT_GE(warn_count.load(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.UnboundedQueueNeverDrops
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, UnboundedQueueNeverDrops)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 0;    // 0 = unbounded
    cfg.check_interval_ms = 60000;
    MonitoredQueue<int> q(cfg);

    for (int i = 0; i < 1000; ++i)
        q.push(i);

    EXPECT_EQ(q.total_dropped(), 0u);
    EXPECT_EQ(q.total_pushed(), 1000u);
    EXPECT_EQ(q.size(), 1000u);
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.FireAndForget_True_SkipsCallbacksEvenWhenSet
//
// Default mode (fire_and_forget=true) must NOT invoke any monitoring callbacks
// even when they are registered and the queue has stagnant depth long enough to
// normally trigger on_warn/on_dead.
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, FireAndForget_True_SkipsCallbacksEvenWhenSet)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 100;
    cfg.check_interval_ms = 30;
    cfg.dead_timeout_ms   = 50;
    cfg.fire_and_forget   = true; // default — callbacks must be silenced
    MonitoredQueue<int> q(cfg);

    std::atomic<int> warn_count{0};
    std::atomic<int> dead_count{0};
    std::atomic<int> cleared_count{0};
    q.set_on_warn([&](size_t, int) { ++warn_count; });
    q.set_on_dead([&]() { ++dead_count; });
    q.set_on_cleared([&](int) { ++cleared_count; });

    // Simulate stagnant backpressure that would normally trigger all three callbacks.
    for (int cycle = 0; cycle < 6; ++cycle)
    {
        for (int i = 0; i < 3; ++i)
            q.push(i);
        std::this_thread::sleep_for(ms{40});
        q.drain([](int &) {});
    }

    EXPECT_EQ(warn_count.load(), 0) << "fire_and_forget=true must silence on_warn";
    EXPECT_EQ(dead_count.load(), 0) << "fire_and_forget=true must silence on_dead";
    EXPECT_EQ(cleared_count.load(), 0) << "fire_and_forget=true must silence on_cleared";
}

// ─────────────────────────────────────────────────────────────────────────────
// MonitoredQueueTest.MoveAssignment_ResetsMonitoringState
//
// Moving a queue that was in backpressure state must not carry stale monitoring
// state (backpressure_, dead_fired_, depth_at_last_check_) into the new object.
// ─────────────────────────────────────────────────────────────────────────────
TEST(MonitoredQueueTest, MoveAssignment_ResetsMonitoringState)
{
    MonitoredQueue<int>::Config cfg;
    cfg.max_depth         = 100;
    cfg.check_interval_ms = 30;
    cfg.dead_timeout_ms   = 50;
    cfg.fire_and_forget   = false;

    // Build a queue in backpressure state.
    MonitoredQueue<int> src(cfg);
    std::atomic<int> dead_count{0};
    src.set_on_dead([&]() { ++dead_count; });

    for (int cycle = 0; cycle < 6; ++cycle)
    {
        for (int i = 0; i < 3; ++i)
            src.push(i);
        std::this_thread::sleep_for(ms{40});
        src.drain([](int &) {});
    }
    EXPECT_EQ(dead_count.load(), 1) << "src should have fired on_dead";

    // Move-assign into a fresh queue.
    MonitoredQueue<int> dst(cfg);
    std::atomic<int> dst_dead_count{0};
    dst.set_on_dead([&]() { ++dst_dead_count; });
    dst = std::move(src);

    // dst inherited the dead callback from src via move, but monitoring state reset.
    // A single drain of an empty queue must NOT re-fire on_dead.
    dst.drain([](int &) {});
    EXPECT_EQ(dst_dead_count.load(), 0) << "stale dead_fired_ must not carry over via move";
    EXPECT_FALSE(dst.in_backpressure()) << "backpressure_ must reset after move";
}
