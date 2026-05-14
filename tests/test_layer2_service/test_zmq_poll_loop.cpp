/**
 * @file test_zmq_poll_loop.cpp
 * @brief L2 unit tests for `pylabhub::scripting::ZmqPollLoop`.
 *
 * Subject: the ZMQ socket polling loop module — coordinates a list
 * of socket dispatchers, a signal-socket for command-queue wake,
 * periodic tasks, and a shutdown predicate.  Tested as a coordinated
 * unit using real `zmq::context_t` + inproc PAIR sockets.
 *
 * Migrated 2026-05-14 from the SetUpTestSuite-LifecycleGuard
 * antipattern (TESTING_TODO § "Pattern-3 migration debt": originally
 * flagged for "two STS guards in one binary").  Moved from
 * `tests/test_layer3_datahub/` — placement there was historical;
 * `ZmqPollLoop` is a `pylabhub::scripting` utility module (L2),
 * not an L3 datahub protocol test.  The sibling `PeriodicTask`
 * tests (L1, function-level) split off to
 * `tests/test_layer1_base/test_periodic_task.cpp`.
 *
 * Compiled as its own executable `test_layer2_zmq_poll_loop` —
 * NOT part of any aggregate target — because it uses the framework's
 * `BinaryLifecycleEnvironment` (binary-wide `LifecycleGuard` via
 * gtest `::testing::Environment`), which requires being the only
 * `LifecycleGuard` owner in its binary.  See
 * `tests/test_framework/binary_lifecycle.h` for the rationale.
 *
 * Interference vectors examined for these 7 tests (all come back
 * clean — no need for Pattern 3 subprocess isolation):
 *   1. No static state in `ZmqPollLoop`.
 *   2. Per-test local `zmq::context_t` — inproc endpoints scoped to
 *      context; destroyed in TearDown.
 *   3. Per-test `LogCaptureFixture::Install/Uninstall` — Logger sink
 *      redirect is per-test scope.
 *   4. No deliberate crashes / panics; no init-once invariant
 *      re-violation (single Environment-owned guard in the binary).
 *
 * `ZmqPollLoop::run()` emits LOGGER_INFO (start/exit) and
 * LOGGER_WARN (zmq::poll error) — so Logger must be initialized,
 * provided by the binary-wide `PLH_BINARY_LIFECYCLE_MODULES` below.
 * `LogCaptureFixture::AssertNoUnexpectedLogWarnError` in TearDown
 * guards against any unexpected WARN/ERROR (the regression defense).
 */

#include "binary_lifecycle.h"
#include "log_capture_fixture.h"
#include "test_sync_utils.h"
#include "utils/logger.hpp"
#include "utils/zmq_context.hpp"
#include "utils/zmq_poll_loop.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace pylabhub::scripting;
using pylabhub::tests::helper::poll_until;

// Binary-wide LifecycleGuard for Logger.  Required because
// `ZmqPollLoop::run()` emits LOGGER_INFO / LOGGER_WARN.  This is the
// only `LifecycleGuard` owner in this binary by design — see file
// header.
PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule()
)

class ZmqPollLoopTest : public ::testing::Test,
                         public pylabhub::tests::LogCaptureFixture
{
  protected:
    void SetUp() override
    {
        LogCaptureFixture::Install();
        ctx_ = std::make_unique<zmq::context_t>(1);
    }

    void TearDown() override
    {
        ctx_.reset();
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
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
    // before we send.  Kept short.
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    sender.send(zmq::message_t("X", 1), zmq::send_flags::none);

    // Wait for the dispatch handler to actually run — replaces a
    // bare `sleep_for + EXPECT_GE` ordering pattern (Class B
    // silent-failure — see REVIEW_TestAudit_2026-05-01.md §0).
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
    ASSERT_TRUE(poll_until([&] { return fire_count.load() >= 2; },
                           std::chrono::seconds{2}))
        << "PeriodicTask fired " << fire_count.load() << " times; "
        << "expected >=2 within 2 s at 10 ms cadence";
    EXPECT_GE(fire_count.load(), 2);

    running.store(false);
    t.join();
}
