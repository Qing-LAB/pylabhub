#include "utils/Lifecycle.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "shared_test_helpers.h" // For StringCapture

#include <vector>
#include <string>

namespace {
// A simple module that does nothing, for testing registration.
void null_callback(const char *) {}

// Global/static atomic counter for the test
std::atomic<int> global_startup_counter{0};

void global_startup_callback(const char* arg) {
    (void)arg; // Unused
    global_startup_counter++;
}

class LifecycleTest : public ::testing::Test {
protected:
    // void SetUp() override {
    //     // Reset the owner flag before each test to ensure isolation.
    //     // This is a bit of a hack, but necessary for testing the singleton guard.
    //     bool expected = true;
    //     pylabhub::lifecycle::LifecycleGuard::owner_flag().compare_exchange_strong(expected, false);
    // }
};

// Test that creating multiple LifecycleGuards results in only one owner
// and that a warning is printed for subsequent guards.
TEST_F(LifecycleTest, MultipleGuardsWarning) {
    testing::internal::CaptureStderr();

    // The first guard should become the owner.
    pylabhub::lifecycle::LifecycleGuard guard1;
    EXPECT_TRUE(pylabhub::lifecycle::IsInitialized());

    // The second guard should not become the owner.
    pylabhub::lifecycle::LifecycleGuard guard2;

    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_THAT(output, testing::HasSubstr("WARNING: LifecycleGuard constructed but an owner already exists."));
}

// Test that modules are correctly registered and initialized.
TEST_F(LifecycleTest, ModuleRegistrationAndInitialization) {
    testing::internal::CaptureStderr();

    global_startup_counter = 0; // Reset for this test

    ModuleDef module_a("ModuleA");
    module_a.set_startup(global_startup_callback); // Pass the C-style function pointer

    {
        pylabhub::lifecycle::LifecycleGuard guard(std::move(module_a));
        EXPECT_EQ(global_startup_counter.load(), 1);
    }

    std::string output = testing::internal::GetCapturedStderr();
    EXPECT_THAT(output, testing::HasSubstr("Startup sequence determined for 1 modules"));
    EXPECT_THAT(output, testing::HasSubstr("-> Starting module: 'ModuleA'"));
}

// Test that the is_initialized flag works as expected.
TEST_F(LifecycleTest, IsInitializedFlag) {
    EXPECT_FALSE(pylabhub::lifecycle::IsInitialized());
    {
        pylabhub::lifecycle::LifecycleGuard guard;
        EXPECT_TRUE(pylabhub::lifecycle::IsInitialized());
    }
    // After the guard is destroyed, the manager is finalized, but the "initialized"
    // flag should probably remain true, as it indicates that initialization *did* happen.
    // Let's test this assumption.
    EXPECT_TRUE(pylabhub::lifecycle::IsInitialized());
}

// Test that attempting to register a module after initialization aborts.
// This requires running in a separate process.
TEST_F(LifecycleTest, RegisterAfterInitAborts) {
    pylabhub::lifecycle::LifecycleGuard guard;
    EXPECT_DEATH({
        ModuleDef module_a("LateModule");
        pylabhub::lifecycle::RegisterModule(std::move(module_a));
    }, "Attempted to register module 'LateModule' after initialization has started");
}

} // namespace
