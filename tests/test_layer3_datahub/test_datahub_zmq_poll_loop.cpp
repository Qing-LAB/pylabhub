/**
 * @file test_datahub_zmq_poll_loop.cpp
 * @brief Unit tests for scripting::PeriodicTask and scripting::ZmqPollLoop.
 *
 * PeriodicTask tests are pure logic (no ZMQ needed).
 * ZmqPollLoop tests use ZMQ inproc sockets for deterministic control.
 */

#include "utils/zmq_poll_loop.hpp"
#include "utils/zmq_context.hpp"

#include "test_sync_utils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pylabhub::scripting;
using pylabhub::tests::helper::poll_until;

// ============================================================================
// PeriodicTask tests (iteration-gated mode)
// ============================================================================

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

// ============================================================================
// PeriodicTask tests (time-only mode — no iteration gate)
// ============================================================================

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
// ZmqPollLoop tests
// ============================================================================

class ZmqPollLoopTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ctx_ = std::make_unique<zmq::context_t>(1);
    }

    void TearDown() override
    {
        ctx_.reset();
    }

    std::unique_ptr<zmq::context_t> ctx_;
};

TEST_F(ZmqPollLoopTest, EmptySocketsReturnsImmediately)
{
    ZmqPollLoop loop{[] { return true; }, "test"};
    loop.run();
}

TEST_F(ZmqPollLoopTest, ShutdownStopsLoop)
{
    zmq::socket_t sock(*ctx_, zmq::socket_type::pair);
    sock.bind("inproc://test-shutdown");

    std::atomic<bool> running{true};
    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.sockets = {{zmq::socket_ref(zmq::from_handle, sock.handle()), [] {}}};

    std::thread t([&] { loop.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    running.store(false);

    t.join();
}

TEST_F(ZmqPollLoopTest, DispatchesOnPollin)
{
    zmq::socket_t sender(*ctx_, zmq::socket_type::pair);
    zmq::socket_t receiver(*ctx_, zmq::socket_type::pair);
    receiver.bind("inproc://test-dispatch");
    sender.connect("inproc://test-dispatch");

    std::atomic<int> dispatch_count{0};
    std::atomic<bool> running{true};

    auto recv_ref = zmq::socket_ref(zmq::from_handle, receiver.handle());
    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.sockets = {{recv_ref, [&] {
        zmq::message_t msg;
        (void)receiver.recv(msg, zmq::recv_flags::dontwait);
        dispatch_count.fetch_add(1);
    }}};

    std::thread t([&] { loop.run(); });

    // ZMQ inproc PAIR establishment is synchronous after bind/connect,
    // but the worker thread needs a moment to actually enter zmq::poll
    // before we send.  Sending before poll is entered would still be
    // delivered (queued by ZMQ) — kept short.
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    sender.send(zmq::message_t("X", 1), zmq::send_flags::none);

    // Wait for the dispatch handler to actually run.  Replaces a
    // bare `sleep_for(50ms); EXPECT_GE(count, 1)` ordering pattern
    // (Class B silent-failure — see REVIEW_TestAudit_2026-05-01.md
    // §0).  A regression where dispatch never fires now produces
    // a clear "did not see >=1 dispatch" failure within the 2 s
    // deadline instead of an opaque count-mismatch.
    ASSERT_TRUE(poll_until([&] { return dispatch_count.load() >= 1; },
                           std::chrono::seconds{2}))
        << "dispatch handler did not fire within 2 s after send";
    EXPECT_GE(dispatch_count.load(), 1);

    running.store(false);
    t.join();
}

TEST_F(ZmqPollLoopTest, SignalSocketWakesLoop)
{
    zmq::socket_t sig_write(*ctx_, zmq::socket_type::pair);
    zmq::socket_t sig_read(*ctx_, zmq::socket_type::pair);
    sig_read.bind("inproc://test-signal");
    sig_write.connect("inproc://test-signal");

    std::atomic<int> drain_count{0};
    std::atomic<bool> running{true};

    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.signal_socket = zmq::socket_ref(zmq::from_handle, sig_read.handle());
    loop.drain_commands = [&] { drain_count.fetch_add(1); };

    std::thread t([&] { loop.run(); });

    // Same rationale as DispatchesOnPollin: short establishment
    // sleep + condition wait on the side-effect.
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    sig_write.send(zmq::message_t("W", 1), zmq::send_flags::none);

    ASSERT_TRUE(poll_until([&] { return drain_count.load() >= 1; },
                           std::chrono::seconds{2}))
        << "drain_commands callback did not run within 2 s after signal";
    EXPECT_GE(drain_count.load(), 1);

    running.store(false);
    t.join();
}

TEST_F(ZmqPollLoopTest, ShutdownPredicateStopsLoop)
{
    zmq::socket_t sock(*ctx_, zmq::socket_type::pair);
    sock.bind("inproc://test-shutdown-pred");

    std::atomic<bool> running{true};
    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.sockets = {{zmq::socket_ref(zmq::from_handle, sock.handle()), [] {}}};

    std::thread t([&] { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    running.store(false);
    t.join();
}

TEST_F(ZmqPollLoopTest, MultipleSocketsDispatchCorrectly)
{
    zmq::socket_t s1(*ctx_, zmq::socket_type::pair);
    zmq::socket_t s2(*ctx_, zmq::socket_type::pair);
    zmq::socket_t c1(*ctx_, zmq::socket_type::pair);
    zmq::socket_t c2(*ctx_, zmq::socket_type::pair);
    s1.bind("inproc://test-multi-1");
    s2.bind("inproc://test-multi-2");
    c1.connect("inproc://test-multi-1");
    c2.connect("inproc://test-multi-2");

    std::atomic<int> count1{0}, count2{0};
    std::atomic<bool> running{true};

    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.sockets = {
        {zmq::socket_ref(zmq::from_handle, s1.handle()), [&] {
            zmq::message_t m;
            (void)s1.recv(m, zmq::recv_flags::dontwait);
            count1.fetch_add(1);
        }},
        {zmq::socket_ref(zmq::from_handle, s2.handle()), [&] {
            zmq::message_t m;
            (void)s2.recv(m, zmq::recv_flags::dontwait);
            count2.fetch_add(1);
        }},
    };

    std::thread t([&] { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    c1.send(zmq::message_t("A", 1), zmq::send_flags::none);
    c2.send(zmq::message_t("B", 1), zmq::send_flags::none);

    // Wait until BOTH per-socket dispatchers have run.  A regression
    // that only routes one socket would now fail with the explicit
    // "both >=1" predicate inside the deadline rather than producing
    // a confusing post-deadline count comparison.
    ASSERT_TRUE(poll_until(
        [&] { return count1.load() >= 1 && count2.load() >= 1; },
        std::chrono::seconds{2}))
        << "expected both per-socket dispatchers to fire; "
        << "count1=" << count1.load() << " count2=" << count2.load();
    EXPECT_GE(count1.load(), 1);
    EXPECT_GE(count2.load(), 1);

    running.store(false);
    t.join();
}

TEST_F(ZmqPollLoopTest, PeriodicTasksFireDuringLoop)
{
    zmq::socket_t sock(*ctx_, zmq::socket_type::pair);
    sock.bind("inproc://test-periodic");

    std::atomic<int> fire_count{0};
    std::atomic<bool> running{true};

    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.sockets = {{zmq::socket_ref(zmq::from_handle, sock.handle()), [] {}}};
    loop.periodic_tasks.emplace_back(
        [&] { fire_count.fetch_add(1); }, 10); // 10ms, time-only

    std::thread t([&] { loop.run(); });
    // Periodic task fires at 10 ms cadence; wait until at least 2
    // fires are observed.  Replaces `sleep_for(50ms); EXPECT_GE(>=2)`
    // — a 100× slower regression would have produced the same green
    // result on the old form because we never asserted the upper
    // bound.
    ASSERT_TRUE(poll_until([&] { return fire_count.load() >= 2; },
                           std::chrono::seconds{2}))
        << "PeriodicTask fired " << fire_count.load() << " times; "
        << "expected >=2 within 2 s at 10 ms cadence";
    EXPECT_GE(fire_count.load(), 2);

    running.store(false);
    t.join();
}
