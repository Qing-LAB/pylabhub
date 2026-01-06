#include "platform.hpp"         // For PLATFORM_WIN64, which is used by test_process_utils.h
#include "test_entrypoint.h"    // For g_self_exe_path
#include "test_process_utils.h" // For NULL_PROC_HANDLE and ProcessHandle
#include <gtest/gtest.h>

using namespace pylabhub::tests::helper; // Bring test helper namespace into scope

// This file contains the test fixtures that spawn worker processes
// for testing the dynamic lifecycle functionality. The actual test logic
// is implemented in lifecycle_workers.cpp.

class LifecycleDynamicTest : public ::testing::Test
{
};

// --- Success Cases ---

TEST_F(LifecycleDynamicTest, LoadAndUnload)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.dynamic.load_unload", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LifecycleDynamicTest, RefCounting)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.dynamic.ref_counting", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LifecycleDynamicTest, DependencyChain)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.dynamic.dependency_chain", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LifecycleDynamicTest, DiamondDependency)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.dynamic.diamond_dependency", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LifecycleDynamicTest, FinalizeUnloadsAll)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.dynamic.finalize_unloads_all", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

// --- Failure Cases ---

TEST_F(LifecycleDynamicTest, RegisterBeforeInitFails)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.dynamic.register_before_init_fail", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    // Worker returns 0 if the registration correctly fails as expected.
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LifecycleDynamicTest, LoadFailsWithUnmetStaticDependency)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.dynamic.static_dependency_fail", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    // Worker returns 0 if registration correctly fails.
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LifecycleDynamicTest, RegistrationFailsWithUnresolvedDependency)
{
    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(
        g_self_exe_path, "lifecycle.registration_fails_with_unresolved_dependency", {});
    ASSERT_NE(proc, NULL_PROC_HANDLE);
    // Worker returns 0 if registration correctly fails as expected.
    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}

TEST_F(LifecycleDynamicTest, ReentrantLoadFails)

{

    pylabhub::tests::helper::ProcessHandle proc = pylabhub::tests::helper::spawn_worker_process(

        g_self_exe_path, "lifecycle.dynamic.reentrant_load_fail", {});

    ASSERT_NE(proc, NULL_PROC_HANDLE);

    // Worker returns 0 if LoadModule correctly fails.

    ASSERT_EQ(pylabhub::tests::helper::wait_for_worker_and_get_exit_code(proc), 0);
}
