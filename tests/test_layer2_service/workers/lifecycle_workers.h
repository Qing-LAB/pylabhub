#pragma once

namespace pylabhub::tests::worker::lifecycle
{

int test_multiple_guards_warning();
int test_module_registration_and_initialization();
int test_is_initialized_flag();
int test_register_after_init_aborts();
int test_unresolved_dependency();
int test_case_insensitive_dependency();
int test_static_circular_dependency_aborts();
int test_static_elaborate_indirect_cycle_aborts();

// --- Null/overflow input validation (requires lifecycle to be initialized) ---
int load_module_null_returns_false();
int load_module_overflow_returns_false();
int unload_module_null_returns_false();
int unload_module_overflow_returns_false();

// --- Init/finalize idempotency and is_finalized ---
int test_init_idempotency();
int test_finalize_idempotency();
int test_is_finalized_flag();

// --- Dynamic Lifecycle Workers ---
int dynamic_register_before_init_fail();
int dynamic_load_unload();
int dynamic_ref_counting();
int dynamic_dependency_chain();
int dynamic_diamond_dependency();
int dynamic_finalize_unloads_all();
int dynamic_static_dependency_fail();
int registration_fails_with_unresolved_dependency();
int dynamic_reentrant_load_fail();
int dynamic_persistent_module();
int dynamic_persistent_module_finalize();
int dynamic_persistent_in_middle();
int dynamic_unload_timeout();

// --- Owner-managed teardown (HEP-CORE-0001 §"Owner-managed teardown") ---
// Validator-fail at unload time + `set_owner_managed_teardown(true)` →
// success-without-callback (graph cleaned, no contamination, no WARN).
int dynamic_owner_managed_teardown_clean_unload();
// Validator-fail at unload time WITHOUT the opt-in flag → HEP-0001
// default anomaly (FAILED_SHUTDOWN, contaminated, retained in graph,
// re-registration with same name fails).
int dynamic_validator_fail_default_anomaly();

// --- Synchronous shutdown (HEP-CORE-0011 §"Engine Construction
//     Lifecycle"; ModuleDef::set_synchronous_shutdown opt-in) ---
//
// Persistent dynamic module with sync flag set: at finalize() Phase 2
// the framework MUST call shutdown.func DIRECTLY on the
// ~LifecycleGuard thread (no `timedShutdown` worker spawn).  Verified
// by recording `std::this_thread::get_id()` inside the callback and
// comparing to the test thread's id.
int dynamic_sync_shutdown_runs_on_caller_thread();
// Persistent dynamic module WITHOUT the sync flag (default): at
// finalize() Phase 2 the framework MUST use `timedShutdown` (spawns
// a worker thread per module).  Verified by recording the callback's
// thread id and asserting it differs from the test thread.  Locks
// the regression-protection contract: sync is opt-in, default
// behaviour preserved for every module that does not opt in.
int dynamic_default_shutdown_runs_on_spawned_thread();
// Sync-shutdown callback that throws — must be caught by the
// inline-shutdown try/catch in `finalize()` (`run_inline` lambda) and
// recorded as `FailedShutdown` rather than propagating out of the
// LifecycleGuard dtor.
int dynamic_sync_shutdown_callback_throws_is_failed_shutdown();

// --- Log sink injection tests ---
int log_sink_routes_warning();
int log_sink_cleared_uses_fallback();

// --- Async unload + finalize interaction ---
int finalize_waits_for_pending_async_unload();
int finalize_sink_safe_during_async_failure();

} // namespace pylabhub::tests::worker::lifecycle
