/**
 * @file test_hub_lua_integration.cpp
 * @brief Pattern 3 driver — HubHost + LuaEngine integration tests
 *        (HEP-CORE-0033 Phase 7 D3.3 + HEP-CORE-0011 §"Engine
 *        Construction Lifecycle").
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/hub_lua_integration_workers.cpp`; each scenario constructs
 * a real HubHost wired to a real on-disk init.lua and (for the
 * event-observer tests) drives wire-protocol REG_REQ traffic through
 * a real BrokerRequestComm client.
 *
 * Pattern 3 fits this file especially well: each subprocess gets a
 * fresh LuaJIT runtime state, removing the in-process re-init
 * fragility that the suite-level guard was working around.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class HubLuaIntegrationTest : public IsolatedProcessTest
{
};

TEST_F(HubLuaIntegrationTest, RealLuaScript_OnInitOnStop_FireAndLog)
{
    auto w = SpawnWorker(
        "hub_lua_integration.real_lua_script_on_init_on_stop_fire_and_log");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, ScriptSyntaxError_StartupThrows)
{
    auto w = SpawnWorker(
        "hub_lua_integration.script_syntax_error_startup_throws");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, OnTick_FiresPeriodically_WhenIdle)
{
    auto w = SpawnWorker(
        "hub_lua_integration.on_tick_fires_periodically_when_idle");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, OnTick_CatchUp_FixedRateWithCompensation)
{
    auto w = SpawnWorker(
        "hub_lua_integration.on_tick_catch_up_fixed_rate_with_compensation");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, ReadAccessors_AllReachable_FromOnInit)
{
    auto w = SpawnWorker(
        "hub_lua_integration.read_accessors_all_reachable_from_on_init");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, RequestShutdown_FromOnInit_WakesMainLoop)
{
    auto w = SpawnWorker(
        "hub_lua_integration.request_shutdown_from_on_init_wakes_main_loop");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest,
       EventObservers_ChannelRegistration_FiresOnChannelOpenedAndOnRoleRegistered)
{
    auto w = SpawnWorker(
        "hub_lua_integration.event_observers_channel_registration_"
        "fires_on_channel_opened_and_on_role_registered");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, EventObserver_ConsumerRegistration_FiresOnConsumerAdded)
{
    auto w = SpawnWorker(
        "hub_lua_integration.event_observer_consumer_registration_"
        "fires_on_consumer_added");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, PostEvent_FromOnInit_FiresOnAppCallback)
{
    auto w = SpawnWorker(
        "hub_lua_integration.post_event_from_on_init_fires_on_app_callback");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest,
       Augment_QueryMetrics_FromAdminThreadCallSite_MutatesResponse)
{
    auto w = SpawnWorker(
        "hub_lua_integration.augment_query_metrics_from_admin_thread_"
        "call_site_mutates_response");
    ExpectWorkerOk(w);
}

TEST_F(HubLuaIntegrationTest, Augment_NullReturn_KeepsDefaultResponse)
{
    auto w = SpawnWorker(
        "hub_lua_integration.augment_null_return_keeps_default_response");
    ExpectWorkerOk(w);
}
