#include "lifecycle_workers.h"
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp" // For LOGGER_INFO etc.

#include <atomic>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

#include "shared_test_helpers.h"

using namespace pylabhub::utils;

namespace
{
// --- Globals for static module tests ---
std::atomic<int> g_startup_counter{0};
void counter_startup_callback(const char *) { g_startup_counter++; }
void reset_static_counters() { g_startup_counter = 0; }

// --- Globals for dynamic module tests ---
std::atomic<int> dyn_A_start{0}, dyn_B_start{0}, dyn_C_start{0}, dyn_D_start{0};
std::atomic<int> dyn_A_stop{0}, dyn_B_stop{0}, dyn_C_stop{0}, dyn_D_stop{0};

void reset_dynamic_counters() {
    dyn_A_start=0; dyn_B_start=0; dyn_C_start=0; dyn_D_start=0;
    dyn_A_stop=0; dyn_B_stop=0; dyn_C_stop=0; dyn_D_stop=0;
}

void startup_A(const char *) { dyn_A_start++; }
void startup_B(const char *) { dyn_B_start++; }
void startup_C(const char *) { dyn_C_start++; }
void startup_D(const char *) { dyn_D_start++; }
void shutdown_A(const char *) { dyn_A_stop++; }
void shutdown_B(const char *) { dyn_B_stop++; }
void shutdown_C(const char *) { dyn_C_stop++; }
void shutdown_D(const char *) { dyn_D_stop++; }

} // namespace


// ============================================================================
// Static Lifecycle Workers
// ============================================================================

int pylabhub::tests::worker::lifecycle::test_multiple_guards_warning()
{
    testing::internal::CaptureStderr();
    LifecycleGuard guard1;
    LifecycleGuard guard2;
    std::string output = testing::internal::GetCapturedStderr();
    if (output.find("WARNING: LifecycleGuard constructed but an owner already exists.") ==
        std::string::npos)
        return 1;
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
    return 1; // Should not be reached
}

int pylabhub::tests::worker::lifecycle::test_unresolved_dependency()
{
    ModuleDef module_a("ModuleA");
    module_a.add_dependency("NonExistentModule");
    LifecycleGuard guard(std::move(module_a)); // Should abort
    return 1; // Should not be reached
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
    LifecycleGuard guard(std::move(module_a), std::move(module_b));

    return 1; // Should not be reached.
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

    if (!LoadModule("DynA"))
        return 2;
    if (!LoadModule("DynA"))
        return 3;
    if (dyn_A_start != 1)
        return 4;

    if (!UnloadModule("DynA"))
        return 5;
    if (dyn_A_stop != 0)
        return 6;

    if (!UnloadModule("DynA"))
        return 7;
    if (dyn_A_stop != 1)
        return 8;

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

    if (!LoadModule("DynA"))
        return 5;
    if (dyn_A_start != 1 || dyn_B_start != 1 || dyn_C_start != 1 || dyn_D_start != 1)
        return 6;

    if (!UnloadModule("DynA"))
        return 7;
    if (dyn_D_stop != 0)
        return 8;

    if (!UnloadModule("DynB"))
        return 9;
    if (dyn_D_stop != 0)
        return 10;

    if (!UnloadModule("DynC"))
        return 11;
    if (dyn_D_stop != 1)
        return 12;

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

int pylabhub::tests::worker::lifecycle::dynamic_static_dependency_fail()
{
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef dynMod("DynA");
    dynMod.add_dependency("NonExistentStaticMod");
    if (RegisterDynamicModule(std::move(dynMod)))
        return 1;

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_circular_dependency_fail()
{
    LifecycleGuard guard(Logger::GetLifecycleModule());

    ModuleDef modA("DynA");
    modA.add_dependency("DynB");
    if (!RegisterDynamicModule(std::move(modA)))
        return 1;

    ModuleDef modB("DynB");
    modB.add_dependency("DynA");
    if (!RegisterDynamicModule(std::move(modB)))
        return 2;

    if (LoadModule("DynA"))
        return 3;

    return 0;
}

int pylabhub::tests::worker::lifecycle::dynamic_reentrant_load_fail()
{
    struct ReentrantCallbacks
    {
        static void startup(const char *)
        {
            LoadModule("DynB");
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

    if (LoadModule("DynA"))
        return 3;

    return 0;
}
