#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <string>

using namespace pylabhub::utils;
using namespace pylabhub::tests::helper;
using namespace ::testing;

class LifecycleTest : public ::testing::Test
{
};

// Test that creating multiple LifecycleGuards results in only one owner
// and that a warning is printed for subsequent guards.
TEST_F(LifecycleTest, MultipleGuardsWarning)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_multiple_guards_warning", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
    // The warning is a PLH_DEBUG message, which goes to stderr.
    ASSERT_THAT(proc.get_stderr(),
                HasSubstr("WARNING: LifecycleGuard constructed but an owner already exists."));
}

// Test that modules are correctly registered and initialized.
TEST_F(LifecycleTest, ModuleRegistrationAndInitialization)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_module_registration_and_initialization",
                       {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0) << "Worker failed. Stderr:\n" << proc.get_stderr();
}

// Test that the is_initialized flag works as expected.
TEST_F(LifecycleTest, IsInitializedFlag)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_is_initialized_flag", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
    // Expect debug output in stderr, so don't assert empty.
    // The test's main purpose is to check the flag, which is covered by the exit code.
}

// Test that InitializeApp is idempotent (safe to call multiple times).
TEST_F(LifecycleTest, InitIdempotency)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_init_idempotency", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

// Test that FinalizeApp is idempotent (safe to call multiple times).
TEST_F(LifecycleTest, FinalizeIdempotency)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_finalize_idempotency", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

// Test that the is_finalized flag works as expected.
TEST_F(LifecycleTest, IsFinalizedFlag)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_is_finalized_flag", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

// Test that attempting to register a module after initialization aborts.
// This requires running in a separate process.
TEST_F(LifecycleTest, RegisterAfterInitAborts)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_register_after_init_aborts", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_NE(proc.wait_for_exit(), 0);
    ASSERT_THAT(proc.get_stderr(),
                HasSubstr("FATAL: register_module called after initialization."));
}

// Test that initialization fails if a module has an undefined dependency.
TEST_F(LifecycleTest, FailsWithUnresolvedDependency)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_unresolved_dependency", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_NE(proc.wait_for_exit(), 0);
    ASSERT_THAT(proc.get_stderr(), HasSubstr("[PLH_LifeCycle] FATAL: Undefined dependency:"));
}

// Test that initialization fails if a dependency name differs by case.
TEST_F(LifecycleTest, FailsWithCaseSensitiveDependency)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_case_insensitive_dependency", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_NE(proc.wait_for_exit(), 0);
    ASSERT_THAT(proc.get_stderr(), HasSubstr("[PLH_LifeCycle] FATAL: Undefined dependency:"));
}

// Test that initialization fails if a direct, two-module static dependency cycle is introduced.
TEST_F(LifecycleTest, StaticCircularDependencyAborts)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_static_circular_dependency_aborts", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_NE(proc.wait_for_exit(), 0);
    ASSERT_THAT(proc.get_stderr(),
                HasSubstr("[PLH_LifeCycle] FATAL: Circular dependency detected"));
}

// Test that initialization fails with a complex, indirect static dependency cycle.
TEST_F(LifecycleTest, StaticElaborateIndirectCycleAborts)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_static_elaborate_indirect_cycle_aborts",
                       {});
    ASSERT_TRUE(proc.valid());
    ASSERT_NE(proc.wait_for_exit(), 0);
    ASSERT_THAT(proc.get_stderr(),
                HasSubstr("[PLH_LifeCycle] FATAL: Circular dependency detected"));
}

// ============================================================================
// Module name string_view validation (MAX_MODULE_NAME_LEN = 256)
// ============================================================================

TEST_F(LifecycleTest, ModuleDef_RejectsEmptyName)
{
    // With string_view API, empty string replaces null as the "invalid name" case.
    EXPECT_THROW(ModuleDef(""), std::invalid_argument);
}

TEST_F(LifecycleTest, ModuleDef_RejectsNameExceedingMaxLength)
{
    std::string long_name(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'x');
    EXPECT_THROW(ModuleDef mod(long_name), std::length_error);
}

TEST_F(LifecycleTest, ModuleDef_AcceptsNameAtMaxLength)
{
    std::string max_name(ModuleDef::MAX_MODULE_NAME_LEN, 'a');
    EXPECT_NO_THROW({ ModuleDef mod(max_name); });
}

TEST_F(LifecycleTest, AddDependency_IgnoresEmpty)
{
    // With string_view API, empty string replaces null — silently ignored.
    ModuleDef mod("ValidModule");
    EXPECT_NO_THROW(mod.add_dependency(""));
}

TEST_F(LifecycleTest, AddDependency_RejectsNameExceedingMaxLength)
{
    ModuleDef mod("ValidModule");
    std::string long_dep(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'y');
    EXPECT_THROW(mod.add_dependency(long_dep.c_str()), std::length_error);
}

TEST_F(LifecycleTest, LoadModule_ReturnsFalseForNull)
{
    // LoadModule checks init-state before null; must run with lifecycle initialized.
    WorkerProcess proc(g_self_exe_path, "lifecycle.load_module_null_returns_false", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

TEST_F(LifecycleTest, LoadModule_ReturnsFalseForNameExceedingMaxLength)
{
    // LoadModule checks init-state before length; must run with lifecycle initialized.
    WorkerProcess proc(g_self_exe_path, "lifecycle.load_module_overflow_returns_false", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

TEST_F(LifecycleTest, UnloadModule_ReturnsFalseForNull)
{
    // UnloadModule checks init-state before null; must run with lifecycle initialized.
    WorkerProcess proc(g_self_exe_path, "lifecycle.unload_module_null_returns_false", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

TEST_F(LifecycleTest, UnloadModule_ReturnsFalseForNameExceedingMaxLength)
{
    // UnloadModule checks init-state before length; must run with lifecycle initialized.
    WorkerProcess proc(g_self_exe_path, "lifecycle.unload_module_overflow_returns_false", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

// ============================================================================
// Log sink injection tests
// ============================================================================

// Test that an installed log sink receives the warning emitted when unload_module
// is called on a module that is still referenced by another loaded module.
TEST_F(LifecycleTest, LogSink_RoutesWarningThroughSink)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.log_sink_routes_warning", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0) << "Worker failed. Stderr:\n" << proc.get_stderr();
    ASSERT_THAT(proc.get_stderr(), HasSubstr("LIFECYCLE_SINK:"));
    ASSERT_THAT(proc.get_stderr(), HasSubstr("Cannot unload module"));
}

// Test that clearing the log sink stops routing through it (messages fall back to PLH_DEBUG).
TEST_F(LifecycleTest, LogSink_ClearedStopsRouting)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.log_sink_cleared_uses_fallback", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0) << "Worker failed. Stderr:\n" << proc.get_stderr();
    // The sink must NOT have been called — its prefix should be absent.
    ASSERT_THAT(proc.get_stderr(), Not(HasSubstr("LIFECYCLE_SINK:")));
}

// ============================================================================
// Async unload + finalize interaction
// ============================================================================

// Test that finalize() waits for a pending async unload even when WaitForUnload
// was never called explicitly — the LifecycleGuard destructor must drain the
// shutdown thread and guarantee the stop callback ran before returning.
TEST_F(LifecycleTest, FinalizeWaitsForPendingAsyncUnload)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.finalize_waits_for_pending_async_unload", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0) << "Worker failed. Stderr:\n" << proc.get_stderr();
}

// Test that the log sink and logger both survive a timed-out async module shutdown
// that overlaps with the final shutdown sequence.  Specifically verifies that:
//  - lifecycleError() routes through LOGGER_ERROR while the logger is still running.
//  - do_logger_shutdown removes the sink before tearing down the logger queue.
//  - The process exits cleanly (no crash, no use-after-free).
TEST_F(LifecycleTest, FinalizeSinkSafeDuringAsyncShutdownFailure)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.finalize_sink_safe_during_async_failure", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_EQ(proc.wait_for_exit(), 0) << "Worker failed. Stderr:\n" << proc.get_stderr();
    // The timeout error must have been emitted (either via sink→logger or PLH_DEBUG).
    ASSERT_THAT(proc.get_stderr(), HasSubstr("TIMED OUT"));
}
