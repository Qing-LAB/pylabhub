/**
 * @file test_hub_host.cpp
 * @brief Pattern 3 driver — HubHost lifecycle tests
 *        (HEP-CORE-0033 §4 / Phase 6.1b).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/hub_host_workers.cpp`; each one transplants the prior
 * TEST_F's body verbatim, preserving the outcome+path+speed pins
 * the original carefully established (the 2026-05-01 lesson on
 * busy-port failure-mode fast vs slow paths).
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class HubHostTest : public IsolatedProcessTest
{
};

TEST_F(HubHostTest, Construct_WithoutStartup_NoThreadSpawn)
{
    auto w = SpawnWorker("hub_host.construct_without_startup_no_thread_spawn");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, BrokerHubState_IsHubHostHubState)
{
    auto w = SpawnWorker("hub_host.broker_hub_state_is_hub_host_hub_state");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, Startup_Idempotent)
{
    auto w = SpawnWorker("hub_host.startup_idempotent");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, Shutdown_Idempotent)
{
    auto w = SpawnWorker("hub_host.shutdown_idempotent");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, RunMainLoop_BlocksUntilRequestShutdown)
{
    auto w = SpawnWorker("hub_host.run_main_loop_blocks_until_request_shutdown");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, Startup_FailsCleanlyOnBusyPort)
{
    auto w = SpawnWorker("hub_host.startup_fails_cleanly_on_busy_port");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, Destructor_CleansUpEvenWithoutExplicitShutdown)
{
    auto w = SpawnWorker("hub_host.destructor_cleans_up_even_without_explicit_shutdown");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, ConfigAccessor_ReturnsLoadedValue)
{
    auto w = SpawnWorker("hub_host.config_accessor_returns_loaded_value");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, StartupAfterShutdown_Throws)
{
    auto w = SpawnWorker("hub_host.startup_after_shutdown_throws");
    ExpectWorkerOk(w);
}

TEST_F(HubHostTest, FailedStartupAllowsRetry)
{
    auto w = SpawnWorker("hub_host.failed_startup_allows_retry");
    ExpectWorkerOk(w);
}
