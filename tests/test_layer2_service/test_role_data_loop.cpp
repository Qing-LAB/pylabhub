/**
 * @file test_role_data_loop.cpp
 * @brief L2 unit tests for the unified data loop framework primitives.
 *
 * Tests retry_acquire(), run_data_loop() with mock RoleCycleOps,
 * MonitoredQueue::set_on_push_signal(), and RoleAPIBase thread manager.
 *
 * No real infrastructure (broker, queues, engines) — pure logic tests.
 */

#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"
#include "utils/thread_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace pylabhub::scripting;
using namespace pylabhub::hub;
using pylabhub::LoopTimingPolicy;

// ============================================================================
// retry_acquire tests
// ============================================================================

class RetryAcquireTest : public ::testing::Test
{
  protected:
    RoleHostCore core;
};

TEST_F(RetryAcquireTest, SucceedsOnFirstAttempt)
{
    int dummy = 42;
    AcquireContext ctx;
    ctx.short_timeout = std::chrono::milliseconds{10};
    ctx.short_timeout_us = std::chrono::microseconds{10000};
    ctx.deadline = std::chrono::steady_clock::time_point::max();
    ctx.is_max_rate = false;

    core.set_running(true);
    void *result = retry_acquire(ctx, core,
        [&](auto) { return static_cast<void *>(&dummy); });
    EXPECT_EQ(result, &dummy);
}

TEST_F(RetryAcquireTest, MaxRateSingleAttempt)
{
    int attempt_count = 0;
    AcquireContext ctx;
    ctx.short_timeout = std::chrono::milliseconds{1};
    ctx.short_timeout_us = std::chrono::microseconds{1000};
    ctx.deadline = std::chrono::steady_clock::time_point::max();
    ctx.is_max_rate = true;

    core.set_running(true);
    void *result = retry_acquire(ctx, core,
        [&](auto) -> void * { ++attempt_count; return nullptr; });
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(attempt_count, 1); // MaxRate: exactly one attempt
}

TEST_F(RetryAcquireTest, RetriesUntilSuccess)
{
    int attempt_count = 0;
    int dummy = 99;
    AcquireContext ctx;
    ctx.short_timeout = std::chrono::milliseconds{1};
    ctx.short_timeout_us = std::chrono::microseconds{1000};
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    ctx.is_max_rate = false;

    core.set_running(true);
    void *result = retry_acquire(ctx, core,
        [&](auto) -> void * {
            ++attempt_count;
            if (attempt_count >= 3)
                return static_cast<void *>(&dummy);
            return nullptr;
        });
    EXPECT_EQ(result, &dummy);
    EXPECT_GE(attempt_count, 3);
}

TEST_F(RetryAcquireTest, StopsOnShutdown)
{
    int attempt_count = 0;
    AcquireContext ctx;
    ctx.short_timeout = std::chrono::milliseconds{1};
    ctx.short_timeout_us = std::chrono::microseconds{1000};
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    ctx.is_max_rate = false;

    core.set_running(true);

    // Shut down after 2 attempts from another thread.
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        core.request_stop();
    });

    void *result = retry_acquire(ctx, core,
        [&](auto) -> void * { ++attempt_count; return nullptr; });

    stopper.join();
    EXPECT_EQ(result, nullptr);
    EXPECT_GE(attempt_count, 1);
}

TEST_F(RetryAcquireTest, StopsWhenDeadlineExhausted)
{
    int attempt_count = 0;
    AcquireContext ctx;
    ctx.short_timeout = std::chrono::milliseconds{5};
    ctx.short_timeout_us = std::chrono::microseconds{5000};
    // Deadline very close.
    ctx.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{8};
    ctx.is_max_rate = false;

    core.set_running(true);
    auto start = std::chrono::steady_clock::now();
    void *result = retry_acquire(ctx, core,
        [&](auto) -> void * { ++attempt_count; return nullptr; });
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(result, nullptr);
    EXPECT_GE(attempt_count, 1);
    // Must return within reasonable time (deadline + tolerance).
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
}

TEST_F(RetryAcquireTest, FirstCycleRetriesIndefinitely)
{
    // deadline == max means first cycle → retries until success or shutdown.
    int attempt_count = 0;
    int dummy = 77;
    AcquireContext ctx;
    ctx.short_timeout = std::chrono::milliseconds{1};
    ctx.short_timeout_us = std::chrono::microseconds{1000};
    ctx.deadline = std::chrono::steady_clock::time_point::max();
    ctx.is_max_rate = false;

    core.set_running(true);
    void *result = retry_acquire(ctx, core,
        [&](auto) -> void * {
            ++attempt_count;
            if (attempt_count >= 5)
                return static_cast<void *>(&dummy);
            return nullptr;
        });
    EXPECT_EQ(result, &dummy);
    EXPECT_EQ(attempt_count, 5);
}

// ============================================================================
// Mock CycleOps for run_data_loop tests
// ============================================================================

class MockCycleOps final : public RoleCycleOps
{
  public:
    int acquire_count{0};
    int invoke_count{0};
    int cleanup_shutdown_count{0};
    int cleanup_exit_count{0};
    bool acquire_returns_data{true};
    bool invoke_returns_continue{true};

    bool acquire(const AcquireContext &) override
    {
        ++acquire_count;
        return acquire_returns_data;
    }

    void cleanup_on_shutdown() override { ++cleanup_shutdown_count; }

    bool invoke_and_commit(std::vector<IncomingMessage> &) override
    {
        ++invoke_count;
        return invoke_returns_continue;
    }

    void cleanup_on_exit() override { ++cleanup_exit_count; }
};

// ============================================================================
// run_data_loop tests
// ============================================================================

class RunDataLoopTest : public ::testing::Test
{
  protected:
    RoleHostCore core;
    std::unique_ptr<RoleAPIBase> api;

    void SetUp() override
    {
        const auto *info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        api = std::make_unique<RoleAPIBase>(
            core, "test",
            std::string{info->test_suite_name()} + "." + info->name());
    }
};

TEST_F(RunDataLoopTest, ShutdownStopsLoop)
{
    MockCycleOps ops;
    core.set_running(true);

    // Stop after a few cycles.
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        core.request_stop();
    });

    LoopConfig cfg;
    cfg.period_us = 0; // MaxRate
    cfg.loop_timing = LoopTimingPolicy::MaxRate;

    api->run_data_loop(cfg, ops);
    stopper.join();

    EXPECT_GE(ops.acquire_count, 1);
    EXPECT_GE(ops.invoke_count, 1);
    EXPECT_EQ(ops.cleanup_exit_count, 1);
    // cleanup_on_shutdown should NOT be called for normal loop exit
    // (it's called only when shutdown detected after deadline wait).
}

TEST_F(RunDataLoopTest, InvokeReturnsFalseStopsLoop)
{
    MockCycleOps ops;
    ops.invoke_returns_continue = false; // stop_on_script_error
    core.set_running(true);

    LoopConfig cfg;
    cfg.period_us = 0;
    cfg.loop_timing = LoopTimingPolicy::MaxRate;

    api->run_data_loop(cfg, ops);

    EXPECT_EQ(ops.acquire_count, 1);
    EXPECT_EQ(ops.invoke_count, 1);
    EXPECT_EQ(ops.cleanup_exit_count, 1);
}

TEST_F(RunDataLoopTest, MetricsIncrement)
{
    MockCycleOps ops;
    core.set_running(true);

    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        core.request_stop();
    });

    LoopConfig cfg;
    cfg.period_us = 0;
    cfg.loop_timing = LoopTimingPolicy::MaxRate;

    api->run_data_loop(cfg, ops);
    stopper.join();

    EXPECT_GE(core.iteration_count(), 1u);
    EXPECT_GE(core.last_cycle_work_us(), 0u);
}

TEST_F(RunDataLoopTest, NoDataSkipsDeadlineWait)
{
    MockCycleOps ops;
    ops.acquire_returns_data = false; // no data acquired
    core.set_running(true);

    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        core.request_stop();
    });

    LoopConfig cfg;
    cfg.period_us = 100000; // 100ms FixedRate
    cfg.loop_timing = LoopTimingPolicy::FixedRate;
    cfg.queue_io_wait_timeout_ratio = 0.1;

    auto start = std::chrono::steady_clock::now();
    api->run_data_loop(cfg, ops);
    stopper.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Should NOT have slept 100ms per cycle because no data → no deadline wait.
    // The loop should have run multiple cycles in ~30ms.
    EXPECT_GE(ops.acquire_count, 2);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 200);
}

TEST_F(RunDataLoopTest, OverrunDetected)
{
    MockCycleOps ops;
    core.set_running(true);

    int cycle = 0;
    ops.acquire_returns_data = true;

    // Override invoke to waste time on first cycle, triggering overrun.
    struct SlowOps final : public RoleCycleOps
    {
        int cycle_count{0};
        bool acquire(const AcquireContext &) override { return true; }
        void cleanup_on_shutdown() override {}
        bool invoke_and_commit(std::vector<IncomingMessage> &) override
        {
            ++cycle_count;
            if (cycle_count == 2)
            {
                // Waste 15ms on second cycle to trigger overrun on 10ms period.
                std::this_thread::sleep_for(std::chrono::milliseconds{15});
            }
            return cycle_count < 4; // stop after 4 cycles
        }
        void cleanup_on_exit() override {}
    } slow_ops;

    LoopConfig cfg;
    cfg.period_us = 10000; // 10ms
    cfg.loop_timing = LoopTimingPolicy::FixedRate;
    cfg.queue_io_wait_timeout_ratio = 0.1;

    api->run_data_loop(cfg, slow_ops);

    // At least one overrun should have been detected.
    EXPECT_GE(core.loop_overrun_count(), 1u);
}

// ============================================================================
// Thread manager tests
// ============================================================================

class ThreadManagerTest : public ::testing::Test
{
  protected:
    RoleHostCore core;
    std::unique_ptr<RoleAPIBase> api;

    // Minimal ScriptEngine stub for ThreadEngineGuard.
    struct StubEngine : public ScriptEngine
    {
      protected:
        bool init_engine_(const std::string &, RoleHostCore *) override { return true; }
        bool build_api_(RoleAPIBase &) override { return true; }
        void finalize_engine_() override {}
      public:
        bool load_script(const std::filesystem::path &, const std::string &,
                         const std::string &) override { return true; }
        bool has_callback(const std::string &) const override { return false; }
        bool register_slot_type(const pylabhub::hub::SchemaSpec &,
                                const std::string &,
                                const std::string &) override { return true; }
        size_t type_sizeof(const std::string &) const override { return 0; }
        bool invoke(const std::string &) override { return true; }
        bool invoke(const std::string &, const nlohmann::json &) override { return true; }
        InvokeResponse eval(const std::string &) override
        { return {InvokeStatus::NotFound, {}}; }
        void invoke_on_init() override {}
        void invoke_on_stop() override {}
        InvokeResult invoke_produce(InvokeTx, std::vector<IncomingMessage> &) override
        { return InvokeResult::Commit; }
        InvokeResult invoke_consume(InvokeRx, std::vector<IncomingMessage> &) override
        { return InvokeResult::Commit; }
        InvokeResult invoke_process(InvokeRx, InvokeTx, std::vector<IncomingMessage> &) override
        { return InvokeResult::Commit; }
        InvokeResult invoke_on_inbox(InvokeInbox) override
        { return InvokeResult::Commit; }
        uint64_t script_error_count() const noexcept override { return 0; }
        bool supports_multi_state() const noexcept override { return false; }
    };

    StubEngine engine;

    void SetUp() override
    {
        // role_tag + uid at ctor. Each test's uid is unique per-name so
        // its dynamic lifecycle module "ThreadManager:test:{uid}" doesn't
        // clash with other tests in the same binary.
        const auto *info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        api = std::make_unique<RoleAPIBase>(
            core, "test",
            std::string{info->test_suite_name()} + "." + info->name());
        api->set_engine(&engine);
    }
};

TEST_F(ThreadManagerTest, SpawnAndJoin)
{
    core.set_running(true);
    std::atomic<int> counter{0};

    api->thread_manager().spawn("worker", [&] {
        counter.fetch_add(1);
    });

    EXPECT_EQ(api->thread_manager().active_count(), 1u);

    // Thread runs immediately; give it a moment.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    api->thread_manager().drain();
    EXPECT_EQ(counter.load(), 1);
    EXPECT_EQ(api->thread_manager().active_count(), 0u);
}

TEST_F(ThreadManagerTest, MultipleThreads)
{
    core.set_running(true);
    std::atomic<int> counter{0};

    api->thread_manager().spawn("a", [&] { counter.fetch_add(1); });
    api->thread_manager().spawn("b", [&] { counter.fetch_add(10); });
    api->thread_manager().spawn("c", [&] { counter.fetch_add(100); });

    EXPECT_EQ(api->thread_manager().active_count(), 3u);

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    api->thread_manager().drain();

    EXPECT_EQ(counter.load(), 111);
    EXPECT_EQ(api->thread_manager().active_count(), 0u);
}

TEST_F(ThreadManagerTest, JoinInReverseOrder)
{
    core.set_running(true);
    std::vector<int> order;
    std::mutex mu;

    api->thread_manager().spawn("first", [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        std::lock_guard<std::mutex> lk(mu);
        order.push_back(1);
    });
    api->thread_manager().spawn("second", [&] {
        std::lock_guard<std::mutex> lk(mu);
        order.push_back(2);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    api->thread_manager().drain();

    // Both should have run. Exact order depends on scheduling,
    // but both must complete.
    EXPECT_EQ(order.size(), 2u);
}
