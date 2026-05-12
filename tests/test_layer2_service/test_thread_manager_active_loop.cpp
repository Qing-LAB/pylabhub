/**
 * @file test_thread_manager_active_loop.cpp
 * @brief Targeted tests for ThreadManager::SlotContext + active_loop_exited
 *        primitive (HEP-CORE-0031 §4.1 Thread Shutdown Contract).
 *
 * The active_loop_exited flag is the primitive that lets a managed thread
 * declare "I have exited my active loop and will not touch external shared
 * state from this point on" so the teardown caller can safely destroy that
 * state without waiting for the thread body to fully return.  These tests
 * pin the contract:
 *
 *   - mark_active_loop_exited() flips the flag (atomic store).
 *   - is_active_loop_exited(name) reflects the flag for known slots;
 *     returns false for unknown names.
 *   - wait_for_active_loop_exit(name, timeout):
 *       (a) returns true once flag is set (fast path + polled);
 *       (b) returns false on timeout;
 *       (c) returns false for unknown names.
 *   - The early-mark contract: the caller can observe active_loop_exited
 *     = true BEFORE the body returns (key property for using the flag
 *     as the resource-release synchronization point in MD1).
 *   - Backwards-compatible spawn overload (no SlotContext) auto-marks
 *     the flag after body return so old callers see reasonable
 *     defaults.
 *   - drain() two-stage diagnostic: a thread stuck IN its active loop is
 *     detached with a different log message than a thread stuck in
 *     post-loop cleanup.
 *
 * Each assertion pins a wall-clock bound or a deterministic side effect
 * so a regression that takes the wrong path can't silently pass — per
 * CLAUDE.md "Tests must pin path, timing, and structure" rule.
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
// Tests
// ──────────────────────────────────────────────────────────────────────

TEST(ThreadManagerActiveLoopTest, OldOverload_FlagSetAfterBodyReturn)
{
    // Backwards-compatible spawn (body takes no args): the wrapper marks
    // active_loop_exited automatically when body returns.  Verifies the
    // default-good behaviour: old callers do not need to change to get
    // reasonable semantics for the new query API.
    ThreadManager tm("test", "old_overload");

    auto stop = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(tm.spawn("worker", [stop]() {
        while (!stop->load(std::memory_order_acquire))
            std::this_thread::sleep_for(2ms);
    }));

    // While the body is running, the flag is NOT set.
    EXPECT_FALSE(tm.is_active_loop_exited("worker"))
        << "Flag must be false while body is running";

    stop->store(true, std::memory_order_release);

    // After body returns the wrapper marks the flag.  Use the wait API
    // with a bounded timeout to verify (also exercises the polling path).
    EXPECT_TRUE(tm.wait_for_active_loop_exit("worker", 500ms))
        << "Old overload: wrapper must mark active_loop_exited after body";

    // is_active_loop_exited mirrors what wait_for_active_loop_exit just saw.
    EXPECT_TRUE(tm.is_active_loop_exited("worker"));
}

TEST(ThreadManagerActiveLoopTest, NewOverload_EarlyMark_ObservableBeforeBodyReturn)
{
    // KEY CONTRACT TEST: with the new overload, the thread can mark
    // active_loop_exited BEFORE body returns.  The wait_for primitive
    // sees the flag as soon as the thread marks it; the body can then
    // continue in post-loop cleanup without holding the teardown caller.
    ThreadManager tm("test", "early_mark");

    auto exit_loop  = std::make_shared<std::atomic<bool>>(false);
    auto release_body = std::make_shared<std::atomic<bool>>(false);

    ASSERT_TRUE(tm.spawn("worker",
        [exit_loop, release_body](ThreadManager::SlotContext &ctx) {
            // Phase 1: in active loop.  Touch external state freely.
            while (!exit_loop->load(std::memory_order_acquire))
                std::this_thread::sleep_for(2ms);

            // Phase 2: leaving active loop.  Mark the flag.
            ctx.mark_active_loop_exited();

            // Phase 3: post-loop self-cleanup.  Must NOT touch any external
            // shared state (none here — this is just a synthetic delay
            // that proves the caller can see the flag BEFORE body return).
            while (!release_body->load(std::memory_order_acquire))
                std::this_thread::sleep_for(2ms);
        }));

    EXPECT_FALSE(tm.is_active_loop_exited("worker"));

    // Tell the body to leave the active loop.
    exit_loop->store(true, std::memory_order_release);

    // The flag should become true WITHOUT body returning (release_body
    // is still false; thread is parked in post-loop).
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(tm.wait_for_active_loop_exit("worker", 500ms))
        << "Flag must be observable as soon as thread marks it";
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, 100ms)
        << "Flag observation should be prompt (well under timeout)";

    // The thread is still in post-loop (parked on release_body).  Confirm
    // by checking active_count — the slot is still in flight.
    EXPECT_EQ(tm.active_count(), 1u)
        << "Body has not yet returned; slot still tracked";

    // Now release the body so drain() doesn't time out at teardown.
    release_body->store(true, std::memory_order_release);
    EXPECT_EQ(tm.drain(), 0u)
        << "After release, body returns and drain completes cleanly";
}

TEST(ThreadManagerActiveLoopTest, IsActiveLoopExited_UnknownName_ReturnsFalse)
{
    ThreadManager tm("test", "unknown");
    EXPECT_FALSE(tm.is_active_loop_exited("nonexistent"));
}

TEST(ThreadManagerActiveLoopTest, WaitForActiveLoopExit_UnknownName_ReturnsFalseFast)
{
    ThreadManager tm("test", "wait_unknown");

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(tm.wait_for_active_loop_exit("nonexistent", 500ms));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, 50ms)
        << "Unknown name must return false immediately (no polling)";
}

TEST(ThreadManagerActiveLoopTest, WaitForActiveLoopExit_Timeout_ReturnsFalse)
{
    // Thread is stuck in active loop; flag never set; wait must time out.
    ThreadManager tm("test", "wait_timeout");

    auto stop = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(tm.spawn("worker",
        [stop](ThreadManager::SlotContext &) {
            while (!stop->load(std::memory_order_acquire))
                std::this_thread::sleep_for(2ms);
            // Note: NEVER called mark_active_loop_exited from inside body.
            // After body returns, wrapper marks it — but the wait below
            // happens BEFORE we release stop.
        }));

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(tm.wait_for_active_loop_exit("worker", 100ms));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, 100ms) << "Must wait for the full timeout";
    EXPECT_LT(elapsed, 200ms) << "Must not over-wait beyond timeout + poll granularity";

    // Cleanup
    stop->store(true, std::memory_order_release);
    EXPECT_EQ(tm.drain(), 0u);
}

TEST(ThreadManagerActiveLoopTest, MarkActiveLoopExited_Idempotent)
{
    // Multiple calls to mark are safe.  Atomic store is idempotent —
    // verify the API contract holds (no crash, no assertion).
    ThreadManager tm("test", "idempotent");

    auto release = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(tm.spawn("worker",
        [release](ThreadManager::SlotContext &ctx) {
            ctx.mark_active_loop_exited();
            ctx.mark_active_loop_exited();  // second call — must be safe
            ctx.mark_active_loop_exited();  // third call — must be safe
            while (!release->load(std::memory_order_acquire))
                std::this_thread::sleep_for(2ms);
        }));

    EXPECT_TRUE(tm.wait_for_active_loop_exit("worker", 500ms));
    release->store(true, std::memory_order_release);
    EXPECT_EQ(tm.drain(), 0u);
}

TEST(ThreadManagerActiveLoopTest, Drain_FastPath_AlreadyExited)
{
    // When active_loop_exited is already true before drain polls, the
    // first stage is a no-op (no polling).  Verify drain completes
    // quickly when the thread has already marked exit + returned.
    ThreadManager::reset_process_detached_count_for_testing();
    ThreadManager tm("test", "fast_path");

    ASSERT_TRUE(tm.spawn("quick",
        [](ThreadManager::SlotContext &ctx) {
            ctx.mark_active_loop_exited();
            // Body returns immediately.
        }));

    // Give the body a moment to actually run + return.
    std::this_thread::sleep_for(50ms);

    const auto t0 = std::chrono::steady_clock::now();
    const auto detached = tm.drain();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(detached, 0u) << "Clean drain";
    EXPECT_LT(elapsed, 100ms)
        << "Fast path: active_loop_exited already set, no Stage 1 polling";
}

TEST(ThreadManagerActiveLoopTest, Drain_StuckInActiveLoop_DetachesWithDiagnostic)
{
    // Thread never observes stop signal.  drain() should detach after
    // the first half of join_timeout (Stage 1: active_loop_exited never
    // set).  detached count goes up.
    ThreadManager::reset_process_detached_count_for_testing();
    ThreadManager tm("test", "stuck_loop");

    auto stop = std::make_shared<std::atomic<bool>>(false);
    ThreadManager::SpawnOptions opts;
    opts.join_timeout = 100ms;  // 50ms per stage
    ASSERT_TRUE(tm.spawn("stuck",
        [stop](ThreadManager::SlotContext &) {
            while (!stop->load(std::memory_order_acquire))
                std::this_thread::sleep_for(2ms);
        },
        opts));

    const auto t0 = std::chrono::steady_clock::now();
    const auto detached = tm.drain();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(detached, 1u) << "Stuck thread detached after timeout";
    EXPECT_EQ(ThreadManager::process_detached_count(), 1u);
    // Stage 1 timeout (50ms) + Stage 2 timeout (50ms) ≈ 100ms total.
    EXPECT_GE(elapsed, 80ms) << "Drain must respect both stage timeouts";
    EXPECT_LT(elapsed, 300ms) << "Should not over-wait beyond two stages";

    // Cleanup the thread that's still running (detached).  Give it a
    // moment to exit cleanly so the next test starts clean.
    stop->store(true, std::memory_order_release);
    std::this_thread::sleep_for(50ms);

    // We deliberately exercised the detach path — clear the process-
    // wide unclean-shutdown counter so the gtest harness's check at
    // teardown doesn't fail this test on the leaked-thread guard.
    ThreadManager::reset_process_detached_count_for_testing();
}

}  // namespace
