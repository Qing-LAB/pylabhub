/**
 * @file test_thread_manager_join_named.cpp
 * @brief Targeted tests for ThreadManager::join_named (HEP-CORE-0033 §4.2
 *        step 2 — Phase 8c shutdown ordering).
 *
 * `join_named` is the primitive that lets HubHost drain admin in-flight
 * RPCs synchronously before destroying HubAPI in `runner->shutdown_()`
 * (avoids the lifetime race where an admin thread mid-augment_* would
 * dereference a freed HubAPI).  These tests pin the contract:
 *
 *   - Happy path: signal → bounded join returns true; slot removed.
 *   - Unknown name: returns false without side effects.
 *   - Idempotent: second call returns false (slot already removed).
 *   - Bounded-join expiry: thread that ignores the stop signal is
 *     detached after `join_timeout`; returns false.
 *   - Cooperates with later drain(): unjoined siblings still drain.
 *   - After drain(): join_named refuses (closing flag set).
 *
 * Each assertion pins both the boolean outcome AND a wall-clock bound
 * (or a path-discriminating side effect) so a regression that takes
 * the wrong path can't silently pass — see CLAUDE.md "Tests must pin
 * path, timing, and structure" rule.
 */

#include "utils/thread_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pylabhub::utils;
using namespace std::chrono_literals;

namespace
{

// ──────────────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────────────

/// Spawn a "good citizen" thread that exits when its `stop` flag flips.
/// Returns the flag handle so the test can flip it.
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
/// @p run_for ms.  Used to exercise the bounded-join expiry path.
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

// ============================================================================
// Tests
// ============================================================================

TEST(ThreadManagerJoinNamedTest, HappyPath_SignalThenJoin_ReturnsTrue)
{
    ThreadManager tm("test", "happy");
    auto stop = spawn_cooperating(tm, "alpha");
    EXPECT_EQ(tm.active_count(), 1u);

    // Signal the thread to exit.
    stop->store(true, std::memory_order_release);

    // join_named should observe the done flag promptly (the thread
    // returns within one 5ms sleep slice).  Bound the wall-clock to
    // confirm the bounded-join did NOT have to wait for a timeout
    // detach — that would have taken ~500ms.
    const auto t0 = std::chrono::steady_clock::now();
    const bool joined = tm.join_named("alpha");
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_TRUE(joined);
    EXPECT_LT(elapsed, 100ms)
        << "join_named took too long — likely went down the timeout/detach path";
    EXPECT_EQ(tm.active_count(), 0u);
}

TEST(ThreadManagerJoinNamedTest, UnknownName_ReturnsFalse_NoSideEffects)
{
    ThreadManager tm("test", "unknown");
    auto stop = spawn_cooperating(tm, "alpha");
    EXPECT_EQ(tm.active_count(), 1u);

    EXPECT_FALSE(tm.join_named("does_not_exist"));
    EXPECT_EQ(tm.active_count(), 1u);  // alpha still tracked

    // Cleanup.
    stop->store(true, std::memory_order_release);
    EXPECT_TRUE(tm.join_named("alpha"));
}

TEST(ThreadManagerJoinNamedTest, Idempotent_SecondCallReturnsFalse)
{
    ThreadManager tm("test", "idem");
    auto stop = spawn_cooperating(tm, "alpha");
    stop->store(true, std::memory_order_release);

    EXPECT_TRUE(tm.join_named("alpha"));
    EXPECT_EQ(tm.active_count(), 0u);

    // Slot already removed — second call must be a no-op false.
    EXPECT_FALSE(tm.join_named("alpha"));
}

TEST(ThreadManagerJoinNamedTest, UncooperativeThread_DetachedAfterTimeout)
{
    ThreadManager tm("test", "uncoop");
    // Thread sleeps for 1000ms; join timeout is 100ms — must detach.
    spawn_uncooperative(tm, "stuck", 1000ms, 100ms);

    const auto t0 = std::chrono::steady_clock::now();
    const bool joined = tm.join_named("stuck");
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_FALSE(joined);  // detached, not joined cleanly
    // Detach happens at deadline; bound it generously to avoid CI flake
    // but tight enough that a regression to "wait for the full sleep"
    // (1000ms) would fail.
    EXPECT_GE(elapsed, 100ms);
    EXPECT_LT(elapsed, 300ms);
    EXPECT_EQ(tm.active_count(), 0u);  // slot removed regardless

    // Wait for the actual thread to finish so it doesn't outlive the
    // test (the std::thread is detached but still running); the
    // shared_ptr<done> the slot held keeps the atomic alive even
    // after the slot was destroyed, so the thread can still write
    // to it on its way out.
    std::this_thread::sleep_for(1100ms);

    // We deliberately exercised the detach path — clear the process-
    // wide unclean-shutdown counter so the gtest harness's check at
    // teardown doesn't fail this test on the leaked-thread guard.
    ThreadManager::reset_process_detached_count_for_testing();
}

TEST(ThreadManagerJoinNamedTest, CooperatesWithDrain_UnjoinedSiblingsDrain)
{
    ThreadManager tm("test", "coop_drain");
    auto a = spawn_cooperating(tm, "alpha");
    auto b = spawn_cooperating(tm, "beta");
    auto c = spawn_cooperating(tm, "gamma");
    EXPECT_EQ(tm.active_count(), 3u);

    // Single-thread drain via join_named.
    a->store(true);
    EXPECT_TRUE(tm.join_named("alpha"));
    EXPECT_EQ(tm.active_count(), 2u);

    // drain() should still find beta + gamma — must signal them first
    // (drain doesn't signal, only joins).
    b->store(true);
    c->store(true);
    EXPECT_EQ(tm.drain(), 0u);  // 0 detached = clean drain
    EXPECT_EQ(tm.active_count(), 0u);
}

TEST(ThreadManagerJoinNamedTest, AfterDrain_RefusesNewJoin)
{
    ThreadManager tm("test", "post_drain");
    auto stop = spawn_cooperating(tm, "alpha");
    stop->store(true, std::memory_order_release);
    EXPECT_EQ(tm.drain(), 0u);

    // After drain, the manager's `closing` flag is set.  join_named
    // must refuse — same contract as spawn() refusing post-drain.
    EXPECT_FALSE(tm.join_named("alpha"));
    EXPECT_FALSE(tm.join_named("anything"));
}
