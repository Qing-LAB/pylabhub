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
#include "utils/role_handler.hpp" // RoleHandler + Presence construction
#include "utils/role_host_core.hpp"
#include "utils/role_presence.hpp"
#include "utils/script_engine.hpp"
#include "utils/thread_manager.hpp"

#include "plh_service.hpp"
#include "role_api_base_test_access.h" // L2-test install_handler helper
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
using pylabhub::scripting::run_data_loop;
using pylabhub::scripting::ScriptEngine;
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
    int acquire_count{0};
    int invoke_count{0};
    int cleanup_shutdown_count{0};
    int cleanup_exit_count{0};
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
    // HEP-CORE-0011 §"Loop-ready gate" — mock ops always report
    // Ready so the framework default gate is a no-op in these tests;
    // gate behaviour is exercised in dedicated cycle-ops tests.
    bool default_init_ready(const RoleAPIBase &) const { return true; }
};

struct SlowOps final
{
    int cycle_count{0};
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
    bool default_init_ready(const RoleAPIBase &) const { return true; }
};

// HEP-CORE-0011 §"Loop-ready gate" — Ops whose framework default
// starts NotReady.  Used to pin the AND composition invariant: no
// matter what the user script's `on_init` returns, the framework
// floor holds the loop closed until `default_init_ready` also flips
// true.  A script cannot bypass the floor by returning Ready alone.
class FrameworkFloorHoldsOps final
{
  public:
    int acquire_count{0};
    int invoke_count{0};
    int cleanup_shutdown_count{0};
    int cleanup_exit_count{0};

    bool acquire(const AcquireContext &)
    {
        ++acquire_count;
        return false;
    }
    void cleanup_on_shutdown() { ++cleanup_shutdown_count; }
    bool invoke_and_commit(std::vector<IncomingMessage> &)
    {
        ++invoke_count;
        return true;
    }
    void cleanup_on_exit() { ++cleanup_exit_count; }
    // Framework floor: not ready.  If the AND-composition invariant
    // holds, run_data_loop must NEVER call `acquire` regardless of
    // what the script's on_init returns.
    bool default_init_ready(const RoleAPIBase &) const { return false; }
};

struct StubEngine : public ScriptEngine
{
  protected:
    bool init_engine_(const std::string &, RoleHostCore *) override { return true; }
    bool build_api_(RoleAPIBase &) override { return true; }
    void finalize_engine_() override {}

  public:
    bool load_script(const std::filesystem::path &, const std::string &,
                     const std::string &) override
    {
        return true;
    }
    bool has_callback(const std::string &) const noexcept override { return false; }
    bool register_slot_type(const pylabhub::hub::SchemaSpec &, const std::string &,
                            const std::string &) override
    {
        return true;
    }
    size_t type_sizeof(const std::string &) const override { return 0; }
    bool invoke(const std::string &) override { return true; }
    bool invoke(const std::string &, const nlohmann::json &) override { return true; }
    InvokeResponse eval(const std::string &) override { return {InvokeStatus::NotFound, {}}; }
    InvokeResponse invoke_returning(const std::string &, const nlohmann::json &, int64_t) override
    {
        return {InvokeStatus::NotFound, {}};
    }
    pylabhub::scripting::ScriptEngine::InitStatus invoke_on_init() override
    {
        return pylabhub::scripting::ScriptEngine::InitStatus::Ready;
    }
    void invoke_on_stop() override {}
    void invoke_on_channel_closing(const std::string &, const std::string &) override {}
    void invoke_on_consumer_died(const std::string &, const std::string &,
                                 const std::string &) override
    {
    }
    void invoke_on_hub_dead(const std::string &) override {}
    void invoke_on_band_member_joined(const std::string &, const std::string &,
                                      const std::string &) override
    {
    }
    void invoke_on_band_member_left(const std::string &, const std::string &,
                                    const std::string &) override
    {
    }
    void invoke_on_band_message(const std::string &, const std::string &,
                                const nlohmann::json &) override
    {
    }
    void invoke_on_band_lost(const std::string &, const std::string &) override {}
    void invoke_on_allowlist_changed(const std::string &,
                                     const std::vector<pylabhub::scripting::AllowedPeer> &,
                                     const std::string &) override
    {
    }
    InvokeResult invoke_produce(InvokeTx, std::vector<IncomingMessage> &) override
    {
        return InvokeResult::Commit;
    }
    InvokeResult invoke_consume(InvokeRx, std::vector<IncomingMessage> &) override
    {
        return InvokeResult::Commit;
    }
    InvokeResult invoke_process(InvokeRx, InvokeTx, std::vector<IncomingMessage> &) override
    {
        return InvokeResult::Commit;
    }
    InvokeResult invoke_on_inbox(InvokeInbox) override { return InvokeResult::Commit; }
    uint64_t script_error_count() const noexcept override { return 0; }
    bool supports_multi_state() const noexcept override { return false; }
};

// HEP-CORE-0011 §"Loop-ready gate" — StubEngine variant that reports
// a user-side on_init hook returning Ready.  Combined with
// FrameworkFloorHoldsOps, exercises the case "user script says Ready
// but framework floor says NotReady" — the loop MUST stay closed
// (AND, not OR).
struct StubEngineWithReadyHook : public StubEngine
{
    bool has_callback(const std::string &name) const noexcept override { return name == "on_init"; }
    pylabhub::scripting::ScriptEngine::InitStatus invoke_on_init() override
    {
        return pylabhub::scripting::ScriptEngine::InitStatus::Ready;
    }
};

/// Fresh RoleAPIBase per worker. The uid embeds the scenario name so each
/// subprocess registers a unique dynamic ThreadManager module — cosmetic in
/// a single-test subprocess but keeps logs grep-friendly.
std::unique_ptr<RoleAPIBase> make_api(RoleHostCore &core, const char *uid)
{
    return std::make_unique<RoleAPIBase>(core, "test", uid);
}

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
// ─────────────────────────────────────────────────────────────────────────
// L2 fixture step — install a legitimately-constructed Presence in
// `Authorized` state on the API so the HEP-CORE-0036 §8.2 outer guard
// admits the data loop.  NOT a bypass: the gate scans the presences we
// installed and finds an Authorized one because we put it there with
// the same atomic store production uses at the end of apply_*_reg_ack.
// All other Presence fields are dummies — the loop tests don't read
// them.  See `tests/test_framework/role_api_base_test_access.h` for
// the no-bypass contract.
//
// Symbol absent in Release / non-test builds (mirrors the friend +
// private-setter gating on RoleAPIBase).  Parent TEST_F bodies
// GTEST_SKIP before reaching the worker.
void install_one_authorized_presence(RoleAPIBase &api, const char *channel)
{
    using pylabhub::scripting::Presence;
    using pylabhub::scripting::RegistrationState;
    using pylabhub::scripting::RoleHandler;
    using pylabhub::scripting::RoleKind;
    using pylabhub::scripting::test::RoleAPIBaseTestAccess;

    Presence p;
    p.hub.broker = "tcp://l2-test:0";
    p.hub.broker_pubkey = "l2-test-pubkey";
    p.channel = channel;
    p.role_kind = RoleKind::Producer;
    // The actual write the gate observes.  Mirrors apply_*_reg_ack
    // exactly (role_api_base.cpp transitions).
    p.registration_state.store(RegistrationState::Authorized, std::memory_order_release);

    std::vector<Presence> presences;
    presences.push_back(std::move(p));

    auto handler = std::make_unique<RoleHandler>(std::move(presences));
    RoleAPIBaseTestAccess::install_handler(api, std::move(handler));
}
#endif

} // namespace

// ── run_data_loop scenarios ─────────────────────────────────────────────────
//
// Each scenario constructs a Presence(Authorized) via the L2-test
// helper above so the HEP-CORE-0036 §8.2 outer guard admits the loop.
// The gate runs unchanged — it scans the installed presences and
// reports `true` because the test put one in `Authorized` state with
// the same atomic store production uses at the end of apply_*_reg_ack.
//
// Scenario bodies are gated to the same build configuration as the
// test access friend (`PYLABHUB_BUILD_TESTS && !defined(NDEBUG)`).
// Outside that, the symbols are absent — parent TEST_F bodies in
// `test_role_data_loop.cpp` already GTEST_SKIP before spawning the
// worker, so the dispatcher never tries to route to a missing
// scenario.

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
int shutdown_stops_loop()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "shutdown_stops_loop");
            install_one_authorized_presence(*api, "ch.shutdown");

            StubEngine engine;
            MockCycleOps ops;
            core.set_running(true);
            std::thread stopper(
                [&]
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    core.request_stop();
                });

            LoopConfig cfg;
            cfg.period_us = 0;
            cfg.loop_timing = LoopTimingPolicy::MaxRate;

            run_data_loop(*api, core, cfg, ops, engine);
            stopper.join();

            EXPECT_GE(ops.acquire_count, 1);
            EXPECT_GE(ops.invoke_count, 1);
            EXPECT_EQ(ops.cleanup_exit_count, 1);
        },
        "role_data_loop::shutdown_stops_loop", Logger::GetLifecycleModule());
}

// HEP-CORE-0011 §"Loop-ready gate" — AND-composition invariant pin.
//
// Setup: Ops::default_init_ready returns false; engine reports a user
// on_init hook that returns Ready.  If the framework composes as
// `default && script`, the loop's init_done stays false and acquire is
// never called; the init_timeout budget elapses and the loop exits
// with `StopReason::InitTimeout`.
//
// Failure modes this catches:
//   1. Composition inverted from AND to OR — script Ready alone
//      would open the gate and `acquire_count > 0`.
//   2. Short-circuit that skips the framework default when a script
//      hook is present — same visible symptom.
//   3. Init timeout budget silently disabled — the loop would run
//      forever with acquire_count = 0 but no InitTimeout stop.
int framework_floor_holds_gate()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "framework_floor_holds_gate");
            install_one_authorized_presence(*api, "ch.floor_holds");

            StubEngineWithReadyHook engine;
            FrameworkFloorHoldsOps ops;
            core.set_running(true);

            LoopConfig cfg;
            cfg.period_us = 0;
            cfg.loop_timing = LoopTimingPolicy::MaxRate;
            // Small budget so the test completes quickly; the pre-Ready
            // cycle pacer runs at kLoopReadyGateInterval (100 ms), so
            // 500 ms allows a handful of paced cycles before the
            // InitTimeout fires.
            cfg.init_timeout_ms = 500;

            run_data_loop(*api, core, cfg, ops, engine);

            // Framework floor held → acquire never called.
            EXPECT_EQ(ops.acquire_count, 0)
                << "AND composition invariant broken: script Ready alone "
                   "opened the loop-ready gate";
            // Drain + invoke_and_commit still fires per cycle during
            // the pre-Ready hold (Step D+E runs unconditionally so
            // NOTIFYs can advance state).
            EXPECT_GE(ops.invoke_count, 1) << "pre-Ready phase must still drain messages";
            // Loop exited cleanly.
            EXPECT_EQ(ops.cleanup_exit_count, 1);
            // Stop reason must be InitTimeout — distinguishes this
            // failure mode from a generic critical error or a script
            // error.
            EXPECT_EQ(core.stop_reason(), RoleHostCore::StopReason::InitTimeout)
                << "loop-ready gate timeout must surface as "
                   "StopReason::InitTimeout";
        },
        "role_data_loop::framework_floor_holds_gate", Logger::GetLifecycleModule());
}

int invoke_returns_false_stops_loop()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "invoke_returns_false");
            install_one_authorized_presence(*api, "ch.invoke_false");

            StubEngine engine;
            MockCycleOps ops;
            ops.invoke_returns_continue = false;
            core.set_running(true);

            LoopConfig cfg;
            cfg.period_us = 0;
            cfg.loop_timing = LoopTimingPolicy::MaxRate;

            run_data_loop(*api, core, cfg, ops, engine);

            EXPECT_EQ(ops.acquire_count, 1);
            EXPECT_EQ(ops.invoke_count, 1);
            EXPECT_EQ(ops.cleanup_exit_count, 1);
        },
        "role_data_loop::invoke_returns_false_stops_loop", Logger::GetLifecycleModule());
}

int metrics_increment()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "metrics_increment");
            install_one_authorized_presence(*api, "ch.metrics");

            StubEngine engine;
            MockCycleOps ops;
            core.set_running(true);
            std::thread stopper(
                [&]
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    core.request_stop();
                });

            LoopConfig cfg;
            cfg.period_us = 0;
            cfg.loop_timing = LoopTimingPolicy::MaxRate;

            run_data_loop(*api, core, cfg, ops, engine);
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
            auto api = make_api(core, "no_data_skips_deadline_wait");
            install_one_authorized_presence(*api, "ch.no_data");

            StubEngine engine;
            MockCycleOps ops;
            ops.acquire_returns_data = false;
            core.set_running(true);
            std::thread stopper(
                [&]
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds{30});
                    core.request_stop();
                });

            LoopConfig cfg;
            cfg.period_us = 100000;
            cfg.loop_timing = LoopTimingPolicy::FixedRate;
            cfg.queue_io_wait_timeout_ratio = 0.1;

            auto start = std::chrono::steady_clock::now();
            run_data_loop(*api, core, cfg, ops, engine);
            stopper.join();
            auto elapsed = std::chrono::steady_clock::now() - start;

            EXPECT_GE(ops.acquire_count, 2);
            EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 200);
        },
        "role_data_loop::no_data_skips_deadline_wait", Logger::GetLifecycleModule());
}

int overrun_detected()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "overrun_detected");
            install_one_authorized_presence(*api, "ch.overrun");

            core.set_running(true);
            StubEngine engine;
            SlowOps slow_ops;

            LoopConfig cfg;
            cfg.period_us = 10000;
            cfg.loop_timing = LoopTimingPolicy::FixedRate;
            cfg.queue_io_wait_timeout_ratio = 0.1;

            run_data_loop(*api, core, cfg, slow_ops, engine);

            EXPECT_GE(core.loop_overrun_count(), 1u);
        },
        "role_data_loop::overrun_detected", Logger::GetLifecycleModule());
}
#endif // PYLABHUB_BUILD_TESTS && !NDEBUG

// ── ThreadManager scenarios ─────────────────────────────────────────────────

int thread_manager_spawn_and_join()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "tm_spawn_and_join");
            StubEngine engine;
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
        "role_data_loop::thread_manager_spawn_and_join", Logger::GetLifecycleModule());
}

int thread_manager_multiple_threads()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "tm_multiple_threads");
            StubEngine engine;
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
        "role_data_loop::thread_manager_multiple_threads", Logger::GetLifecycleModule());
}

int thread_manager_join_in_reverse_order()
{
    return run_gtest_worker(
        [&]()
        {
            RoleHostCore core;
            auto api = make_api(core, "tm_join_in_reverse_order");
            StubEngine engine;
            api->set_engine(&engine);

            core.set_running(true);
            std::vector<int> order;
            std::mutex mu;

            api->thread_manager().spawn("first",
                                        [&]
                                        {
                                            std::this_thread::sleep_for(
                                                std::chrono::milliseconds{5});
                                            std::lock_guard<std::mutex> lk(mu);
                                            order.push_back(1);
                                        });
            api->thread_manager().spawn("second",
                                        [&]
                                        {
                                            std::lock_guard<std::mutex> lk(mu);
                                            order.push_back(2);
                                        });

            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            api->thread_manager().drain();

            EXPECT_EQ(order.size(), 2u);
        },
        "role_data_loop::thread_manager_join_in_reverse_order", Logger::GetLifecycleModule());
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
                if (dot == std::string_view::npos || mode.substr(0, dot) != "role_data_loop")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_data_loop;

#if defined(PYLABHUB_BUILD_TESTS) && !defined(NDEBUG)
                if (sc == "shutdown_stops_loop")
                    return shutdown_stops_loop();
                if (sc == "invoke_returns_false_stops_loop")
                    return invoke_returns_false_stops_loop();
                if (sc == "framework_floor_holds_gate")
                    return framework_floor_holds_gate();
                if (sc == "metrics_increment")
                    return metrics_increment();
                if (sc == "no_data_skips_deadline_wait")
                    return no_data_skips_deadline_wait();
                if (sc == "overrun_detected")
                    return overrun_detected();
#endif
                if (sc == "thread_manager_spawn_and_join")
                    return thread_manager_spawn_and_join();
                if (sc == "thread_manager_multiple_threads")
                    return thread_manager_multiple_threads();
                if (sc == "thread_manager_join_in_reverse_order")
                    return thread_manager_join_in_reverse_order();

                fmt::print(stderr, "[role_data_loop] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static RoleDataLoopWorkerRegistrar g_role_data_loop_registrar;

} // namespace
