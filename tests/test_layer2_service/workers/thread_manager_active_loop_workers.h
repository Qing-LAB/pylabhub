#pragma once
/**
 * @file thread_manager_active_loop_workers.h
 * @brief Workers for `ThreadManager` shutdown-contract tests (Pattern 3).
 *
 * Covers both contract families:
 *   - Monotonic-mark API (`SlotContext::mark_active_loop_exited` /
 *     `wait_for_active_loop_exit`).
 *   - Transactional API (`SlotContext::with_active_loop` /
 *     `request_shutdown` / `request_shutdown_all` /
 *     `wait_for_quiescence`).
 *
 * Per `docs/README/README_testing.md` § "Choosing a test pattern", every
 * body that constructs `ThreadManager` must run in a worker subprocess.
 */

namespace pylabhub::tests::worker
{
namespace thread_manager_active_loop
{

// ── Monotonic-mark family ──────────────────────────────────────────────────
int old_overload_flag_set_after_body_return();
int new_overload_early_mark_observable_before_body_return();
int is_active_loop_exited_unknown_name_returns_false();
int wait_for_active_loop_exit_unknown_name_returns_false_fast();
int wait_for_active_loop_exit_timeout_returns_false();
int mark_active_loop_exited_idempotent();
int drain_fast_path_already_exited();

// ── drain() two-stage diagnostics (detach paths — emit ERROR logs) ─────────
int drain_stuck_in_active_loop_detaches_with_active_loop_diagnostic();
int drain_stuck_in_post_loop_detaches_with_post_loop_diagnostic();

// ── Transactional family ───────────────────────────────────────────────────
int with_active_loop_bracket_toggles_in_active_loop();
int with_active_loop_skips_body_if_shutdown_requested_before_entry();
int with_active_loop_raii_reset_on_exception();
int shutdown_requested_thread_side_poll_observes_flag();
int request_shutdown_unknown_name_returns_false();
int request_shutdown_all_flips_closing_and_rejects_new_spawn();
int wait_for_quiescence_default_safe_threads_pass_instantly();
int wait_for_quiescence_excludes_calling_thread();

} // namespace thread_manager_active_loop
} // namespace pylabhub::tests::worker
