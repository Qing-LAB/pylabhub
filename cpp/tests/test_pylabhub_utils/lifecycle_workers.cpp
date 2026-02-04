#include "lifecycle_workers.h"
#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "gtest/gtest.h"

using namespace pylabhub::utils;

namespace
{
// --- Globals for static module tests ---
std::atomic<int> g_startup_counter{0};
void counter_startup_callback(const char *)
{
    g_startup_counter++;
}
void reset_static_counters()
{
    g_startup_counter = 0;
}

// --- Globals for dynamic module tests ---
std::atomic<int> dyn_A_start{0}, dyn_B_start{0}, dyn_C_start{0}, dyn_D_start{0}, dyn_E_start{0};
std::atomic<int> dyn_A_stop{0}, dyn_B_stop{0}, dyn_C_stop{0}, dyn_D_stop{0}, dyn_E_stop{0};

void reset_dynamic_counters()
{
    dyn_A_start = 0;
    dyn_B_start = 0;
    dyn_C_start = 0;
    dyn_D_start = 0;
    dyn_E_start = 0;
    dyn_A_stop = 0;
    dyn_B_stop = 0;
    dyn_C_stop = 0;
    dyn_D_stop = 0;
    dyn_E_stop = 0;
}

void startup_A(const char *)
{
    dyn_A_start++;
}
void startup_B(const char *)
{
    dyn_B_start++;
}
void startup_C(const char *)
{
    dyn_C_start++;
}
void startup_D(const char *)
{
    dyn_D_start++;
}
void startup_E(const char *)
{
    dyn_E_start++;
}
void shutdown_A(const char *)
{
    dyn_A_stop++;
}
void shutdown_B(const char *)
{
    dyn_B_stop++;
}
void shutdown_C(const char *)
{
    dyn_C_stop++;
}
void shutdown_D(const char *)
{
    dyn_D_stop++;
}
void shutdown_E(const char *)
{
    dyn_E_stop++;
}

} // namespace

// ============================================================================
// Static Lifecycle Workers
// ============================================================================

int pylabhub::tests::worker::lifecycle::test_multiple_guards_warning()
{
    LifecycleGuard guard1;
    LifecycleGuard guard2;
    return 0;
}

int pylabhub::tests::worker::lifecycle::test_module_registration_and_initialization()
{
    reset_static_counters();
    ModuleDef module_a("ModuleA");
    module_a.set_startup(counter_startup_callback);
    LifecycleGuard guard(std::move(module_a));
    return (g_startup_counter.load() != 1);
}

int pylabhub::tests::worker::lifecycle::test_register_after_init_aborts()
{
    LifecycleGuard guard;
    ModuleDef module_a("LateStaticModule");
    RegisterModule(std::move(module_a)); // Should abort
    return 1;                            // Should not be reached
}

int pylabhub::tests::worker::lifecycle::test_unresolved_dependency()
{
    ModuleDef module_a("ModuleA");
    module_a.add_dependency("NonExistentModule");
    LifecycleGuard guard(std::move(module_a)); // Should abort
    return 1;                                  // Should not be reached
}

int pylabhub::tests::worker::lifecycle::test_is_initialized_flag()
{
    // App should not be initialized before a guard is created.
    if (IsAppInitialized())
    {
        return 1;
    }

    LifecycleGuard guard;

    // After the first guard is created, the app should be initialized.
    if (!IsAppInitialized())
    {
        return 2;
    }

    return 0;
}

int pylabhub::tests::worker::lifecycle::test_case_insensitive_dependency()
{
    // Module dependency resolution is case-sensitive. This test verifies that
    // initialization will fail if a dependency is declared with a name that
    // differs only by case.
    ModuleDef module_a("ModuleA");
    ModuleDef module_b("ModuleB");
    module_b.add_dependency("modulea"); // Dependency with wrong case

    // This should cause a panic/abort because "modulea" is not found.
    LifecycleGuard guard(MakeModDefList(std::move(module_a), std::move(module_b)));

    return 1; // Should not be reached.
}

int pylabhub::tests::worker::lifecycle::test_static_circular_dependency_aborts()
{
    ModuleDef module_a("CycleA");
    module_a.add_dependency("CycleB");

    ModuleDef module_b("CycleB");
    module_b.add_dependency("CycleA");

    // Registering both modules should succeed. The cycle is formed, but
    // is only detected during the topological sort in initialize().
    RegisterModule(std::move(module_a));
    RegisterModule(std::move(module_b));

    // The LifecycleGuard's constructor calls initialize(), which should detect
    // the cycle and abort the program.
    LifecycleGuard guard;

    return 1; // Should not be reached.
}

int pylabhub::tests::worker::lifecycle::test_static_elaborate_indirect_cycle_aborts()
{
    // Cluster 1
    ModuleDef c1_root("C1_Root");
    ModuleDef c1_a1("C1_A1");
    c1_a1.add_dependency("C1_Root");
    ModuleDef c1_a2("C1_A2");
    c1_a2.add_dependency("C1_Root");
    ModuleDef c1_b1("C1_B1");
    c1_b1.add_dependency("C1_A1");
    ModuleDef c1_b2("C1_B2");
    c1_b2.add_dependency("C1_A1");
    c1_b2.add_dependency("C1_A2");
    ModuleDef c1_c1("C1_C1");
    c1_c1.add_dependency("C1_B1");

    // Cluster 2
    ModuleDef c2_root("C2_Root");
    ModuleDef c2_a1("C2_A1");
    c2_a1.add_dependency("C2_Root");
    ModuleDef c2_a2("C2_A2");
    c2_a2.add_dependency("C2_Root");
    ModuleDef c2_b1("C2_B1");
    c2_b1.add_dependency("C2_A1");
    ModuleDef c2_b2("C2_B2");
    c2_b2.add_dependency("C2_A1");
    c2_b2.add_dependency("C2_A2");
    ModuleDef c2_c1("C2_C1");
    c2_c1.add_dependency("C2_B1");
    c2_c1.add_dependency("C2_B2");
    ModuleDef c2_d1("C2_D1");
    c2_d1.add_dependency("C2_C1");

    c2_c1.add_dependency("C1_B2"); // Link 1 (C1 -> C2)
    c1_a1.add_dependency("C2_D1"); // Link 2 (C2 -> C1, forms the cycle)
    c1_c1.add_dependency("C2_A2"); // Link 3 (Non-cycling cross-link)

    RegisterModule(std::move(c1_root));
    RegisterModule(std::move(c1_a1));
    RegisterModule(std::move(c1_a2));
    RegisterModule(std::move(c1_b1));
    RegisterModule(std::move(c1_b2));
    RegisterModule(std::move(c1_c1));
    RegisterModule(std::move(c2_root));
    RegisterModule(std::move(c2_a1));
    RegisterModule(std::move(c2_a2));
    RegisterModule(std::move(c2_b1));
    RegisterModule(std::move(c2_b2));
    RegisterModule(std::move(c2_c1));
    RegisterModule(std::move(c2_d1));

    // This guard will call initialize(), which will run the topological sort
    // and detect the cycle: C1_A1 -> C1_B2 -> C2_C1 -> C2_D1 -> C1_A1
    LifecycleGuard guard;

    return 1; // Should not be reached
}

// ============================================================================
// Dynamic Lifecycle Workers
// ============================================================================

int pylabhub::tests::worker::lifecycle::dynamic_register_before_init_fail()
{
    ModuleDef mod("DynA");
    // Should fail because static core is not initialized.
    if (RegisterDynamicModule(std::move(mod)))
        return 1;
    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_load_unload()
{
    reset_dynamic_counters();
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef mod("DynA");
    mod.set_startup(startup_A);
    mod.set_shutdown(shutdown_A, 100);
    if (!RegisterDynamicModule(std::move(mod)))
        return 1;

    if (!LoadModule("DynA"))
        return 2;
    if (dyn_A_start != 1)
        return 3;

    if (!UnloadModule("DynA"))
        return 4;
    if (dyn_A_stop != 1)
        return 5;

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_ref_counting()
{
    reset_dynamic_counters();
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef mod("DynA");
    mod.set_startup(startup_A);
    mod.set_shutdown(shutdown_A, 100);
    if (!RegisterDynamicModule(std::move(mod)))
        return 1;

    // First load
    if (!LoadModule("DynA"))
        return 2;
    if (dyn_A_start != 1)
        return 3;

    // Second load should be a no-op
    if (!LoadModule("DynA"))
        return 4;
    if (dyn_A_start != 1) // Should not start again
        return 5;

    // A single unload should now work
    if (!UnloadModule("DynA"))
        return 6;
    if (dyn_A_stop != 1)
        return 7;

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_dependency_chain()
{
    reset_dynamic_counters();
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef modB("DynB");
    modB.set_startup(startup_B);
    modB.set_shutdown(shutdown_B, 100);
    if (!RegisterDynamicModule(std::move(modB)))
        return 1;

    ModuleDef modA("DynA");
    modA.add_dependency("DynB");
    modA.set_startup(startup_A);
    modA.set_shutdown(shutdown_A, 100);
    if (!RegisterDynamicModule(std::move(modA)))
        return 2;

    if (!LoadModule("DynA"))
        return 3;
    if (dyn_A_start != 1 || dyn_B_start != 1)
        return 4;

    // Unloading DynA should trigger a recursive unload of DynB.
    if (!UnloadModule("DynA"))
        return 5;
    if (dyn_A_stop != 1 || dyn_B_stop != 1)
        return 6;

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_diamond_dependency()
{
    reset_dynamic_counters();
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef modD("DynD");
    modD.set_startup(startup_D);
    modD.set_shutdown(shutdown_D, 100);
    if (!RegisterDynamicModule(std::move(modD)))
        return 1;

    ModuleDef modB("DynB");
    modB.add_dependency("DynD");
    modB.set_startup(startup_B);
    modB.set_shutdown(shutdown_B, 100);
    if (!RegisterDynamicModule(std::move(modB)))
        return 2;

    ModuleDef modC("DynC");
    modC.add_dependency("DynD");
    modC.set_startup(startup_C);
    modC.set_shutdown(shutdown_C, 100);
    if (!RegisterDynamicModule(std::move(modC)))
        return 3;

    ModuleDef modA("DynA");
    modA.add_dependency("DynB");
    modA.add_dependency("DynC");
    modA.set_startup(startup_A);
    modA.set_shutdown(shutdown_A, 100);
    if (!RegisterDynamicModule(std::move(modA)))
        return 4;

    // --- SCENARIO 1: Unload from the top ---
    if (!LoadModule("DynA"))
        return 5;
    if (dyn_A_start != 1 || dyn_B_start != 1 || dyn_C_start != 1 || dyn_D_start != 1)
        return 6;

    // Unloading A should trigger a full cascade unload.
    if (!UnloadModule("DynA"))
        return 7;
    if (dyn_A_stop != 1 || dyn_B_stop != 1 || dyn_C_stop != 1 || dyn_D_stop != 1)
        return 8;

    // --- SCENARIO 2: Unload side branches ---
    reset_dynamic_counters();
    // Re-register modules since they were removed from the graph.
    ModuleDef modD2("DynD");
    modD2.set_startup(startup_D);
    modD2.set_shutdown(shutdown_D, 100);
    if (!RegisterDynamicModule(std::move(modD2)))
        return 101;
    ModuleDef modB2("DynB");
    modB2.add_dependency("DynD");
    modB2.set_startup(startup_B);
    modB2.set_shutdown(shutdown_B, 100);
    if (!RegisterDynamicModule(std::move(modB2)))
        return 102;
    ModuleDef modC2("DynC");
    modC2.add_dependency("DynD");
    modC2.set_startup(startup_C);
    modC2.set_shutdown(shutdown_C, 100);
    if (!RegisterDynamicModule(std::move(modC2)))
        return 103;

    // Load B and C independently
    if (!LoadModule("DynB"))
        return 9;
    if (!LoadModule("DynC"))
        return 10;
    if (dyn_B_start != 1 || dyn_C_start != 1 || dyn_D_start != 1)
        return 11;
    if (dyn_A_start != 0)
        return 12;

    // Try to unload D. Should fail (ref_count is 2). `UnloadModule` returns false.
    if (UnloadModule("DynD"))
        return 13;
    if (dyn_D_stop != 0)
        return 14;

    // Unload B. D should NOT be unloaded (ref_count will drop to 1).
    if (!UnloadModule("DynB"))
        return 15;
    if (dyn_B_stop != 1)
        return 16;
    if (dyn_D_stop != 0)
        return 17;

    // Try to unload D again. Should still fail (ref_count is 1).
    if (UnloadModule("DynD"))
        return 18;
    if (dyn_D_stop != 0)
        return 19;

    // Unload C. This should now trigger D to be unloaded.
    if (!UnloadModule("DynC"))
        return 20;
    if (dyn_C_stop != 1)
        return 21;
    if (dyn_D_stop != 1)
        return 22;

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_finalize_unloads_all()
{
    reset_dynamic_counters();
    {
        LifecycleGuard guard(Logger::GetLifecycleModule());
        ModuleDef mod("DynA");
        mod.set_startup(startup_A);
        mod.set_shutdown(shutdown_A, 100);
        if (!RegisterDynamicModule(std::move(mod)))
            return 1;

        if (!LoadModule("DynA"))
            return 2;
    }
    if (dyn_A_stop != 1)
        return 3;
    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_persistent_in_middle()
{
    PLH_DEBUG("-> WORKER dynamic_persistent_in_middle STARTED");
    reset_dynamic_counters();
    LifecycleGuard guard(Logger::GetLifecycleModule());

    PLH_DEBUG("WORKER: Registering modules...");
    // Register E (bottom of one branch)
    ModuleDef modE("DynE");
    modE.set_startup(startup_E);
    modE.set_shutdown(shutdown_E, 100);
    if (!RegisterDynamicModule(std::move(modE)))
        return 1;

    // Register D (persistent, bottom of other branch)
    ModuleDef modD("DynD");
    modD.set_startup(startup_D);
    modD.set_shutdown(shutdown_D, 100);
    modD.set_as_persistent();
    if (!RegisterDynamicModule(std::move(modD)))
        return 2;

    // Register C
    ModuleDef modC("DynC");
    modC.add_dependency("DynE");
    modC.set_startup(startup_C);
    modC.set_shutdown(shutdown_C, 100);
    if (!RegisterDynamicModule(std::move(modC)))
        return 3;

    // Register B
    ModuleDef modB("DynB");
    modB.add_dependency("DynD");
    modB.set_startup(startup_B);
    modB.set_shutdown(shutdown_B, 100);
    if (!RegisterDynamicModule(std::move(modB)))
        return 4;

    // Register A (top)
    ModuleDef modA("DynA");
    modA.add_dependency("DynB");
    modA.add_dependency("DynC");
    modA.set_startup(startup_A);
    modA.set_shutdown(shutdown_A, 100);
    if (!RegisterDynamicModule(std::move(modA)))
        return 5;
    PLH_DEBUG("WORKER: Module registration complete.");

    // Load the whole graph from the top
    PLH_DEBUG("WORKER: Calling LoadModule('DynA')...");
    if (!LoadModule("DynA"))
    {
        PLH_DEBUG("WORKER: LoadModule('DynA') failed unexpectedly.");
        return 10;
    }
    PLH_DEBUG("WORKER: LoadModule('DynA') finished. Checking startup counters...");
    if (dyn_A_start != 1 || dyn_B_start != 1 || dyn_C_start != 1 || dyn_D_start != 1 ||
        dyn_E_start != 1)
    {
        PLH_DEBUG("WORKER: Startup counters are incorrect.");
        return 11;
    }
    PLH_DEBUG("WORKER: Startup counters are correct.");

    // Unload from the top
    PLH_DEBUG("WORKER: Calling UnloadModule('DynA')...");
    if (!UnloadModule("DynA"))
    {
        PLH_DEBUG("WORKER: UnloadModule('DynA') failed unexpectedly.");
        return 12;
    }
    PLH_DEBUG("WORKER: UnloadModule('DynA') finished. Checking shutdown counters...");

    // A, B, C, E should be stopped. D (persistent) should NOT be stopped.
    if (dyn_A_stop != 1)
        return 13;
    if (dyn_B_stop != 1)
        return 14;
    if (dyn_C_stop != 1)
        return 15;
    if (dyn_E_stop != 1)
        return 16;
    if (dyn_D_stop != 0)
    {
        PLH_DEBUG("WORKER: persistent module 'DynD' was incorrectly stopped.");
        return 17;
    }
    PLH_DEBUG("WORKER: Shutdown counters are correct. Test passed.");

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_static_dependency_fail()
{
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef dynMod("DynA");
    dynMod.add_dependency("NonExistentStaticMod");
    if (RegisterDynamicModule(std::move(dynMod)))
        return 1;

    return 0;
}

int pylabhub::tests::worker::lifecycle::registration_fails_with_unresolved_dependency()
{
    LifecycleGuard guard(Logger::GetLifecycleModule());

    // This test verifies that the system prevents dependency cycles at registration
    // time by failing to register a module with an unresolved dependency.
    // We attempt to register DynA which depends on DynB (which doesn't exist yet).
    // This registration must fail.
    ModuleDef modA("DynA");
    modA.add_dependency("DynB");
    if (!RegisterDynamicModule(std::move(modA)))
    {
        // This is the expected outcome. The registration failed because the
        // dependency was not resolved.
        return 0; // Success for the test
    }

    // If we reach here, the registration unexpectedly succeeded, which is a failure.
    return 1;
}

int pylabhub::tests::worker::lifecycle::dynamic_reentrant_load_fail()
{
    struct ReentrantCallbacks
    {
        static void startup(const char *)
        {
            // LoadModule("DynB") will detect re-entrancy and return false.
            // We must throw an exception to signal the failure to the parent
            // `loadModule` call.
            if (LoadModule("DynB"))
            {
                throw std::runtime_error("Re-entrant LoadModule('DynB') unexpectedly succeeded!");
            }
            throw std::runtime_error(
                "LoadModule('DynB') detected re-entrant call and failed as expected.");
        }
    };

    LifecycleGuard guard(Logger::GetLifecycleModule());
    ModuleDef modB("DynB");
    if (!RegisterDynamicModule(std::move(modB)))
        return 1;

    ModuleDef modA("DynA");
    modA.set_startup(ReentrantCallbacks::startup);
    if (!RegisterDynamicModule(std::move(modA)))
        return 2;

    // LoadModule("DynA") should fail because its startup callback throws an exception.
    LoadModule("DynA");
    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_persistent_module()
{
    reset_dynamic_counters();
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef mod_perm("DynPerm");
    mod_perm.set_startup(startup_D); // Using D's counters
    mod_perm.set_shutdown(shutdown_D, 100);
    mod_perm.set_as_persistent();
    if (!RegisterDynamicModule(std::move(mod_perm)))
        return 1;

    ModuleDef mod_a("DynA");
    mod_a.add_dependency("DynPerm");
    mod_a.set_startup(startup_A);
    mod_a.set_shutdown(shutdown_A, 100);
    if (!RegisterDynamicModule(std::move(mod_a)))
        return 2;

    if (!LoadModule("DynA"))
        return 3;
    if (dyn_A_start != 1 || dyn_D_start != 1)
        return 4;

    if (!UnloadModule("DynA"))
        return 5;
    if (dyn_A_stop != 1)
        return 6;
    if (dyn_D_stop != 0)
        return 7; // DynPerm should NOT be stopped

    // finalize() should shut it down
    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_persistent_module_finalize()
{
    reset_dynamic_counters();
    {
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef mod_perm("DynPerm");
        mod_perm.set_startup(startup_D);
        mod_perm.set_shutdown(shutdown_D, 100);
        mod_perm.set_as_persistent();
        if (!RegisterDynamicModule(std::move(mod_perm)))
            return 1;

        if (!LoadModule("DynPerm"))
            return 2;
        if (dyn_D_start != 1)
            return 3;

        // Don't unload, let the LifecycleGuard handle it.
    }
    // At this point, finalize() has been called.

    if (dyn_D_stop != 1)
        return 4; // DynPerm should BE stopped by finalize()

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_unload_timeout()
{
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef mod("HangingModule");
    mod.set_shutdown(
        [](const char *)
        {
            PLH_DEBUG("HangingModule shutdown started, sleeping for 250ms...");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            PLH_DEBUG("HangingModule shutdown finished sleep.");
        },
        50); // 50ms timeout

    if (!RegisterDynamicModule(std::move(mod)))
        return 1;

    if (!LoadModule("HangingModule"))
        return 2;

    // This should return in ~50ms, not hang for 250ms.
    // The test runner will verify the "TIMEOUT!" message in stderr.
    if (!UnloadModule("HangingModule"))
        return 3;

    return 0;
}
