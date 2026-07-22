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
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule())

class ZmqPollLoopTest : public ::testing::Test, public pylabhub::tests::LogCaptureFixture
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
    loop.sockets = {{recv_ref, [&]
                     {
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
    ASSERT_TRUE(poll_until([&] { return dispatch_count.load() >= 1; }, std::chrono::seconds{2}))
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

    ASSERT_TRUE(poll_until([&] { return drain_count.load() >= 1; }, std::chrono::seconds{2}))
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
        {zmq::socket_ref(zmq::from_handle, s1.handle()),
         [&]
         {
             zmq::message_t m;
             (void)s1.recv(m, zmq::recv_flags::dontwait);
             count1.fetch_add(1);
         }},
        {zmq::socket_ref(zmq::from_handle, s2.handle()),
         [&]
         {
             zmq::message_t m;
             (void)s2.recv(m, zmq::recv_flags::dontwait);
             count2.fetch_add(1);
         }},
    };

    std::thread t([&] { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    c1.send(zmq::message_t("A", 1), zmq::send_flags::none);
    c2.send(zmq::message_t("B", 1), zmq::send_flags::none);

    ASSERT_TRUE(poll_until([&] { return count1.load() >= 1 && count2.load() >= 1; },
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
    loop.periodic_tasks.emplace_back([&] { fire_count.fetch_add(1); }, 10); // 10ms, time-only

    std::thread t([&] { loop.run(); });
    ASSERT_TRUE(poll_until([&] { return fire_count.load() >= 2; }, std::chrono::seconds{2}))
        << "PeriodicTask fired " << fire_count.load() << " times; "
        << "expected >=2 within 2 s at 10 ms cadence";
    EXPECT_GE(fire_count.load(), 2);

    running.store(false);
    t.join();
}

// ============================================================================
// Coverage additions (2026-05-14) — fill gaps from review of the 7
// original transplanted tests.  Each test below closes a branch in
// `ZmqPollLoop::run()` that the originals did not exercise:
//   - FalsySocketEntrySkipped — `if (!entry.socket) continue;`
//     (zmq_poll_loop.hpp:160-163)
//   - MultiplePeriodicTasksFireIndependently — min-timeout calculation
//     when `periodic_tasks.size() > 1` (lines 200-209)
//   - SignalSocketDrainsAllPendingBytesInOnePass — drain-all while-loop
//     (lines 240-248) consumes ALL pending bytes, not 1-per-iteration
// ============================================================================

TEST_F(ZmqPollLoopTest, FalsySocketEntrySkipped)
{
    // A `zmq::socket_ref{}` is null/falsy.  `ZmqPollLoop::run()` must
    // skip such entries when assembling its pollitem array (otherwise
    // `zmq::poll` would receive a null socket handle and crash).  The
    // valid entry placed alongside must still dispatch normally.
    zmq::socket_t valid(*ctx_, zmq::socket_type::pair);
    zmq::socket_t client(*ctx_, zmq::socket_type::pair);
    valid.bind("inproc://test-falsy-skip");
    client.connect("inproc://test-falsy-skip");

    std::atomic<int> valid_count{0};
    std::atomic<int> falsy_count{0};
    std::atomic<bool> running{true};

    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.sockets = {
        {zmq::socket_ref{}, [&] { falsy_count.fetch_add(1); }},
        {zmq::socket_ref(zmq::from_handle, valid.handle()),
         [&]
         {
             zmq::message_t m;
             (void)valid.recv(m, zmq::recv_flags::dontwait);
             valid_count.fetch_add(1);
         }},
    };

    std::thread t([&] { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    client.send(zmq::message_t("X", 1), zmq::send_flags::none);

    ASSERT_TRUE(poll_until([&] { return valid_count.load() >= 1; }, std::chrono::seconds{2}))
        << "valid socket dispatcher did not fire within 2 s — "
        << "either zmq::poll crashed on the falsy entry, or the "
        << "skip path is broken and the valid entry's index in "
        << "the pollitem array is wrong";

    EXPECT_GE(valid_count.load(), 1);
    EXPECT_EQ(falsy_count.load(), 0) << "falsy entry's dispatch must never be invoked — the "
                                     << "skip happens at array-assembly time so its dispatcher "
                                     << "is never stored in `dispatchers`";

    running.store(false);
    t.join();
}

TEST_F(ZmqPollLoopTest, MultiplePeriodicTasksFireIndependently)
{
    // Two periodic_tasks with different intervals.  The poll-timeout
    // min logic (lines 200-209) must accommodate both — otherwise a
    // regression in `min(remaining, timeout)` would starve one task.
    // Detection: a starved task fires at the 200 ms default cap
    // instead of its own (shorter) interval.
    zmq::socket_t sock(*ctx_, zmq::socket_type::pair);
    sock.bind("inproc://test-multi-periodic");

    std::atomic<int> fire_a{0};
    std::atomic<int> fire_b{0};
    std::atomic<bool> running{true};

    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.sockets = {{zmq::socket_ref(zmq::from_handle, sock.handle()), [] {}}};
    loop.periodic_tasks.emplace_back([&] { fire_a.fetch_add(1); }, 10); // time-only, 10 ms
    loop.periodic_tasks.emplace_back([&] { fire_b.fetch_add(1); }, 20); // time-only, 20 ms

    std::thread t([&] { loop.run(); });
    // Allow ~250 ms wall time: at 10 ms cadence task A should fire
    // many times; task B at 20 ms cadence should fire many times.
    // We assert modest lower bounds to stay robust under CI jitter
    // while still proving "both tasks are actually firing on their
    // own cadence, not at the 200 ms cap".
    ASSERT_TRUE(poll_until([&] { return fire_a.load() >= 3 && fire_b.load() >= 2; },
                           std::chrono::seconds{2}))
        << "expected both periodic tasks to fire on their own "
        << "cadences within 2 s (A>=3 at 10 ms, B>=2 at 20 ms); "
        << "got fire_a=" << fire_a.load() << " fire_b=" << fire_b.load() << " — implies the "
        << "min-timeout logic is starving one of the tasks";
    EXPECT_GE(fire_a.load(), 3);
    EXPECT_GE(fire_b.load(), 2);

    running.store(false);
    t.join();
}

TEST_F(ZmqPollLoopTest, SignalSocketDrainsAllPendingBytesInOnePass)
{
    // The signal-socket drain (lines 240-248) reads in a while-loop
    // until `recv(dontwait)` returns nullopt — consuming ALL pending
    // bytes in a single poll iteration.  Pre-queue 5 bytes before
    // starting the loop: a correct drain handles all 5 in one pass
    // and calls drain_commands exactly once.  A regression draining
    // 1-byte-per-iteration would call drain_commands 5 times.
    zmq::socket_t sig_write(*ctx_, zmq::socket_type::pair);
    zmq::socket_t sig_read(*ctx_, zmq::socket_type::pair);
    sig_read.bind("inproc://test-signal-drain-all");
    sig_write.connect("inproc://test-signal-drain-all");

    std::atomic<int> drain_count{0};
    std::atomic<bool> running{true};

    ZmqPollLoop loop{[&] { return running.load(); }, "test"};
    loop.signal_socket = zmq::socket_ref(zmq::from_handle, sig_read.handle());
    loop.drain_commands = [&] { drain_count.fetch_add(1); };

    // Pre-queue 5 wake-up bytes BEFORE the loop runs.  inproc PAIR
    // delivery is synchronous, so all 5 are at sig_read when the
    // loop's first zmq::poll runs.
    for (int i = 0; i < 5; ++i)
    {
        sig_write.send(zmq::message_t("W", 1), zmq::send_flags::none);
    }

    std::thread t([&] { loop.run(); });

    ASSERT_TRUE(poll_until([&] { return drain_count.load() >= 1; }, std::chrono::seconds{2}))
        << "drain_commands did not fire within 2 s after pre-queuing "
        << "5 signal bytes";

    // Allow any errant extra iterations to take effect.  With a
    // correct drain, only one drain_commands invocation should
    // occur because all 5 bytes were consumed in a single pass and
    // no further bytes ever arrive.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_EQ(drain_count.load(), 1)
        << "expected drain_commands invoked exactly once (5 bytes "
        << "drained in 1 pass via while-loop); got " << drain_count.load()
        << " — implies the drain consumes only 1 byte per iteration";

    running.store(false);
    t.join();
}
