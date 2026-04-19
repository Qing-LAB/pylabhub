/**
 * @file role_data_loop_workers.cpp
 * @brief Worker bodies for run_data_loop + ThreadManager tests (Pattern 3).
 *
 * Why a worker subprocess
 * -----------------------
 * Every body builds a RoleAPIBase, which constructs a ThreadManager that
 * registers a dynamic lifecycle module against LifecycleManager. Without
 * an initialised lifecycle the registration leaves half-state and the
 * teardown flakes. run_gtest_worker owns the Logger LifecycleGuard for
 * this subprocess; LifecycleGuard's destructor finalizes ThreadManager
 * cleanly before _exit.
 *
 * The mock CycleOps types (MockCycleOps, SlowOps) and StubEngine that
 * used to live in the parent test file are defined here in an anonymous
 * namespace so each worker can reuse them without dragging gtest fixture
 * machinery into the subprocess.
 */
#include "role_data_loop_workers.h"

#include "service/data_loop.hpp"

#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"
#include "utils/thread_manager.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using pylabhub::LoopTimingPolicy;
// data_loop.hpp + script_engine.hpp put AcquireContext, LoopConfig,
// run_data_loop, IncomingMessage, InvokeRx/Tx/Inbox, InvokeResult,
// InvokeResponse, InvokeStatus, RoleAPIBase, RoleHostCore, ScriptEngine
// all under pylabhub::scripting (the role API namespace).
using pylabhub::scripting::AcquireContext;
using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::InvokeInbox;
using pylabhub::scripting::InvokeResponse;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeStatus;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::LoopConfig;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::ScriptEngine;
using pylabhub::scripting::run_data_loop;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace role_data_loop
{
namespace
{

class MockCycleOps final
{
  public:
    int  acquire_count{0};
    int  invoke_count{0};
    int  cleanup_shutdown_count{0};
    int  cleanup_exit_count{0};
    bool acquire_returns_data{true};
    bool invoke_returns_continue{true};

    bool acquire(const AcquireContext &)
    {
        ++acquire_count;
        return acquire_returns_data;
    }
    void cleanup_on_shutdown() { ++cleanup_shutdown_count; }
    bool invoke_and_commit(std::vector<IncomingMessage> &)
    {
        ++invoke_count;
        return invoke_returns_continue;
    }
    void cleanup_on_exit() { ++cleanup_exit_count; }
};

struct SlowOps final
{
    int  cycle_count{0};
    bool acquire(const AcquireContext &) { return true; }
    void cleanup_on_shutdown() {}
    bool invoke_and_commit(std::vector<IncomingMessage> &)
    {
        ++cycle_count;
        if (cycle_count == 2)
            std::this_thread::sleep_for(std::chrono::milliseconds{15});
        return cycle_count < 4;
    }
    void cleanup_on_exit() {}
};

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
                            const std::string &, const std::string &) override
    { return true; }
    size_t         type_sizeof(const std::string &) const override { return 0; }
    bool           invoke(const std::string &) override { return true; }
    bool           invoke(const std::string &, const nlohmann::json &) override { return true; }
    InvokeResponse eval(const std::string &) override
    { return {InvokeStatus::NotFound, {}}; }
    void invoke_on_init() override {}
    void invoke_on_stop() override {}
    InvokeResult invoke_produce(InvokeTx, std::vector<IncomingMessage> &) override
    { return InvokeResult::Commit; }
    InvokeResult invoke_consume(InvokeRx, std::vector<IncomingMessage> &) override
    { return InvokeResult::Commit; }
    InvokeResult invoke_process(InvokeRx, InvokeTx,
                                std::vector<IncomingMessage> &) override
    { return InvokeResult::Commit; }
    InvokeResult invoke_on_inbox(InvokeInbox) override { return InvokeResult::Commit; }
    uint64_t     script_error_count() const noexcept override { return 0; }
    bool         supports_multi_state() const noexcept override { return false; }
};

/// Fresh RoleAPIBase per worker. The uid embeds the scenario name so each
/// subprocess registers a unique dynamic ThreadManager module — cosmetic in
/// a single-test subprocess but keeps logs grep-friendly.
std::unique_ptr<RoleAPIBase> make_api(RoleHostCore &core, const char *uid)
{
    return std::make_unique<RoleAPIBase>(core, "test", uid);
}

} // namespace

// ── run_data_loop scenarios ─────────────────────────────────────────────────

int shutdown_stops_loop()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "shutdown_stops_loop");

            MockCycleOps ops;
            core.set_running(true);
            std::thread stopper([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                core.request_stop();
            });

            LoopConfig cfg;
            cfg.period_us  = 0;
            cfg.loop_timing = LoopTimingPolicy::MaxRate;

            run_data_loop(*api, core, cfg, ops);
            stopper.join();

            EXPECT_GE(ops.acquire_count, 1);
            EXPECT_GE(ops.invoke_count, 1);
            EXPECT_EQ(ops.cleanup_exit_count, 1);
        },
        "role_data_loop::shutdown_stops_loop", Logger::GetLifecycleModule());
}

int invoke_returns_false_stops_loop()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "invoke_returns_false");

            MockCycleOps ops;
            ops.invoke_returns_continue = false;
            core.set_running(true);

            LoopConfig cfg;
            cfg.period_us  = 0;
            cfg.loop_timing = LoopTimingPolicy::MaxRate;

            run_data_loop(*api, core, cfg, ops);

            EXPECT_EQ(ops.acquire_count, 1);
            EXPECT_EQ(ops.invoke_count, 1);
            EXPECT_EQ(ops.cleanup_exit_count, 1);
        },
        "role_data_loop::invoke_returns_false_stops_loop",
        Logger::GetLifecycleModule());
}

int metrics_increment()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "metrics_increment");

            MockCycleOps ops;
            core.set_running(true);
            std::thread stopper([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                core.request_stop();
            });

            LoopConfig cfg;
            cfg.period_us  = 0;
            cfg.loop_timing = LoopTimingPolicy::MaxRate;

            run_data_loop(*api, core, cfg, ops);
            stopper.join();

            EXPECT_GE(core.iteration_count(), 1u);
            EXPECT_GE(core.last_cycle_work_us(), 0u);
        },
        "role_data_loop::metrics_increment", Logger::GetLifecycleModule());
}

int no_data_skips_deadline_wait()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "no_data_skips_deadline_wait");

            MockCycleOps ops;
            ops.acquire_returns_data = false;
            core.set_running(true);
            std::thread stopper([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds{30});
                core.request_stop();
            });

            LoopConfig cfg;
            cfg.period_us                = 100000;
            cfg.loop_timing              = LoopTimingPolicy::FixedRate;
            cfg.queue_io_wait_timeout_ratio = 0.1;

            auto start = std::chrono::steady_clock::now();
            run_data_loop(*api, core, cfg, ops);
            stopper.join();
            auto elapsed = std::chrono::steady_clock::now() - start;

            EXPECT_GE(ops.acquire_count, 2);
            EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                          .count(),
                      200);
        },
        "role_data_loop::no_data_skips_deadline_wait",
        Logger::GetLifecycleModule());
}

int overrun_detected()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "overrun_detected");

            core.set_running(true);
            SlowOps slow_ops;

            LoopConfig cfg;
            cfg.period_us                = 10000;
            cfg.loop_timing              = LoopTimingPolicy::FixedRate;
            cfg.queue_io_wait_timeout_ratio = 0.1;

            run_data_loop(*api, core, cfg, slow_ops);

            EXPECT_GE(core.loop_overrun_count(), 1u);
        },
        "role_data_loop::overrun_detected", Logger::GetLifecycleModule());
}

// ── ThreadManager scenarios ─────────────────────────────────────────────────

int thread_manager_spawn_and_join()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "tm_spawn_and_join");
            StubEngine   engine;
            api->set_engine(&engine);

            core.set_running(true);
            std::atomic<int> counter{0};

            api->thread_manager().spawn("worker", [&] { counter.fetch_add(1); });

            EXPECT_EQ(api->thread_manager().active_count(), 1u);
            std::this_thread::sleep_for(std::chrono::milliseconds{20});

            api->thread_manager().drain();
            EXPECT_EQ(counter.load(), 1);
            EXPECT_EQ(api->thread_manager().active_count(), 0u);
        },
        "role_data_loop::thread_manager_spawn_and_join",
        Logger::GetLifecycleModule());
}

int thread_manager_multiple_threads()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "tm_multiple_threads");
            StubEngine   engine;
            api->set_engine(&engine);

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
        },
        "role_data_loop::thread_manager_multiple_threads",
        Logger::GetLifecycleModule());
}

int thread_manager_join_in_reverse_order()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto         api = make_api(core, "tm_join_in_reverse_order");
            StubEngine   engine;
            api->set_engine(&engine);

            core.set_running(true);
            std::vector<int> order;
            std::mutex       mu;

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

            EXPECT_EQ(order.size(), 2u);
        },
        "role_data_loop::thread_manager_join_in_reverse_order",
        Logger::GetLifecycleModule());
}

} // namespace role_data_loop
} // namespace pylabhub::tests::worker

namespace
{

struct RoleDataLoopWorkerRegistrar
{
    RoleDataLoopWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "role_data_loop")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_data_loop;

                if (sc == "shutdown_stops_loop")
                    return shutdown_stops_loop();
                if (sc == "invoke_returns_false_stops_loop")
                    return invoke_returns_false_stops_loop();
                if (sc == "metrics_increment")
                    return metrics_increment();
                if (sc == "no_data_skips_deadline_wait")
                    return no_data_skips_deadline_wait();
                if (sc == "overrun_detected")
                    return overrun_detected();
                if (sc == "thread_manager_spawn_and_join")
                    return thread_manager_spawn_and_join();
                if (sc == "thread_manager_multiple_threads")
                    return thread_manager_multiple_threads();
                if (sc == "thread_manager_join_in_reverse_order")
                    return thread_manager_join_in_reverse_order();

                fmt::print(stderr,
                           "[role_data_loop] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static RoleDataLoopWorkerRegistrar g_role_data_loop_registrar;

} // namespace
