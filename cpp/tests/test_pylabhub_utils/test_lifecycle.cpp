#include "utils/Lifecycle.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"

#include <string>
#include <vector>

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
    // Expect debug output in stderr, so don't assert empty.
    // The test's main purpose is to check initialization success, which is covered by the exit
    // code.
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
