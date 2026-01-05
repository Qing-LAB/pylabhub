#include "lifecycle_workers.h"
#include "utils/Lifecycle.hpp"

#include <atomic>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

#include "shared_test_helpers.h" // For StringCapture

using namespace pylabhub::utils;

namespace
{
// A simple module that does nothing, for testing registration.
void null_callback(const char *) {}

// Global/static atomic counter for the test
std::atomic<int> global_startup_counter{0};

void global_startup_callback(const char *arg)
{
    (void)arg; // Unused
    global_startup_counter++;
}
} // namespace

int pylabhub::tests::worker::lifecycle::test_multiple_guards_warning()
{
    testing::internal::CaptureStderr();

    // The first guard should become the owner.
    LifecycleGuard guard1;
    if (!IsAppInitialized())
    {
        std::cerr << "App not initialized after first guard" << std::endl;
        return 1;
    }

    // The second guard should not become the owner.
    LifecycleGuard guard2;

    std::string output = testing::internal::GetCapturedStderr();
    if (output.find("WARNING: LifecycleGuard constructed but an owner already exists.") ==
        std::string::npos)
    {
        std::cerr << "Expected warning not found in stderr. Output was: " << output << std::endl;
        return 1;
    }

    return 0;
}

int pylabhub::tests::worker::lifecycle::test_module_registration_and_initialization()
{
    testing::internal::CaptureStderr();

    global_startup_counter = 0; // Reset for this test

    ModuleDef module_a("ModuleA");
    module_a.set_startup(global_startup_callback);

    {
        LifecycleGuard guard(std::move(module_a));
        if (global_startup_counter.load() != 1)
        {
            std::cerr << "Startup counter should be 1, but is " << global_startup_counter.load()
                      << std::endl;
            return 1;
        }
    }

    std::string output = testing::internal::GetCapturedStderr();
    if (output.find("Startup sequence determined for 1 modules") == std::string::npos)
    {
        std::cerr << "Expected startup sequence message not found. Output was: " << output
                  << std::endl;
        return 1;
    }
    if (output.find("-> Starting module: 'ModuleA'") == std::string::npos)
    {
        std::cerr << "Expected starting module message not found. Output was: " << output
                  << std::endl;
        return 1;
    }

    return 0;
}

int pylabhub::tests::worker::lifecycle::test_is_initialized_flag()
{
    if (IsAppInitialized())
    {
        std::cerr << "App should not be initialized before guard" << std::endl;
        return 1;
    }
    {
        LifecycleGuard guard;
        if (!IsAppInitialized())
        {
            std::cerr << "App should be initialized within guard scope" << std::endl;
            return 1;
        }
    }
    if (!IsAppInitialized())
    {
        std::cerr << "App should remain initialized after guard is destroyed" << std::endl;
        return 1;
    }
    return 0;
}

int pylabhub::tests::worker::lifecycle::test_register_after_init_aborts()
{
    // This worker is expected to be terminated by an abort signal.
    // The test runner is responsible for verifying this.
    LifecycleGuard guard;
    ModuleDef module_a("LateModule");
    RegisterModule(std::move(module_a));

    // This line should not be reached. If it is, the test has failed.
    return 1;
}

int pylabhub::tests::worker::lifecycle::test_unresolved_dependency()
{
    // This worker is expected to be terminated by an abort signal
    // because "NonExistentModule" does not exist.
    ModuleDef module_a("ModuleA");
    module_a.add_dependency("NonExistentModule");
    LifecycleGuard guard(std::move(module_a)); // This is expected to cause an abort

    // This line should not be reached if the abort happens as expected.
    // If it is reached, the test has failed, as the process should have aborted.
    return 1;
}

int pylabhub::tests::worker::lifecycle::test_case_insensitive_dependency()
{
    try
    {
        ModuleDef module_a("ModuleA");
        module_a.add_dependency("moduleb"); // Note the lowercase 'm'
        ModuleDef module_b("ModuleB");
        LifecycleGuard guard(std::move(module_a), std::move(module_b));
    }
    catch (...)
    {
        // This should not throw, so if it does, the test fails.
        return 1;
    }
    return 0; // Should succeed
}
