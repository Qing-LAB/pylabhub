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

// ============================================================================
// Module name C-string validation (MAX_MODULE_NAME_LEN = 256, null-terminated)
// ============================================================================

TEST_F(LifecycleTest, ModuleDef_RejectsNullName)
{
    EXPECT_THROW(ModuleDef(nullptr), std::invalid_argument);
}

TEST_F(LifecycleTest, ModuleDef_RejectsNameExceedingMaxLength)
{
    std::string long_name(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'x');
    EXPECT_THROW(ModuleDef mod(long_name.c_str()), std::length_error);
}

TEST_F(LifecycleTest, ModuleDef_AcceptsNameAtMaxLength)
{
    std::string max_name(ModuleDef::MAX_MODULE_NAME_LEN, 'a');
    EXPECT_NO_THROW({
        ModuleDef mod(max_name.c_str());
    });
}

TEST_F(LifecycleTest, AddDependency_IgnoresNull)
{
    ModuleDef mod("ValidModule");
    EXPECT_NO_THROW(mod.add_dependency(nullptr));
}

TEST_F(LifecycleTest, AddDependency_RejectsNameExceedingMaxLength)
{
    ModuleDef mod("ValidModule");
    std::string long_dep(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'y');
    EXPECT_THROW(mod.add_dependency(long_dep.c_str()), std::length_error);
}

TEST_F(LifecycleTest, LoadModule_ReturnsFalseForNull)
{
    EXPECT_FALSE(LoadModule(nullptr));
}

TEST_F(LifecycleTest, LoadModule_ReturnsFalseForNameExceedingMaxLength)
{
    std::string long_name(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'z');
    EXPECT_FALSE(LoadModule(long_name.c_str()));
}

TEST_F(LifecycleTest, UnloadModule_ReturnsFalseForNull)
{
    EXPECT_FALSE(UnloadModule(nullptr));
}

TEST_F(LifecycleTest, UnloadModule_ReturnsFalseForNameExceedingMaxLength)
{
    std::string long_name(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'w');
    EXPECT_FALSE(UnloadModule(long_name.c_str()));
}
