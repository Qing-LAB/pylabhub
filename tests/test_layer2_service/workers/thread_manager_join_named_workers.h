#pragma once
/**
 * @file thread_manager_join_named_workers.h
 * @brief Workers for `ThreadManager::join_named` tests (Pattern 3).
 *
 * Every body constructs a `ThreadManager`, which registers a dynamic
 * lifecycle module against `LifecycleManager`.  Per
 * `docs/README/README_testing.md` § "Choosing a test pattern", that
 * forces these tests into a worker subprocess where `run_gtest_worker`
 * owns the `Logger` `LifecycleGuard`.
 */

namespace pylabhub::tests::worker
{
namespace thread_manager_join_named
{

int happy_path_signal_then_join();
int unknown_name_returns_false();
int idempotent_second_call();
int uncooperative_thread_detached();
int bracketed_thread_observes_internal_signal();
int cooperates_with_drain();
int after_drain_refuses_new_join();

} // namespace thread_manager_join_named
} // namespace pylabhub::tests::worker
