#include "lifecycle_workers.h"
#include "test_entrypoint.h"
#include "plh_datahub.hpp"
#include "shared_test_helpers.h"
#include "test_process_utils.h"
#include "gtest/gtest.h"

using namespace pylabhub::utils;
using namespace pylabhub::tests::helper;

namespace
{
// --- Globals for static module tests ---
std::atomic<int> g_startup_counter{0};
void counter_startup_callback(const char *, void *)
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

void startup_A(const char *, void *)
{
    dyn_A_start++;
}
void startup_B(const char *, void *)
{
    dyn_B_start++;
}
void startup_C(const char *, void *)
{
    dyn_C_start++;
}
void startup_D(const char *, void *)
{
    dyn_D_start++;
}
void startup_E(const char *, void *)
{
    dyn_E_start++;
}
void shutdown_A(const char *, void *)
{
    dyn_A_stop++;
}
void shutdown_B(const char *, void *)
{
    dyn_B_stop++;
}
void shutdown_C(const char *, void *)
{
    dyn_C_stop++;
}
void shutdown_D(const char *, void *)
{
    dyn_D_stop++;
}
void shutdown_E(const char *, void *)
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
    return run_worker_bare([&]() {
        reset_static_counters();
        ModuleDef module_a("ModuleA");
        module_a.set_startup(counter_startup_callback);
        LifecycleGuard guard(std::move(module_a));
        ASSERT_EQ(g_startup_counter.load(), 1) << "startup callback must run exactly once";
    }, "lifecycle::test_module_registration_and_initialization");
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
    return run_worker_bare([&]() {
        ASSERT_FALSE(IsAppInitialized()) << "must not be initialized before LifecycleGuard";
        LifecycleGuard guard;
        ASSERT_TRUE(IsAppInitialized()) << "must be initialized after LifecycleGuard";
    }, "lifecycle::test_is_initialized_flag");
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
// Init/finalize idempotency and is_finalized
// ============================================================================

int pylabhub::tests::worker::lifecycle::test_init_idempotency()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard(Logger::GetLifecycleModule());
        ASSERT_TRUE(IsAppInitialized());
        InitializeApp(); // second call must be a no-op
        ASSERT_TRUE(IsAppInitialized()) << "InitializeApp() must remain initialized after second call";
    }, "lifecycle::test_init_idempotency");
}

int pylabhub::tests::worker::lifecycle::test_finalize_idempotency()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard(Logger::GetLifecycleModule());
        ASSERT_TRUE(IsAppInitialized());
        FinalizeApp(); // first call; guard destructor will call again — must be idempotent
    }, "lifecycle::test_finalize_idempotency");
}

int pylabhub::tests::worker::lifecycle::test_is_finalized_flag()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard(Logger::GetLifecycleModule());
        ASSERT_TRUE(IsAppInitialized());
        ASSERT_FALSE(IsAppFinalized()) << "must not be finalized before FinalizeApp()";
        FinalizeApp();
        ASSERT_TRUE(IsAppFinalized()) << "must be finalized after FinalizeApp()";
    }, "lifecycle::test_is_finalized_flag");
}

// ============================================================================
// Dynamic Lifecycle Workers
// ============================================================================

int pylabhub::tests::worker::lifecycle::dynamic_register_before_init_fail()
{
    return run_worker_bare([&]() {
        ModuleDef mod("DynA");
        ASSERT_FALSE(RegisterDynamicModule(std::move(mod)))
            << "RegisterDynamicModule must fail before lifecycle is initialized";
    }, "lifecycle::dynamic.register_before_init_fail");
}

int pylabhub::tests::worker::lifecycle::dynamic_load_unload()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef mod("DynA");
        mod.set_startup(startup_A);
        mod.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(mod)));

        ASSERT_TRUE(LoadModule("DynA"));
        ASSERT_EQ(dyn_A_start.load(), 1) << "startup must run exactly once";

        ASSERT_TRUE(UnloadModule("DynA"));
        WaitForUnload("DynA");
        ASSERT_EQ(dyn_A_stop.load(), 1) << "shutdown must run exactly once";
    }, "lifecycle::dynamic.load_unload");
}

int pylabhub::tests::worker::lifecycle::dynamic_ref_counting()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef mod("DynA");
        mod.set_startup(startup_A);
        mod.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(mod)));

        ASSERT_TRUE(LoadModule("DynA"));
        ASSERT_EQ(dyn_A_start.load(), 1) << "first load: startup must run once";

        // Second load increments ref-count; startup must NOT run again.
        ASSERT_TRUE(LoadModule("DynA"));
        ASSERT_EQ(dyn_A_start.load(), 1) << "second load: startup must not re-run";

        // Single unload decrements ref-count to zero; shutdown must run.
        ASSERT_TRUE(UnloadModule("DynA"));
        WaitForUnload("DynA");
        ASSERT_EQ(dyn_A_stop.load(), 1) << "unload: shutdown must run exactly once";
    }, "lifecycle::dynamic.ref_counting");
}

int pylabhub::tests::worker::lifecycle::dynamic_dependency_chain()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef modB("DynB");
        modB.set_startup(startup_B);
        modB.set_shutdown(shutdown_B, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modB)));

        ModuleDef modA("DynA");
        modA.add_dependency("DynB");
        modA.set_startup(startup_A);
        modA.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modA)));

        ASSERT_TRUE(LoadModule("DynA"));
        ASSERT_EQ(dyn_A_start.load(), 1) << "A must start";
        ASSERT_EQ(dyn_B_start.load(), 1) << "B (dependency) must start";

        // Unloading A must cascade-unload B.
        ASSERT_TRUE(UnloadModule("DynA"));
        WaitForUnload("DynA");
        ASSERT_EQ(dyn_A_stop.load(), 1) << "A must stop";
        ASSERT_EQ(dyn_B_stop.load(), 1) << "B (dependency) must stop";
    }, "lifecycle::dynamic.dependency_chain");
}

int pylabhub::tests::worker::lifecycle::dynamic_diamond_dependency()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef modD("DynD");
        modD.set_startup(startup_D);
        modD.set_shutdown(shutdown_D, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modD)));

        ModuleDef modB("DynB");
        modB.add_dependency("DynD");
        modB.set_startup(startup_B);
        modB.set_shutdown(shutdown_B, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modB)));

        ModuleDef modC("DynC");
        modC.add_dependency("DynD");
        modC.set_startup(startup_C);
        modC.set_shutdown(shutdown_C, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modC)));

        ModuleDef modA("DynA");
        modA.add_dependency("DynB");
        modA.add_dependency("DynC");
        modA.set_startup(startup_A);
        modA.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modA)));

        // --- SCENARIO 1: Unload from the top ---
        ASSERT_TRUE(LoadModule("DynA"));
        ASSERT_EQ(dyn_A_start.load(), 1);
        ASSERT_EQ(dyn_B_start.load(), 1);
        ASSERT_EQ(dyn_C_start.load(), 1);
        ASSERT_EQ(dyn_D_start.load(), 1) << "D (shared dep) must start exactly once";

        ASSERT_TRUE(UnloadModule("DynA"));
        WaitForUnload("DynA");
        ASSERT_EQ(dyn_A_stop.load(), 1);
        ASSERT_EQ(dyn_B_stop.load(), 1);
        ASSERT_EQ(dyn_C_stop.load(), 1);
        ASSERT_EQ(dyn_D_stop.load(), 1) << "D must stop once after full cascade";

        // --- SCENARIO 2: Unload side branches ---
        reset_dynamic_counters();
        ModuleDef modD2("DynD");
        modD2.set_startup(startup_D);
        modD2.set_shutdown(shutdown_D, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modD2)));
        ModuleDef modB2("DynB");
        modB2.add_dependency("DynD");
        modB2.set_startup(startup_B);
        modB2.set_shutdown(shutdown_B, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modB2)));
        ModuleDef modC2("DynC");
        modC2.add_dependency("DynD");
        modC2.set_startup(startup_C);
        modC2.set_shutdown(shutdown_C, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modC2)));

        ASSERT_TRUE(LoadModule("DynB"));
        ASSERT_TRUE(LoadModule("DynC"));
        ASSERT_EQ(dyn_B_start.load(), 1);
        ASSERT_EQ(dyn_C_start.load(), 1);
        ASSERT_EQ(dyn_D_start.load(), 1) << "D started once (shared)";
        ASSERT_EQ(dyn_A_start.load(), 0) << "A was not loaded";

        // D has ref_count==2; unload must be rejected.
        ASSERT_FALSE(UnloadModule("DynD")) << "D must not unload while B and C hold it";
        ASSERT_EQ(dyn_D_stop.load(), 0);

        // Unload B; D ref_count drops to 1 — still held by C.
        ASSERT_TRUE(UnloadModule("DynB"));
        WaitForUnload("DynB");
        ASSERT_EQ(dyn_B_stop.load(), 1);
        ASSERT_EQ(dyn_D_stop.load(), 0) << "D must not stop while C still holds it";

        ASSERT_FALSE(UnloadModule("DynD")) << "D must still be rejected (ref_count==1)";
        ASSERT_EQ(dyn_D_stop.load(), 0);

        // Unload C; D ref_count drops to 0 — cascade stop.
        ASSERT_TRUE(UnloadModule("DynC"));
        WaitForUnload("DynC");
        ASSERT_EQ(dyn_C_stop.load(), 1);
        ASSERT_EQ(dyn_D_stop.load(), 1) << "D must stop after last holder is unloaded";
    }, "lifecycle::dynamic.diamond_dependency");
}

int pylabhub::tests::worker::lifecycle::dynamic_finalize_unloads_all()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        {
            LifecycleGuard guard(Logger::GetLifecycleModule());
            ModuleDef mod("DynA");
            mod.set_startup(startup_A);
            mod.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
            ASSERT_TRUE(RegisterDynamicModule(std::move(mod)));
            ASSERT_TRUE(LoadModule("DynA"));
            // Guard destructs here, triggering finalize() which must unload DynA.
        }
        ASSERT_EQ(dyn_A_stop.load(), 1) << "finalize() must have called the shutdown callback";
    }, "lifecycle::dynamic.finalize_unloads_all");
}

int pylabhub::tests::worker::lifecycle::dynamic_persistent_in_middle()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef modE("DynE");
        modE.set_startup(startup_E);
        modE.set_shutdown(shutdown_E, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modE)));

        ModuleDef modD("DynD");
        modD.set_startup(startup_D);
        modD.set_shutdown(shutdown_D, std::chrono::milliseconds(100));
        modD.set_as_persistent();
        ASSERT_TRUE(RegisterDynamicModule(std::move(modD)));

        ModuleDef modC("DynC");
        modC.add_dependency("DynE");
        modC.set_startup(startup_C);
        modC.set_shutdown(shutdown_C, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modC)));

        ModuleDef modB("DynB");
        modB.add_dependency("DynD");
        modB.set_startup(startup_B);
        modB.set_shutdown(shutdown_B, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modB)));

        ModuleDef modA("DynA");
        modA.add_dependency("DynB");
        modA.add_dependency("DynC");
        modA.set_startup(startup_A);
        modA.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modA)));

        ASSERT_TRUE(LoadModule("DynA")) << "full graph load must succeed";
        ASSERT_EQ(dyn_A_start.load(), 1);
        ASSERT_EQ(dyn_B_start.load(), 1);
        ASSERT_EQ(dyn_C_start.load(), 1);
        ASSERT_EQ(dyn_D_start.load(), 1);
        ASSERT_EQ(dyn_E_start.load(), 1);

        ASSERT_TRUE(UnloadModule("DynA"));
        WaitForUnload("DynA");
        ASSERT_EQ(dyn_A_stop.load(), 1);
        ASSERT_EQ(dyn_B_stop.load(), 1);
        ASSERT_EQ(dyn_C_stop.load(), 1);
        ASSERT_EQ(dyn_E_stop.load(), 1);
        ASSERT_EQ(dyn_D_stop.load(), 0) << "persistent module DynD must NOT be stopped by unload";
    }, "lifecycle::dynamic.persistent_in_middle");
}

int pylabhub::tests::worker::lifecycle::dynamic_static_dependency_fail()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard(Logger::GetLifecycleModule());
        ModuleDef dynMod("DynA");
        dynMod.add_dependency("NonExistentStaticMod");
        ASSERT_FALSE(RegisterDynamicModule(std::move(dynMod)))
            << "registration must fail: dependency 'NonExistentStaticMod' does not exist";
    }, "lifecycle::dynamic.static_dependency_fail");
}

int pylabhub::tests::worker::lifecycle::registration_fails_with_unresolved_dependency()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard(Logger::GetLifecycleModule());
        ModuleDef modA("DynA");
        modA.add_dependency("DynB"); // DynB not registered
        ASSERT_FALSE(RegisterDynamicModule(std::move(modA)))
            << "registration must fail: dependency 'DynB' is not registered";
    }, "lifecycle::registration_fails_with_unresolved_dependency");
}

int pylabhub::tests::worker::lifecycle::dynamic_reentrant_load_fail()
{
    return run_worker_bare([&]() {
        struct ReentrantCallbacks
        {
            static void startup(const char *, void *)
            {
                // The re-entrant LoadModule("DynB") must be rejected (return false).
                // Either way, we throw to signal failure back to the outer LoadModule("DynA").
                if (LoadModule("DynB"))
                    throw std::runtime_error("Re-entrant LoadModule('DynB') unexpectedly succeeded!");
                throw std::runtime_error(
                    "LoadModule('DynB') detected re-entrant call and failed as expected.");
            }
        };

        LifecycleGuard guard(Logger::GetLifecycleModule());
        ModuleDef modB("DynB");
        ASSERT_TRUE(RegisterDynamicModule(std::move(modB)));

        ModuleDef modA("DynA");
        modA.set_startup(ReentrantCallbacks::startup);
        ASSERT_TRUE(RegisterDynamicModule(std::move(modA)));

        // DynA's startup always throws, so LoadModule("DynA") must return false.
        ASSERT_FALSE(LoadModule("DynA"))
            << "re-entrant load must be rejected; LoadModule('DynA') must return false";
    }, "lifecycle::dynamic.reentrant_load_fail");
}

int pylabhub::tests::worker::lifecycle::dynamic_persistent_module()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef mod_perm("DynPerm");
        mod_perm.set_startup(startup_D); // Using D's counters
        mod_perm.set_shutdown(shutdown_D, std::chrono::milliseconds(100));
        mod_perm.set_as_persistent();
        ASSERT_TRUE(RegisterDynamicModule(std::move(mod_perm)));

        ModuleDef mod_a("DynA");
        mod_a.add_dependency("DynPerm");
        mod_a.set_startup(startup_A);
        mod_a.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(mod_a)));

        ASSERT_TRUE(LoadModule("DynA"));
        ASSERT_EQ(dyn_A_start.load(), 1);
        ASSERT_EQ(dyn_D_start.load(), 1);

        ASSERT_TRUE(UnloadModule("DynA"));
        WaitForUnload("DynA");
        ASSERT_EQ(dyn_A_stop.load(), 1);
        ASSERT_EQ(dyn_D_stop.load(), 0) << "persistent module DynPerm must NOT be stopped by unload";

        // finalize() will shut it down when guard goes out of scope.
    }, "lifecycle::dynamic.persistent_module");
}

int pylabhub::tests::worker::lifecycle::dynamic_persistent_module_finalize()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        {
            LifecycleGuard guard(Logger::GetLifecycleModule());

            ModuleDef mod_perm("DynPerm");
            mod_perm.set_startup(startup_D);
            mod_perm.set_shutdown(shutdown_D, std::chrono::milliseconds(100));
            mod_perm.set_as_persistent();
            ASSERT_TRUE(RegisterDynamicModule(std::move(mod_perm)));

            ASSERT_TRUE(LoadModule("DynPerm"));
            ASSERT_EQ(dyn_D_start.load(), 1);

            // Don't unload; let the LifecycleGuard handle it.
        }
        // At this point, finalize() has been called.
        ASSERT_EQ(dyn_D_stop.load(), 1) << "persistent module DynPerm must be stopped by finalize()";
    }, "lifecycle::dynamic.persistent_module_finalize");
}

// ============================================================================
// Owner-managed teardown — see HEP-CORE-0001 §"Owner-managed teardown" /
// `ModuleDef::set_owner_managed_teardown`.
// ============================================================================

namespace
{
// Userdata pattern used by the owner-managed teardown tests below: a
// simple atomic flag that the validator reads.  The "owner" simulates
// its destructor's synchronous teardown by flipping this flag to false
// BEFORE calling UnloadModule, then re-checking the lifecycle state
// after WaitForUnload returns.  Mirrors the ThreadManager pattern in
// `src/utils/service/thread_manager.cpp::tm_impl_validate`.
struct OwnerManagedUserdata
{
    std::atomic<bool> alive{true};
};

bool owner_managed_validator(void *userdata, uint64_t /*key*/) noexcept
{
    auto *ud = static_cast<OwnerManagedUserdata *>(userdata);
    return ud != nullptr && ud->alive.load(std::memory_order_acquire);
}

std::atomic<int> g_owner_managed_callback_runs{0};
void owner_managed_shutdown_callback(const char *, void *)
{
    // The flag-set test (clean unload) must NOT see this fire — when
    // the validator returns false, the lifecycle layer skips the
    // callback.  The flag-clear test (default anomaly) must ALSO NOT
    // see this fire — same reason: validator-fail skips the callback.
    // So a non-zero count from EITHER test indicates a regression where
    // the lifecycle layer ran the callback despite the validator
    // returning false.
    g_owner_managed_callback_runs.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

int pylabhub::tests::worker::lifecycle::dynamic_owner_managed_teardown_clean_unload()
{
    return run_worker_bare([&]() {
        g_owner_managed_callback_runs.store(0);
        LifecycleGuard guard(Logger::GetLifecycleModule());

        OwnerManagedUserdata ud;

        // Register WITH the owner-managed flag.
        {
            ModuleDef mod("OwnerManagedDyn", &ud, owner_managed_validator);
            mod.set_startup([](const char *, void *) {});
            mod.set_shutdown(owner_managed_shutdown_callback,
                             std::chrono::milliseconds(100));
            mod.set_owner_managed_teardown(true);
            ASSERT_TRUE(RegisterDynamicModule(std::move(mod)));
        }
        ASSERT_TRUE(LoadModule("OwnerManagedDyn"));
        ASSERT_EQ(GetDynamicModuleState("OwnerManagedDyn"),
                  DynModuleState::Loaded);

        // Owner "destroys itself": flip alive=false BEFORE UnloadModule.
        // The async unload worker will run, find validator-fail, take
        // the owner-managed branch, and clean the graph.
        ud.alive.store(false, std::memory_order_release);
        ASSERT_TRUE(UnloadModule("OwnerManagedDyn"));
        const auto wait_result =
            WaitForUnload("OwnerManagedDyn", std::chrono::seconds(2));

        // Owner-managed branch removes the module from the graph (matches
        // the success-path cleanup), so wait returns NotRegistered, NOT
        // ShutdownFailed.
        EXPECT_EQ(wait_result, DynModuleState::NotRegistered)
            << "owner-managed teardown must clean the graph entry "
               "(NotRegistered), not contaminate (ShutdownFailed)";

        // Shutdown callback must NOT have run.
        EXPECT_EQ(g_owner_managed_callback_runs.load(), 0)
            << "owner-managed teardown skips the callback (validator-fail "
               "is the documented no-callback signal)";

        // Re-registration with the same name must succeed (graph entry
        // freed).  This is the canonical use case — owner constructed a
        // new instance with the same name after the previous one's dtor.
        OwnerManagedUserdata ud2;
        {
            ModuleDef mod2("OwnerManagedDyn", &ud2, owner_managed_validator);
            mod2.set_startup([](const char *, void *) {});
            mod2.set_shutdown(owner_managed_shutdown_callback,
                              std::chrono::milliseconds(100));
            mod2.set_owner_managed_teardown(true);
            ASSERT_TRUE(RegisterDynamicModule(std::move(mod2)))
                << "owner-managed teardown must free the module name for "
                   "re-registration";
        }
        // Clean teardown of the second instance: flip alive=false +
        // unload, so the test exits cleanly.
        ASSERT_TRUE(LoadModule("OwnerManagedDyn"));
        ud2.alive.store(false, std::memory_order_release);
        ASSERT_TRUE(UnloadModule("OwnerManagedDyn"));
        (void)WaitForUnload("OwnerManagedDyn", std::chrono::seconds(2));
    }, "lifecycle::dynamic.owner_managed_teardown_clean_unload");
}

int pylabhub::tests::worker::lifecycle::dynamic_validator_fail_default_anomaly()
{
    return run_worker_bare([&]() {
        g_owner_managed_callback_runs.store(0);
        LifecycleGuard guard(Logger::GetLifecycleModule());

        OwnerManagedUserdata ud;

        // Register WITHOUT the owner-managed flag — HEP-0001 default.
        {
            ModuleDef mod("AnomalyDyn", &ud, owner_managed_validator);
            mod.set_startup([](const char *, void *) {});
            mod.set_shutdown(owner_managed_shutdown_callback,
                             std::chrono::milliseconds(100));
            // No set_owner_managed_teardown — default false.
            ASSERT_TRUE(RegisterDynamicModule(std::move(mod)));
        }
        ASSERT_TRUE(LoadModule("AnomalyDyn"));
        ASSERT_EQ(GetDynamicModuleState("AnomalyDyn"),
                  DynModuleState::Loaded);

        ud.alive.store(false, std::memory_order_release);
        ASSERT_TRUE(UnloadModule("AnomalyDyn"));

        // Validator-fail in the default branch:
        //   - WARN logged ("userdata validation failed — skipping
        //     shutdown callback") — visible in stderr; this test does
        //     NOT capture/assert it (covered by HubHostTest /
        //     AdminServiceTest LogCaptureFixture rollouts in §6).
        //   - status set to FAILED_SHUTDOWN
        //   - module marked contaminated
        //   - graph entry RETAINED
        //
        // Note: WaitForUnload uses a short timeout because the default
        // anomaly path does not erase the closure entry, so wait will
        // run to its full timeout.  This is by design for the default
        // semantics (HEP-0001) and is verified explicitly in this test.
        const auto wait_result =
            WaitForUnload("AnomalyDyn", std::chrono::milliseconds(300));
        EXPECT_EQ(wait_result, DynModuleState::Unloading)
            << "default anomaly path leaves the closure tracking in "
               "place (HEP-CORE-0001 protocol); WaitForUnload must time "
               "out as Unloading rather than report a clean termination";

        // Re-registration must FAIL (contaminated dependency / name in graph).
        {
            OwnerManagedUserdata ud2;
            ModuleDef mod2("AnomalyDyn", &ud2, owner_managed_validator);
            mod2.set_startup([](const char *, void *) {});
            mod2.set_shutdown(owner_managed_shutdown_callback,
                              std::chrono::milliseconds(100));
            EXPECT_FALSE(RegisterDynamicModule(std::move(mod2)))
                << "default anomaly path retains the module entry — "
                   "re-registration with the same name must fail";
        }

        // Shutdown callback must NOT have run (validator-fail skips it
        // in BOTH branches — only difference is graph cleanup).
        EXPECT_EQ(g_owner_managed_callback_runs.load(), 0);
    }, "lifecycle::dynamic.validator_fail_default_anomaly");
}

int pylabhub::tests::worker::lifecycle::dynamic_unload_timeout()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef mod("HangingModule");
        mod.set_shutdown(
            [](const char *, void *)
            {
                PLH_DEBUG("HangingModule shutdown started, sleeping for 250ms...");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                PLH_DEBUG("HangingModule shutdown finished sleep.");
            },
            std::chrono::milliseconds(50));

        ASSERT_TRUE(RegisterDynamicModule(std::move(mod)));
        ASSERT_TRUE(LoadModule("HangingModule"));

        // This should return in ~50ms, not hang for 250ms.
        // The test runner will verify the "TIMEOUT!" message in stderr.
        ASSERT_TRUE(UnloadModule("HangingModule"));
    }, "lifecycle::dynamic.unload_timeout");
}

// ============================================================================
// Null/overflow input validation (need lifecycle initialized to reach the check)
// ============================================================================

int pylabhub::tests::worker::lifecycle::load_module_null_returns_false()
{
    return run_worker_bare([&]() {
        // With string_view API, there is no null; empty string is the invalid-name equivalent.
        LifecycleGuard guard;
        ASSERT_FALSE(LoadModule("")) << "LoadModule with empty name must return false";
    }, "lifecycle::load_module_null_returns_false");
}

int pylabhub::tests::worker::lifecycle::load_module_overflow_returns_false()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard;
        std::string long_name(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'z');
        ASSERT_FALSE(LoadModule(long_name.c_str()))
            << "LoadModule with name exceeding MAX_MODULE_NAME_LEN must return false";
    }, "lifecycle::load_module_overflow_returns_false");
}

int pylabhub::tests::worker::lifecycle::unload_module_null_returns_false()
{
    return run_worker_bare([&]() {
        // With string_view API, there is no null; empty string is the invalid-name equivalent.
        LifecycleGuard guard;
        ASSERT_FALSE(UnloadModule("")) << "UnloadModule with empty name must return false";
    }, "lifecycle::unload_module_null_returns_false");
}

int pylabhub::tests::worker::lifecycle::unload_module_overflow_returns_false()
{
    return run_worker_bare([&]() {
        LifecycleGuard guard;
        std::string long_name(ModuleDef::MAX_MODULE_NAME_LEN + 1, 'w');
        ASSERT_FALSE(UnloadModule(long_name.c_str()))
            << "UnloadModule with name exceeding MAX_MODULE_NAME_LEN must return false";
    }, "lifecycle::unload_module_overflow_returns_false");
}

// ============================================================================
// Log sink injection workers
// ============================================================================

/**
 * @brief Installs a log sink, then triggers the "cannot unload — in use" warning
 *        by trying to unload a module that still has a dependent loaded.
 *
 * Verifies that the message was routed through the sink by writing it to stderr
 * with a "LIFECYCLE_SINK:" prefix.  The parent test checks for that prefix.
 */
int pylabhub::tests::worker::lifecycle::log_sink_routes_warning()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        // Register and load DynA (the dependency).
        ModuleDef modA("DynA");
        modA.set_startup(startup_A);
        modA.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modA)));

        // Register and load DynB (depends on DynA).
        ModuleDef modB("DynB");
        modB.add_dependency("DynA");
        modB.set_startup(startup_B);
        modB.set_shutdown(shutdown_B, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modB)));

        ASSERT_TRUE(LoadModule("DynB"));

        // Install a sink that writes to stderr with a distinguishable prefix.
        SetLifecycleLogSink(
            [](pylabhub::utils::LifecycleLogLevel /*level*/, const std::string &msg)
            {
                fmt::print(stderr, "LIFECYCLE_SINK: {}\n", msg);
                std::fflush(stderr);
            });

        // Attempt to unload DynA while DynB still depends on it — this must fail
        // and emit the "Cannot unload module" warning through the sink.
        ASSERT_FALSE(UnloadModule("DynA")) << "Unload of DynA must fail while DynB holds it";
    }, "lifecycle::log_sink_routes_warning");
}

/**
 * @brief Installs a sink, immediately clears it, then triggers the same warning.
 *
 * Verifies that after clearing the sink the message does NOT appear with the
 * "LIFECYCLE_SINK:" prefix (it falls back to PLH_DEBUG instead).
 */
int pylabhub::tests::worker::lifecycle::log_sink_cleared_uses_fallback()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        LifecycleGuard guard(Logger::GetLifecycleModule());

        ModuleDef modA("DynA");
        modA.set_startup(startup_A);
        modA.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modA)));

        ModuleDef modB("DynB");
        modB.add_dependency("DynA");
        modB.set_startup(startup_B);
        modB.set_shutdown(shutdown_B, std::chrono::milliseconds(100));
        ASSERT_TRUE(RegisterDynamicModule(std::move(modB)));

        ASSERT_TRUE(LoadModule("DynB"));

        // Install then immediately clear the sink.
        SetLifecycleLogSink(
            [](pylabhub::utils::LifecycleLogLevel /*level*/, const std::string &msg)
            {
                fmt::print(stderr, "LIFECYCLE_SINK: {}\n", msg);
                std::fflush(stderr);
            });
        ClearLifecycleLogSink();

        // The warning should NOT go through the sink.
        ASSERT_FALSE(UnloadModule("DynA")) << "Unload of DynA must fail while DynB holds it";
    }, "lifecycle::log_sink_cleared_uses_fallback");
}

// ============================================================================
// Async unload + finalize interaction workers
// ============================================================================

/**
 * @brief Schedules an async unload then lets finalize() drain the thread.
 *
 * This is the "fire and forget" unload pattern — no WaitForUnload before the
 * LifecycleGuard goes out of scope.  Verifies that Phase 1 of finalize() (join
 * the shutdown thread) correctly waits for the pending unload to complete so
 * the stop callback is always observed.
 */
int pylabhub::tests::worker::lifecycle::finalize_waits_for_pending_async_unload()
{
    return run_worker_bare([&]() {
        reset_dynamic_counters();
        {
            LifecycleGuard guard(Logger::GetLifecycleModule());

            ModuleDef modA("DynA");
            modA.set_startup(startup_A);
            modA.set_shutdown(shutdown_A, std::chrono::milliseconds(100));
            ASSERT_TRUE(RegisterDynamicModule(std::move(modA)));

            ASSERT_TRUE(LoadModule("DynA"));
            ASSERT_EQ(dyn_A_start.load(), 1);

            // Schedule async unload — deliberately skip WaitForUnload.
            // finalize() (triggered by guard destructor) must drain the shutdown
            // thread before returning, so the stop counter is observable here.
            ASSERT_TRUE(UnloadModule("DynA"));

            // Guard destructor calls finalize() here, joining the shutdown thread.
        }

        // After finalize: the shutdown callback must have run exactly once.
        ASSERT_EQ(dyn_A_stop.load(), 1) << "finalize() must join the shutdown thread";
    }, "lifecycle::finalize_waits_for_pending_async_unload");
}

/**
 * @brief Verifies that the lifecycle log sink is safe during async shutdown
 *        failure that overlaps with the final shutdown sequence.
 *
 * Timeline:
 *  1. Logger is running — its log sink is installed.
 *  2. A module with a short timeout is loaded and unloaded (async).
 *  3. The shutdown callback hangs past its timeout.
 *  4. finalize() is called (via LifecycleGuard destructor):
 *     - Phase 1: joins the shutdown thread.
 *       timedShutdown detaches the hung callback thread and records SHUTDOWN_TIMEOUT.
 *       lifecycleError() is called from the shutdown thread — routes through the
 *       log sink → LOGGER_ERROR while the logger is still running.
 *     - Phase 3: Logger static module shuts down.
 *       do_logger_shutdown calls ClearLifecycleLogSink() first, then drains queue.
 *  5. Process exits cleanly with no crash.
 *
 * The test verifies:
 *  - Process exits with code 0.
 *  - The destabilization error message appeared in stderr (via logger or PLH_DEBUG).
 */
int pylabhub::tests::worker::lifecycle::finalize_sink_safe_during_async_failure()
{
    return run_worker_bare([&]() {
        {
            LifecycleGuard guard(Logger::GetLifecycleModule());

            ModuleDef mod("SlowShutdown");
            mod.set_shutdown(
                [](const char *, void *)
                {
                    // Hang longer than the timeout so timedShutdown detaches us.
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                },
                std::chrono::milliseconds(50));

            ASSERT_TRUE(RegisterDynamicModule(std::move(mod)));
            ASSERT_TRUE(LoadModule("SlowShutdown"));

            // Schedule async unload without waiting.
            ASSERT_TRUE(UnloadModule("SlowShutdown"));

            // Guard destructor triggers finalize() here.
            // Phase 1 joins the shutdown thread:
            //   timedShutdown fires after 50ms, detaches the hung callback thread.
            //   lifecycleError("ERROR: ... shutdown TIMED OUT ...") is called.
            //   The log sink routes this to LOGGER_ERROR while the logger is alive.
            // Phase 3 shuts down Logger — do_logger_shutdown removes the sink first.
        }
    }, "lifecycle::finalize_sink_safe_during_async_failure");
}

// Self-registering dispatcher — no separate dispatcher file needed.
namespace
{
struct LifecycleWorkerRegistrar
{
    LifecycleWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "lifecycle")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::lifecycle;
                if (scenario == "test_multiple_guards_warning")
                    return test_multiple_guards_warning();
                if (scenario == "test_module_registration_and_initialization")
                    return test_module_registration_and_initialization();
                if (scenario == "test_is_initialized_flag")
                    return test_is_initialized_flag();
                if (scenario == "test_register_after_init_aborts")
                    return test_register_after_init_aborts();
                if (scenario == "test_unresolved_dependency")
                    return test_unresolved_dependency();
                if (scenario == "test_case_insensitive_dependency")
                    return test_case_insensitive_dependency();
                if (scenario == "test_static_circular_dependency_aborts")
                    return test_static_circular_dependency_aborts();
                if (scenario == "test_static_elaborate_indirect_cycle_aborts")
                    return test_static_elaborate_indirect_cycle_aborts();
                if (scenario == "test_init_idempotency")
                    return test_init_idempotency();
                if (scenario == "test_finalize_idempotency")
                    return test_finalize_idempotency();
                if (scenario == "test_is_finalized_flag")
                    return test_is_finalized_flag();
                if (scenario == "dynamic.register_before_init_fail")
                    return dynamic_register_before_init_fail();
                if (scenario == "dynamic.load_unload")
                    return dynamic_load_unload();
                if (scenario == "dynamic.ref_counting")
                    return dynamic_ref_counting();
                if (scenario == "dynamic.dependency_chain")
                    return dynamic_dependency_chain();
                if (scenario == "dynamic.diamond_dependency")
                    return dynamic_diamond_dependency();
                if (scenario == "dynamic.finalize_unloads_all")
                    return dynamic_finalize_unloads_all();
                if (scenario == "dynamic.persistent_in_middle")
                    return dynamic_persistent_in_middle();
                if (scenario == "dynamic.static_dependency_fail")
                    return dynamic_static_dependency_fail();
                if (scenario == "registration_fails_with_unresolved_dependency")
                    return registration_fails_with_unresolved_dependency();
                if (scenario == "dynamic.reentrant_load_fail")
                    return dynamic_reentrant_load_fail();
                if (scenario == "dynamic.persistent_module")
                    return dynamic_persistent_module();
                if (scenario == "dynamic.persistent_module_finalize")
                    return dynamic_persistent_module_finalize();
                if (scenario == "dynamic.unload_timeout")
                    return dynamic_unload_timeout();
                if (scenario == "dynamic.owner_managed_teardown_clean_unload")
                    return dynamic_owner_managed_teardown_clean_unload();
                if (scenario == "dynamic.validator_fail_default_anomaly")
                    return dynamic_validator_fail_default_anomaly();
                if (scenario == "load_module_null_returns_false")
                    return load_module_null_returns_false();
                if (scenario == "load_module_overflow_returns_false")
                    return load_module_overflow_returns_false();
                if (scenario == "unload_module_null_returns_false")
                    return unload_module_null_returns_false();
                if (scenario == "unload_module_overflow_returns_false")
                    return unload_module_overflow_returns_false();
                if (scenario == "log_sink_routes_warning")
                    return log_sink_routes_warning();
                if (scenario == "log_sink_cleared_uses_fallback")
                    return log_sink_cleared_uses_fallback();
                if (scenario == "finalize_waits_for_pending_async_unload")
                    return finalize_waits_for_pending_async_unload();
                if (scenario == "finalize_sink_safe_during_async_failure")
                    return finalize_sink_safe_during_async_failure();
                fmt::print(stderr, "ERROR: Unknown lifecycle scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static LifecycleWorkerRegistrar g_lifecycle_registrar;
} // namespace
