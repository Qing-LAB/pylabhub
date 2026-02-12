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

} // namespace pylabhub::tests::worker::lifecycle
