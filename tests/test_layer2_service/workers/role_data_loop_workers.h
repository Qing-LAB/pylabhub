#pragma once
/**
 * @file role_data_loop_workers.h
 * @brief Workers for the run_data_loop and ThreadManager test suite.
 *
 * Each test's body constructs a RoleAPIBase, which transitively builds a
 * ThreadManager that registers a dynamic lifecycle module — so every body
 * runs in a subprocess where run_gtest_worker owns a Logger LifecycleGuard.
 */

namespace pylabhub::tests::worker
{
namespace role_data_loop
{

// ── run_data_loop scenarios ─────────────────────────────────────────────────
int shutdown_stops_loop();
int invoke_returns_false_stops_loop();
int metrics_increment();
int no_data_skips_deadline_wait();
int overrun_detected();

// ── ThreadManager scenarios ─────────────────────────────────────────────────
int thread_manager_spawn_and_join();
int thread_manager_multiple_threads();
int thread_manager_join_in_reverse_order();

} // namespace role_data_loop
} // namespace pylabhub::tests::worker
