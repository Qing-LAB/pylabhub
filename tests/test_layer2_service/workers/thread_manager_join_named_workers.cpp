/**
 * @file thread_manager_join_named_workers.cpp
 * @brief Worker bodies for `ThreadManager::join_named` tests (Pattern 3).
 *
 * Why subprocess: each body constructs `ThreadManager`, which registers
 * a dynamic lifecycle module on `Logger`.  Per HEP-CORE-0001 §
 * "Testing implications" + `docs/README/README_testing.md` §
 * "Choosing a test pattern", any test transitively reaching a
 * lifecycle module must run in a worker subprocess.  Migrated from
 * the pre-existing in-process `SetUpTestSuite`-owned `LifecycleGuard`
 * antipattern 2026-05-13.
 */

#include "thread_manager_join_named_workers.h"

#include "utils/thread_manager.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using pylabhub::utils::Logger;
using pylabhub::utils::ThreadManager;
using pylabhub::tests::helper::run_gtest_worker;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker
{
namespace thread_manager_join_named
{

// ── Helpers (per-worker — each subprocess gets its own copy) ────────────────

namespace
{

/// Spawn a "good citizen" thread that exits when its `stop` flag flips.
std::shared_ptr<std::atomic<bool>>
spawn_cooperating(ThreadManager &tm, const std::string &name,
                   std::chrono::milliseconds join_timeout = 500ms)
{
    auto stop = std::make_shared<std::atomic<bool>>(false);
    ThreadManager::SpawnOptions opts;
    opts.join_timeout = join_timeout;
    const bool ok = tm.spawn(name,
        [stop] {
            while (!stop->load(std::memory_order_acquire))
                std::this_thread::sleep_for(5ms);
        },
        opts);
    EXPECT_TRUE(ok) << "spawn(" << name << ") failed";
    return stop;
}

/// Spawn an "uncooperative" thread that ignores stop signals for
/// `run_for` ms.  Used to exercise the bounded-join expiry path.
void spawn_uncooperative(ThreadManager &tm, const std::string &name,
                          std::chrono::milliseconds run_for,
                          std::chrono::milliseconds join_timeout)
{
    ThreadManager::SpawnOptions opts;
    opts.join_timeout = join_timeout;
    const bool ok = tm.spawn(name,
        [run_for] { std::this_thread::sleep_for(run_for); },
        opts);
    EXPECT_TRUE(ok);
}

} // namespace

// ── Scenarios ────────────────────────────────────────────────────────────────

int happy_path_signal_then_join()
{
    return run_gtest_worker(
        [&] {
            ThreadManager tm("test", "happy");
            auto stop = spawn_cooperating(tm, "alpha");
            EXPECT_EQ(tm.active_count(), 1u);

            stop->store(true, std::memory_order_release);

            const auto t0     = std::chrono::steady_clock::now();
            const bool joined = tm.join_named("alpha");
            const auto elapsed =
                std::chrono::steady_clock::now() - t0;

            EXPECT_TRUE(joined);
            EXPECT_LT(elapsed, 100ms) << "join_named took too long — "
                                         "likely went down the timeout/detach path";
            EXPECT_EQ(tm.active_count(), 0u);
        },
        "thread_manager_join_named::happy_path_signal_then_join",
        Logger::GetLifecycleModule());
}

int unknown_name_returns_false()
{
    return run_gtest_worker(
        [&] {
            ThreadManager tm("test", "unknown");
            auto stop = spawn_cooperating(tm, "alpha");
            EXPECT_EQ(tm.active_count(), 1u);

            EXPECT_FALSE(tm.join_named("does_not_exist"));
            EXPECT_EQ(tm.active_count(), 1u);  // alpha still tracked

            stop->store(true, std::memory_order_release);
            EXPECT_TRUE(tm.join_named("alpha"));
        },
        "thread_manager_join_named::unknown_name_returns_false",
        Logger::GetLifecycleModule());
}

int idempotent_second_call()
{
    return run_gtest_worker(
        [&] {
            ThreadManager tm("test", "idem");
            auto stop = spawn_cooperating(tm, "alpha");
            stop->store(true, std::memory_order_release);

            EXPECT_TRUE(tm.join_named("alpha"));
            EXPECT_EQ(tm.active_count(), 0u);

            // Slot already removed — second call must be a no-op false.
            EXPECT_FALSE(tm.join_named("alpha"));
        },
        "thread_manager_join_named::idempotent_second_call",
        Logger::GetLifecycleModule());
}

int uncooperative_thread_detached()
{
    return run_gtest_worker(
        [&] {
            ThreadManager tm("test", "uncoop");
            spawn_uncooperative(tm, "stuck", 1000ms, 100ms);

            const auto t0      = std::chrono::steady_clock::now();
            const bool joined  = tm.join_named("stuck");
            const auto elapsed = std::chrono::steady_clock::now() - t0;

            EXPECT_FALSE(joined)
                << "Path discriminator: detached, not joined cleanly";
            EXPECT_EQ(ThreadManager::process_detached_count(), 1u)
                << "Path discriminator: process counted exactly one detach";
            EXPECT_EQ(tm.active_count(), 0u) << "Slot removed after detach";
            // Anti-hang outer bound — catches regression to "wait for
            // the full sleep" without pinning a specific stage timing.
            EXPECT_LT(elapsed, 500ms) << "Regression to full-sleep wait";

            // Let the detached thread finish before LifecycleGuard's
            // dtor + ThreadManager leak-counter check fire.
            std::this_thread::sleep_for(1100ms);
            ThreadManager::reset_process_detached_count_for_testing();
        },
        "thread_manager_join_named::uncooperative_thread_detached",
        Logger::GetLifecycleModule());
}

int bracketed_thread_observes_internal_signal()
{
    return run_gtest_worker(
        [&] {
            // join_named internally sets the slot's
            // shutdown_requested before the bounded join.  A
            // bracketed body that polls `ctx.shutdown_requested()`
            // observes the signal, exits its bracket, body returns —
            // join completes cleanly without detach.
            ThreadManager tm("test", "bracket_join");

            auto saw_shutdown    = std::make_shared<std::atomic<bool>>(false);
            auto entered_bracket = std::make_shared<std::atomic<bool>>(false);

            ThreadManager::SpawnOptions opts;
            opts.join_timeout = 500ms;
            ASSERT_TRUE(tm.spawn("worker",
                [saw_shutdown, entered_bracket](
                    ThreadManager::SlotContext &ctx) {
                    ctx.with_active_loop([&] {
                        entered_bracket->store(true,
                                               std::memory_order_release);
                        while (!ctx.shutdown_requested())
                            std::this_thread::sleep_for(2ms);
                        saw_shutdown->store(true,
                                            std::memory_order_release);
                    });
                },
                opts));

            const auto enter_dl = std::chrono::steady_clock::now() + 1s;
            while (!entered_bracket->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < enter_dl)
                std::this_thread::sleep_for(2ms);
            ASSERT_TRUE(entered_bracket->load(std::memory_order_acquire));

            EXPECT_TRUE(tm.join_named("worker"));
            EXPECT_EQ(tm.active_count(), 0u);
            EXPECT_TRUE(saw_shutdown->load(std::memory_order_acquire))
                << "Path discriminator: thread observed the per-slot "
                   "shutdown_requested set by join_named internally";
            EXPECT_EQ(ThreadManager::process_detached_count(), 0u)
                << "Body returned via the bracket, not detached";
        },
        "thread_manager_join_named::bracketed_thread_observes_internal_signal",
        Logger::GetLifecycleModule());
}

int cooperates_with_drain()
{
    return run_gtest_worker(
        [&] {
            ThreadManager tm("test", "coop_drain");
            auto a = spawn_cooperating(tm, "alpha");
            auto b = spawn_cooperating(tm, "beta");
            auto c = spawn_cooperating(tm, "gamma");
            EXPECT_EQ(tm.active_count(), 3u);

            // Single-thread drain via join_named.
            a->store(true);
            EXPECT_TRUE(tm.join_named("alpha"));
            EXPECT_EQ(tm.active_count(), 2u);

            // drain() should still find beta + gamma.
            b->store(true);
            c->store(true);
            EXPECT_EQ(tm.drain(), 0u);  // 0 detached = clean drain
            EXPECT_EQ(tm.active_count(), 0u);
        },
        "thread_manager_join_named::cooperates_with_drain",
        Logger::GetLifecycleModule());
}

int after_drain_refuses_new_join()
{
    return run_gtest_worker(
        [&] {
            ThreadManager tm("test", "post_drain");
            auto stop = spawn_cooperating(tm, "alpha");
            stop->store(true, std::memory_order_release);
            EXPECT_EQ(tm.drain(), 0u);

            // After drain, the manager's `closing` flag is set.
            // join_named must refuse — same contract as spawn().
            EXPECT_FALSE(tm.join_named("alpha"));
            EXPECT_FALSE(tm.join_named("anything"));
        },
        "thread_manager_join_named::after_drain_refuses_new_join",
        Logger::GetLifecycleModule());
}

} // namespace thread_manager_join_named
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ─────────────────────────────────────────────────────

namespace
{

struct ThreadManagerJoinNamedRegistrar
{
    ThreadManagerJoinNamedRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "thread_manager_join_named")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::thread_manager_join_named;

                if (sc == "happy_path_signal_then_join")
                    return happy_path_signal_then_join();
                if (sc == "unknown_name_returns_false")
                    return unknown_name_returns_false();
                if (sc == "idempotent_second_call")
                    return idempotent_second_call();
                if (sc == "uncooperative_thread_detached")
                    return uncooperative_thread_detached();
                if (sc == "bracketed_thread_observes_internal_signal")
                    return bracketed_thread_observes_internal_signal();
                if (sc == "cooperates_with_drain")
                    return cooperates_with_drain();
                if (sc == "after_drain_refuses_new_join")
                    return after_drain_refuses_new_join();

                fmt::print(stderr,
                           "[thread_manager_join_named] ERROR: "
                           "unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static ThreadManagerJoinNamedRegistrar g_thread_manager_join_named_registrar;

} // namespace
