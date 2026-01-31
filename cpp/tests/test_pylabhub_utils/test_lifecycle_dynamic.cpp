#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

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
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.load_unload", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

TEST_F(LifecycleDynamicTest, RefCounting)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.ref_counting", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

TEST_F(LifecycleDynamicTest, DependencyChain)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.dependency_chain", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

TEST_F(LifecycleDynamicTest, DiamondDependency)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.diamond_dependency", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

TEST_F(LifecycleDynamicTest, FinalizeUnloadsAll)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.finalize_unloads_all", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

TEST_F(LifecycleDynamicTest, PersistentModuleInDependencyChain)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.persistent_in_middle", {}, true);
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

// --- Failure Cases ---

TEST_F(LifecycleDynamicTest, RegisterBeforeInitFails)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.register_before_init_fail", {});
    ASSERT_TRUE(proc.valid());
    // Worker returns 0 if the registration correctly fails as expected.
    proc.wait_for_exit();
    expect_worker_ok(proc, {"ERROR: register_dynamic_module called before initialization."});
}

TEST_F(LifecycleDynamicTest, LoadFailsWithUnmetStaticDependency)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.static_dependency_fail", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc,
                     {"ERROR: Dependency 'NonExistentStaticMod' for module 'DynA' not found."});
}

TEST_F(LifecycleDynamicTest, RegistrationFailsWithUnresolvedDependency)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.registration_fails_with_unresolved_dependency",
                       {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc, {"ERROR: Dependency 'DynB' for module 'DynA' not found."});
}

TEST_F(LifecycleDynamicTest, ReentrantLoadFails)

{

    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.reentrant_load_fail", {});
    ASSERT_TRUE(proc.valid());

    proc.wait_for_exit();
    expect_worker_ok(proc,
                     {"Re-entrant call to load_module('DynB') detected",
                      "module 'DynA' threw on startup", "re-entrant call and failed as expected"});
}

TEST_F(LifecycleDynamicTest, PersistentModuleIsNotUnloaded)
{
    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.persistent_module", {});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}

TEST_F(LifecycleDynamicTest, PersistentModuleIsUnloadedOnFinalize)

{

    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.persistent_module_finalize", {});

    ASSERT_TRUE(proc.valid());

    proc.wait_for_exit();

    expect_worker_ok(proc);

}



TEST_F(LifecycleDynamicTest, UnloadTimeout)

{

    WorkerProcess proc(g_self_exe_path, "lifecycle.dynamic.unload_timeout", {}, true);

    ASSERT_TRUE(proc.valid());

    proc.wait_for_exit();

    expect_worker_ok(proc, {"TIMEOUT!"});

}
