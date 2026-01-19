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
int dynamic_permanent_module();
int dynamic_permanent_module_finalize();
int dynamic_permanent_in_middle();

} // namespace pylabhub::tests::worker::lifecycle
