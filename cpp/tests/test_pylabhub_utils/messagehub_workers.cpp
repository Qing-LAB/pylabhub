#include "messagehub_workers.h"
#include "plh_datahub.hpp"
#include "shared_test_helpers.h"


namespace pylabhub::tests::worker::messagehub
{

int lifecycle_initialized_follows_state(int, char **)
{
    // This worker function now correctly uses the run_gtest_worker helper.
    // The helper creates a LifecycleGuard that manages the provided modules.
    return pylabhub::tests::helper::run_gtest_worker(
        []() {
            // The LifecycleGuard inside the helper has already initialized the modules.
            // We just need to assert that the outcome is correct.
            EXPECT_TRUE(pylabhub::hub::lifecycle_initialized());
        },
        "messagehub.lifecycle_initialized_follows_state",
        // Provide the modules needed for the test. The LifecycleGuard will handle them.
        // The hub module depends on the logger, so we must provide both.
        pylabhub::hub::GetLifecycleModule(), 
        pylabhub::utils::Logger::GetLifecycleModule());
}

} // namespace pylabhub::tests::worker::messagehub