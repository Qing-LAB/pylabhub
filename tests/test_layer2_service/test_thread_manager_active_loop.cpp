/**
 * @file test_thread_manager_active_loop.cpp
 * @brief Pattern 3 driver — `ThreadManager` shutdown-contract tests
 *        (HEP-CORE-0031 §4.1).
 *
 * Covers both API families:
 *   - Monotonic-mark (`mark_active_loop_exited` / `wait_for_active_loop_exit`).
 *   - Transactional (`with_active_loop` / `request_shutdown` /
 *     `request_shutdown_all` / `wait_for_quiescence`).
 *
 * Each scenario constructs `ThreadManager`, which registers a dynamic
 * lifecycle module on `Logger`; per `docs/README/README_testing.md` §
 * "Choosing a test pattern", the body must run in a worker subprocess.
 * Worker bodies live in `workers/thread_manager_active_loop_workers.cpp`
 * and register their dispatcher at static-init time.
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern explicitly ruled out by
 * `docs/README/README_testing.md` § "Antipatterns".
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class ThreadManagerActiveLoopTest : public IsolatedProcessTest
{
};

// ── Monotonic-mark family ──────────────────────────────────────────────────

TEST_F(ThreadManagerActiveLoopTest, OldOverload_FlagSetAfterBodyReturn)
{
    auto w = SpawnWorker("thread_manager_active_loop.old_overload_flag_set_after_body_return");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, NewOverload_EarlyMark_ObservableBeforeBodyReturn)
{
    auto w = SpawnWorker(
        "thread_manager_active_loop.new_overload_early_mark_observable_before_body_return");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, IsActiveLoopExited_UnknownName_ReturnsFalse)
{
    auto w =
        SpawnWorker("thread_manager_active_loop.is_active_loop_exited_unknown_name_returns_false");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, WaitForActiveLoopExit_UnknownName_ReturnsFalseFast)
{
    auto w = SpawnWorker(
        "thread_manager_active_loop.wait_for_active_loop_exit_unknown_name_returns_false_fast");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, WaitForActiveLoopExit_Timeout_ReturnsFalse)
{
    auto w =
        SpawnWorker("thread_manager_active_loop.wait_for_active_loop_exit_timeout_returns_false");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, MarkActiveLoopExited_Idempotent)
{
    auto w = SpawnWorker("thread_manager_active_loop.mark_active_loop_exited_idempotent");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, Drain_FastPath_AlreadyExited)
{
    auto w = SpawnWorker("thread_manager_active_loop.drain_fast_path_already_exited");
    ExpectWorkerOk(w);
}

// ── drain() two-stage diagnostics — deliberately exercise detach path ──────

TEST_F(ThreadManagerActiveLoopTest, Drain_StuckInActiveLoop_DetachesWithActiveLoopDiagnostic)
{
    auto w = SpawnWorker("thread_manager_active_loop.drain_stuck_in_active_loop");
    // F1 detach path: drain emits the "stuck inside its active loop"
    // ERROR + the "UNCLEAN SHUTDOWN" summary.  Pin both as expected so
    // the framework asserts they fire AND tolerates them.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_errors=*/{"stuck inside its active loop body", "UNCLEAN SHUTDOWN"});
}

TEST_F(ThreadManagerActiveLoopTest, Drain_StuckInPostLoop_DetachesWithPostLoopDiagnostic)
{
    auto w = SpawnWorker("thread_manager_active_loop.drain_stuck_in_post_loop");
    // F2 detach path: drain emits the "stuck in post-loop cleanup"
    // ERROR + the "UNCLEAN SHUTDOWN" summary.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_errors=*/{"stuck in post-loop cleanup", "UNCLEAN SHUTDOWN"});
}

// ── Transactional family ───────────────────────────────────────────────────

TEST_F(ThreadManagerActiveLoopTest, WithActiveLoop_BracketTogglesInActiveLoop)
{
    auto w =
        SpawnWorker("thread_manager_active_loop.with_active_loop_bracket_toggles_in_active_loop");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, WithActiveLoop_SkipsBodyIfShutdownRequestedBeforeEntry)
{
    auto w = SpawnWorker("thread_manager_active_loop.with_active_loop_skips_body_if_shutdown_"
                         "requested_before_entry");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, WithActiveLoop_RAIIResetOnException)
{
    auto w = SpawnWorker("thread_manager_active_loop.with_active_loop_raii_reset_on_exception");
    // Body throws inside the bracket; the spawn wrapper catches and
    // emits "thread '<name>' body threw: <msg>" at ERROR level.
    // Declare it as expected so the framework's no-ERROR guard
    // doesn't flag the deliberate exception path.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_errors=*/{"body threw"});
}

TEST_F(ThreadManagerActiveLoopTest, ShutdownRequested_ThreadSidePollObservesFlag)
{
    auto w =
        SpawnWorker("thread_manager_active_loop.shutdown_requested_thread_side_poll_observes_flag");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, RequestShutdown_UnknownNameReturnsFalse)
{
    auto w = SpawnWorker("thread_manager_active_loop.request_shutdown_unknown_name_returns_false");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, RequestShutdownAll_FlipsClosingAndRejectsNewSpawn)
{
    auto w = SpawnWorker(
        "thread_manager_active_loop.request_shutdown_all_flips_closing_and_rejects_new_spawn");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, WaitForQuiescence_DefaultSafeThreadsPassInstantly)
{
    auto w = SpawnWorker(
        "thread_manager_active_loop.wait_for_quiescence_default_safe_threads_pass_instantly");
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerActiveLoopTest, WaitForQuiescence_ExcludesCallingThread)
{
    auto w = SpawnWorker("thread_manager_active_loop.wait_for_quiescence_excludes_calling_thread");
    ExpectWorkerOk(w);
}
