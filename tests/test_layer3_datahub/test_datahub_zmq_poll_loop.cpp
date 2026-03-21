/**
 * @file test_datahub_zmq_poll_loop.cpp
 * @brief Unit tests for scripting::HeartbeatTracker and scripting::ZmqPollLoop.
 *
 * HeartbeatTracker tests are pure logic (no ZMQ needed).
 * ZmqPollLoop tests use ZMQ inproc sockets for deterministic control.
 */

#include "scripting/zmq_poll_loop.hpp"

#include <gtest/gtest.h>

#include <zmq.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pylabhub::scripting;

// ============================================================================
// HeartbeatTracker tests
// ============================================================================

class HeartbeatTrackerTest : public ::testing::Test
{
};

TEST_F(HeartbeatTrackerTest, FirstTickFiresImmediatelyOnProgress)
{
    int fire_count = 0;
    HeartbeatTracker tracker([&] { ++fire_count; }, 1000);

    // First tick with iteration=1 should fire (bootstraps immediately).
    tracker.tick(1);
    EXPECT_EQ(fire_count, 1);
}

TEST_F(HeartbeatTrackerTest, NoProgressMeansNoFire)
{
    int fire_count = 0;
    HeartbeatTracker tracker([&] { ++fire_count; }, 1000);

    // tick(0) — iteration hasn't advanced from default last_iter(0).
    tracker.tick(0);
    EXPECT_EQ(fire_count, 0);

    // Still 0.
    tracker.tick(0);
    EXPECT_EQ(fire_count, 0);
}

TEST_F(HeartbeatTrackerTest, ProgressButIntervalNotElapsedSkips)
{
    int fire_count = 0;
    HeartbeatTracker tracker([&] { ++fire_count; }, 60000); // 60s interval

    // First fire on progress.
    tracker.tick(1);
    EXPECT_EQ(fire_count, 1);

    // Progress again immediately — interval hasn't elapsed.
    tracker.tick(2);
    EXPECT_EQ(fire_count, 1); // still 1
}

TEST_F(HeartbeatTrackerTest, FiresAgainAfterIntervalElapsed)
{
    int fire_count = 0;
    HeartbeatTracker tracker([&] { ++fire_count; }, 10); // 10ms interval

    tracker.tick(1);
    EXPECT_EQ(fire_count, 1);

    // Wait for interval to elapse.
    std::this_thread::sleep_for(std::chrono::milliseconds{15});

    tracker.tick(2);
    EXPECT_EQ(fire_count, 2);
}

TEST_F(HeartbeatTrackerTest, DefaultIntervalUsedWhenZero)
{
    int fire_count = 0;
    HeartbeatTracker tracker([&] { ++fire_count; }, 0); // 0 → default 2000ms

    EXPECT_EQ(tracker.interval.count(), 2000);

    // First tick still fires (bootstrap).
    tracker.tick(1);
    EXPECT_EQ(fire_count, 1);
}

TEST_F(HeartbeatTrackerTest, NegativeIntervalUsesDefault)
{
    int fire_count = 0;
    HeartbeatTracker tracker([&] { ++fire_count; }, -5);

    EXPECT_EQ(tracker.interval.count(), 2000);
}

TEST_F(HeartbeatTrackerTest, MultiplePeriodicTasks)
{
    int fire_a = 0, fire_b = 0;
    HeartbeatTracker a([&] { ++fire_a; }, 10);
    HeartbeatTracker b([&] { ++fire_b; }, 10);

    // Both fire on first progress.
    a.tick(1);
    b.tick(1);
    EXPECT_EQ(fire_a, 1);
    EXPECT_EQ(fire_b, 1);

    // No progress — neither fires.
    a.tick(1);
    b.tick(1);
    EXPECT_EQ(fire_a, 1);
    EXPECT_EQ(fire_b, 1);
}

TEST_F(HeartbeatTrackerTest, LargeIterationJumpStillFires)
{
    int fire_count = 0;
    HeartbeatTracker tracker([&] { ++fire_count; }, 10);

    // Jump from 0 to 1000.
    tracker.tick(1000);
    EXPECT_EQ(fire_count, 1);
}

// ============================================================================
// ZmqPollLoop tests
// ============================================================================

class ZmqPollLoopTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ctx_ = zmq_ctx_new();
        ASSERT_NE(ctx_, nullptr);
    }

    void TearDown() override
    {
        if (ctx_)
            zmq_ctx_destroy(ctx_);
    }

    void *ctx_{nullptr};
};

TEST_F(ZmqPollLoopTest, EmptySocketsReturnsImmediately)
{
    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    // No sockets added.
    loop.run(); // Should return immediately.
}

TEST_F(ZmqPollLoopTest, AllNullptrSocketsReturnsImmediately)
{
    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {
        {nullptr, [] {}},
        {nullptr, [] {}},
    };
    loop.run(); // Should return immediately.
}

TEST_F(ZmqPollLoopTest, ShutdownStopsLoop)
{
    // Create a PAIR socket (simplest for testing).
    void *sock = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(sock, nullptr);
    ASSERT_EQ(zmq_bind(sock, "inproc://test-shutdown"), 0);

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {{sock, [] {}}};
    loop.poll_interval_ms = 5;

    // Run in a thread, then signal shutdown.
    std::thread t([&] { loop.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    core.set_running(false);

    t.join(); // Should complete quickly.

    zmq_close(sock);
}

TEST_F(ZmqPollLoopTest, ShutdownRequestedStopsLoop)
{
    void *sock = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(sock, nullptr);
    ASSERT_EQ(zmq_bind(sock, "inproc://test-shutdown-req"), 0);

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {{sock, [] {}}};
    loop.poll_interval_ms = 5;

    std::thread t([&] { loop.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    core.request_stop();

    t.join();

    zmq_close(sock);
}

TEST_F(ZmqPollLoopTest, DispatchCalledOnPollin)
{
    void *sender = zmq_socket(ctx_, ZMQ_PAIR);
    void *receiver = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);
    ASSERT_EQ(zmq_bind(receiver, "inproc://test-dispatch"), 0);
    ASSERT_EQ(zmq_connect(sender, "inproc://test-dispatch"), 0);

    std::atomic<int> dispatch_count{0};

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {
        {receiver,
         [&]
         {
             // Drain the message to avoid infinite POLLIN.
             zmq_msg_t msg;
             zmq_msg_init(&msg);
             zmq_msg_recv(&msg, receiver, ZMQ_DONTWAIT);
             zmq_msg_close(&msg);
             dispatch_count.fetch_add(1);
         }},
    };
    loop.poll_interval_ms = 5;

    std::thread t([&] { loop.run(); });

    // Send a message to trigger POLLIN.
    const char *data = "hello";
    zmq_send(sender, data, 5, 0);

    // Wait for dispatch.
    for (int i = 0; i < 100 && dispatch_count.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    EXPECT_GE(dispatch_count.load(), 1);

    core.set_running(false);
    t.join();

    zmq_close(sender);
    zmq_close(receiver);
}

TEST_F(ZmqPollLoopTest, PeriodicTasksFire)
{
    void *sock = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(sock, nullptr);
    ASSERT_EQ(zmq_bind(sock, "inproc://test-periodic"), 0);

    std::atomic<int> task_count{0};
    std::atomic<uint64_t> iter{0};

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {{sock, [] {}}};
    loop.poll_interval_ms = 5;
    loop.get_iteration = [&] { return iter.load(); };
    loop.periodic_tasks.emplace_back([&] { task_count.fetch_add(1); }, 10);

    std::thread t([&] { loop.run(); });

    // Advance iteration to trigger periodic task.
    iter.store(1);

    for (int i = 0; i < 100 && task_count.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    EXPECT_GE(task_count.load(), 1);

    core.set_running(false);
    t.join();

    zmq_close(sock);
}

TEST_F(ZmqPollLoopTest, NullptrSocketsFiltered)
{
    // One valid socket, two nullptr — loop should work with just the valid one.
    void *sender = zmq_socket(ctx_, ZMQ_PAIR);
    void *receiver = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);
    ASSERT_EQ(zmq_bind(receiver, "inproc://test-filter"), 0);
    ASSERT_EQ(zmq_connect(sender, "inproc://test-filter"), 0);

    std::atomic<int> dispatch_count{0};

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {
        {nullptr, [] {}},
        {receiver,
         [&]
         {
             zmq_msg_t msg;
             zmq_msg_init(&msg);
             zmq_msg_recv(&msg, receiver, ZMQ_DONTWAIT);
             zmq_msg_close(&msg);
             dispatch_count.fetch_add(1);
         }},
        {nullptr, [] {}},
    };
    loop.poll_interval_ms = 5;

    std::thread t([&] { loop.run(); });

    const char *data = "test";
    zmq_send(sender, data, 4, 0);

    for (int i = 0; i < 100 && dispatch_count.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    EXPECT_GE(dispatch_count.load(), 1);

    core.set_running(false);
    t.join();

    zmq_close(sender);
    zmq_close(receiver);
}

TEST_F(ZmqPollLoopTest, MultipleSocketsDispatchCorrectly)
{
    void *send1 = zmq_socket(ctx_, ZMQ_PAIR);
    void *recv1 = zmq_socket(ctx_, ZMQ_PAIR);
    void *send2 = zmq_socket(ctx_, ZMQ_PAIR);
    void *recv2 = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(send1, nullptr);
    ASSERT_NE(recv1, nullptr);
    ASSERT_NE(send2, nullptr);
    ASSERT_NE(recv2, nullptr);
    ASSERT_EQ(zmq_bind(recv1, "inproc://test-multi-1"), 0);
    ASSERT_EQ(zmq_connect(send1, "inproc://test-multi-1"), 0);
    ASSERT_EQ(zmq_bind(recv2, "inproc://test-multi-2"), 0);
    ASSERT_EQ(zmq_connect(send2, "inproc://test-multi-2"), 0);

    std::atomic<int> count1{0}, count2{0};

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {
        {recv1,
         [&]
         {
             zmq_msg_t msg;
             zmq_msg_init(&msg);
             zmq_msg_recv(&msg, recv1, ZMQ_DONTWAIT);
             zmq_msg_close(&msg);
             count1.fetch_add(1);
         }},
        {recv2,
         [&]
         {
             zmq_msg_t msg;
             zmq_msg_init(&msg);
             zmq_msg_recv(&msg, recv2, ZMQ_DONTWAIT);
             zmq_msg_close(&msg);
             count2.fetch_add(1);
         }},
    };
    loop.poll_interval_ms = 5;

    std::thread t([&] { loop.run(); });

    // Send to socket 1 only.
    zmq_send(send1, "a", 1, 0);
    for (int i = 0; i < 100 && count1.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    EXPECT_GE(count1.load(), 1);
    EXPECT_EQ(count2.load(), 0);

    // Now send to socket 2.
    zmq_send(send2, "b", 1, 0);
    for (int i = 0; i < 100 && count2.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    EXPECT_GE(count2.load(), 1);

    core.set_running(false);
    t.join();

    zmq_close(send1);
    zmq_close(recv1);
    zmq_close(send2);
    zmq_close(recv2);
}

TEST_F(ZmqPollLoopTest, NoGetIterationSkipsPeriodicTasks)
{
    void *sock = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(sock, nullptr);
    ASSERT_EQ(zmq_bind(sock, "inproc://test-no-iter"), 0);

    std::atomic<int> task_count{0};

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {{sock, [] {}}};
    loop.poll_interval_ms = 5;
    // get_iteration not set — should be null/empty std::function.
    loop.periodic_tasks.emplace_back([&] { task_count.fetch_add(1); }, 10);

    std::thread t([&] { loop.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Task should NOT have fired because get_iteration is empty.
    EXPECT_EQ(task_count.load(), 0);

    core.set_running(false);
    t.join();

    zmq_close(sock);
}

TEST_F(ZmqPollLoopTest, CustomPollInterval)
{
    void *sock = zmq_socket(ctx_, ZMQ_PAIR);
    ASSERT_NE(sock, nullptr);
    ASSERT_EQ(zmq_bind(sock, "inproc://test-custom-poll"), 0);

    RoleHostCore core;
    core.set_running(true);

    ZmqPollLoop loop{core, "test"};
    loop.sockets = {{sock, [] {}}};
    loop.poll_interval_ms = 1; // Very fast polling.

    auto start = std::chrono::steady_clock::now();
    std::thread t([&] { loop.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    core.set_running(false);
    t.join();

    auto elapsed = std::chrono::steady_clock::now() - start;
    // Loop should have run many cycles in 30ms with 1ms poll.
    EXPECT_LT(elapsed, std::chrono::milliseconds{200});

    zmq_close(sock);
}
