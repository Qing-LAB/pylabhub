/**
 * @file thread_manager_active_loop_workers.cpp
 * @brief Worker bodies for `ThreadManager` shutdown-contract tests
 *        (HEP-CORE-0031 §4.1 — Pattern 3).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern explicitly ruled out by
 * `docs/README/README_testing.md` § "Antipatterns".  Each body
 * constructs `ThreadManager`, which registers a dynamic lifecycle
 * module on `Logger` — the canonical trigger for Pattern 3.
 *
 * Workers that deliberately exercise the detach path emit
 * `LOGGER_ERROR` lines.  The parent driver declares those expected
 * substrings via `ExpectWorkerOk(w, {}, {<expected_errors…>})` so the
 * worker framework's unexpected-ERROR guard doesn't flag them.
 */

#include "thread_manager_active_loop_workers.h"

#include "utils/thread_manager.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using pylabhub::utils::Logger;
using pylabhub::utils::ThreadManager;
using pylabhub::tests::helper::run_gtest_worker;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker
{
namespace thread_manager_active_loop
{

// ───────────────────────────────────────────────────────────────────────────
// Monotonic-mark family (handy-case API)
// ───────────────────────────────────────────────────────────────────────────

int old_overload_flag_set_after_body_return()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "old_overload");

            auto stop = std::make_shared<std::atomic<bool>>(false);
            ASSERT_TRUE(tm.spawn("worker", [stop]() {
                while (!stop->load(std::memory_order_acquire))
                    std::this_thread::sleep_for(2ms);
            }));

            EXPECT_FALSE(tm.is_active_loop_exited("worker"))
                << "Flag must be false while body is running";

            stop->store(true, std::memory_order_release);

            EXPECT_TRUE(tm.wait_for_active_loop_exit("worker", 500ms))
                << "Old overload: wrapper must mark active_loop_exited after body";

            EXPECT_TRUE(tm.is_active_loop_exited("worker"));
        },
        "thread_manager_active_loop::old_overload_flag_set_after_body_return",
        Logger::GetLifecycleModule());
}

int new_overload_early_mark_observable_before_body_return()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "early_mark");

            auto exit_loop    = std::make_shared<std::atomic<bool>>(false);
            auto release_body = std::make_shared<std::atomic<bool>>(false);

            ASSERT_TRUE(tm.spawn("worker",
                [exit_loop, release_body](ThreadManager::SlotContext &ctx) {
                    while (!exit_loop->load(std::memory_order_acquire))
                        std::this_thread::sleep_for(2ms);
                    ctx.mark_active_loop_exited();
                    while (!release_body->load(std::memory_order_acquire))
                        std::this_thread::sleep_for(2ms);
                }));

            EXPECT_FALSE(tm.is_active_loop_exited("worker"));

            exit_loop->store(true, std::memory_order_release);

            const auto t0 = std::chrono::steady_clock::now();
            EXPECT_TRUE(tm.wait_for_active_loop_exit("worker", 500ms))
                << "Flag must be observable as soon as thread marks it";
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            EXPECT_LT(elapsed, 100ms)
                << "Flag observation should be prompt (well under timeout)";

            EXPECT_EQ(tm.active_count(), 1u)
                << "Body has not yet returned; slot still tracked";

            release_body->store(true, std::memory_order_release);
            EXPECT_EQ(tm.drain(), 0u)
                << "After release, body returns and drain completes cleanly";
        },
        "thread_manager_active_loop::new_overload_early_mark_observable_before_body_return",
        Logger::GetLifecycleModule());
}

int is_active_loop_exited_unknown_name_returns_false()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "unknown");
            EXPECT_FALSE(tm.is_active_loop_exited("nonexistent"));
        },
        "thread_manager_active_loop::is_active_loop_exited_unknown_name_returns_false",
        Logger::GetLifecycleModule());
}

int wait_for_active_loop_exit_unknown_name_returns_false_fast()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "wait_unknown");

            const auto t0 = std::chrono::steady_clock::now();
            EXPECT_FALSE(tm.wait_for_active_loop_exit("nonexistent", 500ms));
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            EXPECT_LT(elapsed, 50ms)
                << "Unknown name must return false immediately (no polling)";
        },
        "thread_manager_active_loop::wait_for_active_loop_exit_unknown_name_returns_false_fast",
        Logger::GetLifecycleModule());
}

int wait_for_active_loop_exit_timeout_returns_false()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "wait_timeout");

            auto stop = std::make_shared<std::atomic<bool>>(false);
            ASSERT_TRUE(tm.spawn("worker",
                [stop](ThreadManager::SlotContext &) {
                    while (!stop->load(std::memory_order_acquire))
                        std::this_thread::sleep_for(2ms);
                }));

            const auto t0 = std::chrono::steady_clock::now();
            EXPECT_FALSE(tm.wait_for_active_loop_exit("worker", 100ms));
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            EXPECT_GE(elapsed, 100ms) << "Must wait for the full timeout";
            EXPECT_LT(elapsed, 200ms)
                << "Must not over-wait beyond timeout + poll granularity";

            stop->store(true, std::memory_order_release);
            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::wait_for_active_loop_exit_timeout_returns_false",
        Logger::GetLifecycleModule());
}

int mark_active_loop_exited_idempotent()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "idempotent");

            auto release = std::make_shared<std::atomic<bool>>(false);
            ASSERT_TRUE(tm.spawn("worker",
                [release](ThreadManager::SlotContext &ctx) {
                    ctx.mark_active_loop_exited();
                    ctx.mark_active_loop_exited();
                    ctx.mark_active_loop_exited();
                    while (!release->load(std::memory_order_acquire))
                        std::this_thread::sleep_for(2ms);
                }));

            EXPECT_TRUE(tm.wait_for_active_loop_exit("worker", 500ms));
            release->store(true, std::memory_order_release);
            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::mark_active_loop_exited_idempotent",
        Logger::GetLifecycleModule());
}

int drain_fast_path_already_exited()
{
    return run_gtest_worker(
        [] {
            ThreadManager::reset_process_detached_count_for_testing();
            ThreadManager tm("test", "fast_path");

            ASSERT_TRUE(tm.spawn("quick",
                [](ThreadManager::SlotContext &ctx) {
                    ctx.mark_active_loop_exited();
                }));

            std::this_thread::sleep_for(50ms);

            const auto t0 = std::chrono::steady_clock::now();
            const auto detached = tm.drain();
            const auto elapsed = std::chrono::steady_clock::now() - t0;

            EXPECT_EQ(detached, 0u) << "Clean drain";
            EXPECT_LT(elapsed, 100ms)
                << "Fast path: active_loop_exited already set, no Stage 1 polling";
        },
        "thread_manager_active_loop::drain_fast_path_already_exited",
        Logger::GetLifecycleModule());
}

// ───────────────────────────────────────────────────────────────────────────
// drain() two-stage diagnostics — deliberately exercise the detach path
// and emit a stage-differentiated LOGGER_ERROR.  Parent driver declares
// the expected ERROR substrings via ExpectWorkerOk's expected_errors arg.
// ───────────────────────────────────────────────────────────────────────────

int drain_stuck_in_active_loop_detaches_with_active_loop_diagnostic()
{
    return run_gtest_worker(
        [] {
            ThreadManager::reset_process_detached_count_for_testing();
            ThreadManager tm("test", "stuck_active");

            auto stop            = std::make_shared<std::atomic<bool>>(false);
            auto entered_bracket = std::make_shared<std::atomic<bool>>(false);
            auto exited_bracket  = std::make_shared<std::atomic<bool>>(false);

            ThreadManager::SpawnOptions opts;
            opts.join_timeout = 100ms;
            ASSERT_TRUE(tm.spawn("stuck",
                [stop, entered_bracket, exited_bracket](
                    ThreadManager::SlotContext &ctx) {
                    ctx.with_active_loop([&] {
                        entered_bracket->store(true, std::memory_order_release);
                        while (!stop->load(std::memory_order_acquire))
                            std::this_thread::sleep_for(2ms);
                    });
                    exited_bracket->store(true, std::memory_order_release);
                },
                opts));

            const auto enter_deadline = std::chrono::steady_clock::now() + 1s;
            while (!entered_bracket->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < enter_deadline)
                std::this_thread::sleep_for(2ms);
            ASSERT_TRUE(entered_bracket->load(std::memory_order_acquire))
                << "Thread never entered with_active_loop bracket";

            const auto t0       = std::chrono::steady_clock::now();
            const auto detached = tm.drain();
            const auto elapsed  = std::chrono::steady_clock::now() - t0;

            EXPECT_EQ(detached, 1u) << "Stuck thread detached after timeout";
            EXPECT_EQ(ThreadManager::process_detached_count(), 1u);
            EXPECT_FALSE(exited_bracket->load(std::memory_order_acquire))
                << "Thread must have been detached WHILE still inside bracket";
            EXPECT_LT(elapsed, 1s) << "Drain hung past sanity bound";

            stop->store(true, std::memory_order_release);
            const auto exit_deadline = std::chrono::steady_clock::now() + 2s;
            while (!exited_bracket->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < exit_deadline)
                std::this_thread::sleep_for(5ms);

            // Clear detach counter before LifecycleGuard's leak check.
            ThreadManager::reset_process_detached_count_for_testing();
        },
        "thread_manager_active_loop::drain_stuck_in_active_loop",
        Logger::GetLifecycleModule());
}

int drain_stuck_in_post_loop_detaches_with_post_loop_diagnostic()
{
    return run_gtest_worker(
        [] {
            ThreadManager::reset_process_detached_count_for_testing();
            ThreadManager tm("test", "stuck_postloop");

            auto release_body = std::make_shared<std::atomic<bool>>(false);
            ThreadManager::SpawnOptions opts;
            opts.join_timeout = 100ms;
            ASSERT_TRUE(tm.spawn("postloop",
                [release_body](ThreadManager::SlotContext &ctx) {
                    ctx.mark_active_loop_exited();
                    while (!release_body->load(std::memory_order_acquire))
                        std::this_thread::sleep_for(2ms);
                },
                opts));

            std::this_thread::sleep_for(30ms);
            ASSERT_TRUE(tm.is_active_loop_exited("postloop"))
                << "Body should have marked active_loop_exited before drain";

            const auto t0       = std::chrono::steady_clock::now();
            const auto detached = tm.drain();
            const auto elapsed  = std::chrono::steady_clock::now() - t0;

            EXPECT_EQ(detached, 1u)
                << "Post-loop-stuck thread detached after Stage 2 timeout";
            EXPECT_EQ(ThreadManager::process_detached_count(), 1u);
            EXPECT_LT(elapsed, 150ms)
                << "Stage 1 fast path: drain should finish in ~Stage 2 time only";

            release_body->store(true, std::memory_order_release);
            std::this_thread::sleep_for(50ms);

            ThreadManager::reset_process_detached_count_for_testing();
        },
        "thread_manager_active_loop::drain_stuck_in_post_loop",
        Logger::GetLifecycleModule());
}

// ───────────────────────────────────────────────────────────────────────────
// Transactional family (with_active_loop + per-slot shutdown_requested +
// request_shutdown_all + wait_for_quiescence)
// ───────────────────────────────────────────────────────────────────────────

int with_active_loop_bracket_toggles_in_active_loop()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "bracket_toggles");

            auto release         = std::make_shared<std::atomic<bool>>(false);
            auto entered_bracket = std::make_shared<std::atomic<bool>>(false);
            auto exited_bracket  = std::make_shared<std::atomic<bool>>(false);

            ASSERT_TRUE(tm.spawn("worker",
                [release, entered_bracket, exited_bracket](
                    ThreadManager::SlotContext &ctx) {
                    ctx.with_active_loop([&] {
                        entered_bracket->store(true, std::memory_order_release);
                        while (!release->load(std::memory_order_acquire))
                            std::this_thread::sleep_for(2ms);
                    });
                    exited_bracket->store(true, std::memory_order_release);
                }));

            const auto enter_dl = std::chrono::steady_clock::now() + 1s;
            while (!entered_bracket->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < enter_dl)
                std::this_thread::sleep_for(2ms);
            ASSERT_TRUE(entered_bracket->load(std::memory_order_acquire));

            EXPECT_EQ(tm.wait_for_quiescence(50ms), 1u)
                << "Thread in bracket must register as non-quiescent";

            release->store(true, std::memory_order_release);
            EXPECT_EQ(tm.wait_for_quiescence(1s), 0u)
                << "After body exits bracket, wait_for_quiescence must return 0";

            const auto exit_dl = std::chrono::steady_clock::now() + 1s;
            while (!exited_bracket->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < exit_dl)
                std::this_thread::sleep_for(2ms);
            EXPECT_TRUE(exited_bracket->load(std::memory_order_acquire));
            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::with_active_loop_bracket_toggles_in_active_loop",
        Logger::GetLifecycleModule());
}

int with_active_loop_skips_body_if_shutdown_requested_before_entry()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "skip_body");

            auto start_gate = std::make_shared<std::atomic<bool>>(false);
            auto body_ran   = std::make_shared<std::atomic<bool>>(false);

            ASSERT_TRUE(tm.spawn("worker",
                [start_gate, body_ran](ThreadManager::SlotContext &ctx) {
                    while (!start_gate->load(std::memory_order_acquire))
                        std::this_thread::sleep_for(2ms);
                    ctx.with_active_loop([&] {
                        body_ran->store(true, std::memory_order_release);
                    });
                }));

            EXPECT_TRUE(tm.request_shutdown("worker"));
            start_gate->store(true, std::memory_order_release);

            EXPECT_EQ(tm.drain(), 0u);
            EXPECT_FALSE(body_ran->load(std::memory_order_acquire))
                << "with_active_loop must skip body when shutdown_requested is "
                   "set at entry";
        },
        "thread_manager_active_loop::with_active_loop_skips_body_if_shutdown_requested_before_entry",
        Logger::GetLifecycleModule());
}

int with_active_loop_raii_reset_on_exception()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "raii_throw");

            auto entered = std::make_shared<std::atomic<bool>>(false);

            ASSERT_TRUE(tm.spawn("thrower",
                [entered](ThreadManager::SlotContext &ctx) {
                    ctx.with_active_loop([&] {
                        entered->store(true, std::memory_order_release);
                        throw std::runtime_error("simulated bracket throw");
                    });
                }));

            const auto dl = std::chrono::steady_clock::now() + 1s;
            while (!entered->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < dl)
                std::this_thread::sleep_for(2ms);
            ASSERT_TRUE(entered->load(std::memory_order_acquire));

            EXPECT_EQ(tm.wait_for_quiescence(1s), 0u)
                << "RAII reset must restore in_active_loop=false after throw";
            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::with_active_loop_raii_reset_on_exception",
        Logger::GetLifecycleModule());
}

int shutdown_requested_thread_side_poll_observes_flag()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "thread_poll");

            auto saw_shutdown    = std::make_shared<std::atomic<bool>>(false);
            auto entered_bracket = std::make_shared<std::atomic<bool>>(false);

            ASSERT_TRUE(tm.spawn("worker",
                [saw_shutdown, entered_bracket](ThreadManager::SlotContext &ctx) {
                    ctx.with_active_loop([&] {
                        entered_bracket->store(true, std::memory_order_release);
                        while (!ctx.shutdown_requested())
                            std::this_thread::sleep_for(2ms);
                        saw_shutdown->store(true, std::memory_order_release);
                    });
                }));

            const auto enter_dl = std::chrono::steady_clock::now() + 1s;
            while (!entered_bracket->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < enter_dl)
                std::this_thread::sleep_for(2ms);
            ASSERT_TRUE(entered_bracket->load(std::memory_order_acquire));

            EXPECT_FALSE(saw_shutdown->load(std::memory_order_acquire))
                << "shutdown_requested must read false before request_shutdown";

            EXPECT_TRUE(tm.request_shutdown("worker"));

            const auto saw_dl = std::chrono::steady_clock::now() + 1s;
            while (!saw_shutdown->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < saw_dl)
                std::this_thread::sleep_for(2ms);
            EXPECT_TRUE(saw_shutdown->load(std::memory_order_acquire))
                << "Thread-side poll did not observe the flag";

            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::shutdown_requested_thread_side_poll_observes_flag",
        Logger::GetLifecycleModule());
}

int request_shutdown_unknown_name_returns_false()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "unknown_signal");
            EXPECT_FALSE(tm.request_shutdown("nonexistent"));
        },
        "thread_manager_active_loop::request_shutdown_unknown_name_returns_false",
        Logger::GetLifecycleModule());
}

int request_shutdown_all_flips_closing_and_rejects_new_spawn()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "shutdown_all");

            auto saw_shutdown    = std::make_shared<std::atomic<bool>>(false);
            auto entered_bracket = std::make_shared<std::atomic<bool>>(false);

            ASSERT_TRUE(tm.spawn("worker",
                [saw_shutdown, entered_bracket](ThreadManager::SlotContext &ctx) {
                    ctx.with_active_loop([&] {
                        entered_bracket->store(true, std::memory_order_release);
                        while (!ctx.shutdown_requested())
                            std::this_thread::sleep_for(2ms);
                        saw_shutdown->store(true, std::memory_order_release);
                    });
                }));

            const auto enter_dl = std::chrono::steady_clock::now() + 1s;
            while (!entered_bracket->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < enter_dl)
                std::this_thread::sleep_for(2ms);
            ASSERT_TRUE(entered_bracket->load(std::memory_order_acquire));

            EXPECT_EQ(tm.request_shutdown_all(), 1u)
                << "Should report the one existing slot's flag was newly set";

            EXPECT_FALSE(tm.spawn("late",
                [](ThreadManager::SlotContext &) {}))
                << "spawn() after request_shutdown_all must be rejected";

            const auto saw_dl = std::chrono::steady_clock::now() + 1s;
            while (!saw_shutdown->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < saw_dl)
                std::this_thread::sleep_for(2ms);
            EXPECT_TRUE(saw_shutdown->load(std::memory_order_acquire));
            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::request_shutdown_all_flips_closing_and_rejects_new_spawn",
        Logger::GetLifecycleModule());
}

int wait_for_quiescence_default_safe_threads_pass_instantly()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "default_safe");

            auto release = std::make_shared<std::atomic<bool>>(false);
            ASSERT_TRUE(tm.spawn("worker",
                [release](ThreadManager::SlotContext &) {
                    while (!release->load(std::memory_order_acquire))
                        std::this_thread::sleep_for(2ms);
                }));

            EXPECT_EQ(tm.wait_for_quiescence(1s), 0u);

            release->store(true, std::memory_order_release);
            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::wait_for_quiescence_default_safe_threads_pass_instantly",
        Logger::GetLifecycleModule());
}

int wait_for_quiescence_excludes_calling_thread()
{
    return run_gtest_worker(
        [] {
            ThreadManager tm("test", "self_exclude");

            auto wait_done   = std::make_shared<std::atomic<bool>>(false);
            auto wait_result = std::make_shared<std::atomic<std::size_t>>(99);
            auto entered     = std::make_shared<std::atomic<bool>>(false);

            ASSERT_TRUE(tm.spawn("self",
                [&tm, wait_done, wait_result, entered](
                    ThreadManager::SlotContext &ctx) {
                    ctx.with_active_loop([&] {
                        entered->store(true, std::memory_order_release);
                        const auto r = tm.wait_for_quiescence(50ms);
                        wait_result->store(r, std::memory_order_release);
                        wait_done->store(true, std::memory_order_release);
                    });
                }));

            const auto dl = std::chrono::steady_clock::now() + 2s;
            while (!wait_done->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < dl)
                std::this_thread::sleep_for(2ms);

            ASSERT_TRUE(entered->load(std::memory_order_acquire));
            ASSERT_TRUE(wait_done->load(std::memory_order_acquire))
                << "Calling thread must NOT have self-deadlocked";
            EXPECT_EQ(wait_result->load(std::memory_order_acquire), 0u)
                << "Calling thread must be excluded from quiescence count";

            EXPECT_EQ(tm.drain(), 0u);
        },
        "thread_manager_active_loop::wait_for_quiescence_excludes_calling_thread",
        Logger::GetLifecycleModule());
}

} // namespace thread_manager_active_loop
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ─────────────────────────────────────────────────────

namespace
{

struct ThreadManagerActiveLoopRegistrar
{
    ThreadManagerActiveLoopRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "thread_manager_active_loop")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::thread_manager_active_loop;

                if (sc == "old_overload_flag_set_after_body_return")
                    return old_overload_flag_set_after_body_return();
                if (sc == "new_overload_early_mark_observable_before_body_return")
                    return new_overload_early_mark_observable_before_body_return();
                if (sc == "is_active_loop_exited_unknown_name_returns_false")
                    return is_active_loop_exited_unknown_name_returns_false();
                if (sc == "wait_for_active_loop_exit_unknown_name_returns_false_fast")
                    return wait_for_active_loop_exit_unknown_name_returns_false_fast();
                if (sc == "wait_for_active_loop_exit_timeout_returns_false")
                    return wait_for_active_loop_exit_timeout_returns_false();
                if (sc == "mark_active_loop_exited_idempotent")
                    return mark_active_loop_exited_idempotent();
                if (sc == "drain_fast_path_already_exited")
                    return drain_fast_path_already_exited();
                if (sc == "drain_stuck_in_active_loop")
                    return drain_stuck_in_active_loop_detaches_with_active_loop_diagnostic();
                if (sc == "drain_stuck_in_post_loop")
                    return drain_stuck_in_post_loop_detaches_with_post_loop_diagnostic();
                if (sc == "with_active_loop_bracket_toggles_in_active_loop")
                    return with_active_loop_bracket_toggles_in_active_loop();
                if (sc == "with_active_loop_skips_body_if_shutdown_requested_before_entry")
                    return with_active_loop_skips_body_if_shutdown_requested_before_entry();
                if (sc == "with_active_loop_raii_reset_on_exception")
                    return with_active_loop_raii_reset_on_exception();
                if (sc == "shutdown_requested_thread_side_poll_observes_flag")
                    return shutdown_requested_thread_side_poll_observes_flag();
                if (sc == "request_shutdown_unknown_name_returns_false")
                    return request_shutdown_unknown_name_returns_false();
                if (sc == "request_shutdown_all_flips_closing_and_rejects_new_spawn")
                    return request_shutdown_all_flips_closing_and_rejects_new_spawn();
                if (sc == "wait_for_quiescence_default_safe_threads_pass_instantly")
                    return wait_for_quiescence_default_safe_threads_pass_instantly();
                if (sc == "wait_for_quiescence_excludes_calling_thread")
                    return wait_for_quiescence_excludes_calling_thread();

                fmt::print(stderr,
                           "[thread_manager_active_loop] ERROR: "
                           "unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static ThreadManagerActiveLoopRegistrar g_thread_manager_active_loop_registrar;

} // namespace
