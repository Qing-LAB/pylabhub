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

class LifecycleTest : public ::testing::Test
{
};

// Test that creating multiple LifecycleGuards results in only one owner
// and that a warning is printed for subsequent guards.
TEST_F(LifecycleTest, MultipleGuardsWarning)
{
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "lifecycle.test_multiple_guards_warning", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

// Test that modules are correctly registered and initialized.
TEST_F(LifecycleTest, ModuleRegistrationAndInitialization)
{
    ProcessHandle proc = spawn_worker_process(
        g_self_exe_path, "lifecycle.test_module_registration_and_initialization", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

// Test that the is_initialized flag works as expected.
TEST_F(LifecycleTest, IsInitializedFlag)
{
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "lifecycle.test_is_initialized_flag", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(wait_for_worker_and_get_exit_code(proc), 0);
}

// Test that attempting to register a module after initialization aborts.
// This requires running in a separate process.
TEST_F(LifecycleTest, RegisterAfterInitAborts)
{
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "lifecycle.test_register_after_init_aborts", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    // The worker process should abort, resulting in a non-zero exit code.
    ASSERT_NE(wait_for_worker_and_get_exit_code(proc), 0);
} // Closing brace for RegisterAfterInitAborts

// Test that initialization fails if a module has an undefined dependency.
TEST_F(LifecycleTest, FailsWithUnresolvedDependency)
{
    ProcessHandle proc =
        spawn_worker_process(g_self_exe_path, "lifecycle.test_unresolved_dependency", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    // The worker process should abort, resulting in a non-zero exit code.
    ASSERT_NE(wait_for_worker_and_get_exit_code(proc), 0);
}
