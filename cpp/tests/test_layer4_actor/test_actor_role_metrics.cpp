/**
 * @file test_actor_role_metrics.cpp
 * @brief Layer 4 tests for ActorRoleAPI metric and diagnostic methods.
 *
 * Tests cover the RoleMetrics supervised-diagnostics API:
 *   - Initial state: all counters zero; slot_valid defaults to true
 *   - increment_script_errors() / script_error_count()
 *   - increment_loop_overruns() / loop_overrun_count()
 *   - set_last_cycle_work_us() / last_cycle_work_us()
 *   - reset_all_role_run_metrics(): resets ALL three counters
 *   - Identity setters and getters (role_name, uid, env fields)
 *   - Instance independence (two ActorRoleAPI objects do not share state)
 *
 * All tested methods are inline in actor_api.hpp.  The Python interpreter is
 * never initialised; producer_ and consumer_ pointers remain null — the tested
 * methods do not dereference either.
 */

#include "actor_api.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <string>

using pylabhub::actor::ActorRoleAPI;

// ============================================================================
// Initial state
// ============================================================================

TEST(ActorRoleMetrics, InitialCountersAreZero)
{
    ActorRoleAPI api;
    EXPECT_EQ(api.script_error_count(), uint64_t{0});
    EXPECT_EQ(api.loop_overrun_count(), uint64_t{0});
    EXPECT_EQ(api.last_cycle_work_us(), uint64_t{0});
}

TEST(ActorRoleMetrics, InitialSlotValidIsTrue)
{
    ActorRoleAPI api;
    EXPECT_TRUE(api.slot_valid());
}

// ============================================================================
// script_error_count
// ============================================================================

TEST(ActorRoleMetrics, IncrementScriptErrorsOnce)
{
    ActorRoleAPI api;
    api.increment_script_errors();
    EXPECT_EQ(api.script_error_count(), uint64_t{1});
}

TEST(ActorRoleMetrics, IncrementScriptErrorsTwice)
{
    ActorRoleAPI api;
    api.increment_script_errors();
    api.increment_script_errors();
    EXPECT_EQ(api.script_error_count(), uint64_t{2});
}

TEST(ActorRoleMetrics, MultipleScriptErrors)
{
    ActorRoleAPI api;
    for (int i = 0; i < 10; ++i)
        api.increment_script_errors();
    EXPECT_EQ(api.script_error_count(), uint64_t{10});
}

// ============================================================================
// loop_overrun_count
// ============================================================================

TEST(ActorRoleMetrics, IncrementLoopOverrunsOnce)
{
    ActorRoleAPI api;
    api.increment_loop_overruns();
    EXPECT_EQ(api.loop_overrun_count(), uint64_t{1});
}

TEST(ActorRoleMetrics, MultipleLoopOverruns)
{
    ActorRoleAPI api;
    api.increment_loop_overruns();
    api.increment_loop_overruns();
    api.increment_loop_overruns();
    EXPECT_EQ(api.loop_overrun_count(), uint64_t{3});
}

// ============================================================================
// last_cycle_work_us
// ============================================================================

TEST(ActorRoleMetrics, SetLastCycleWorkUs)
{
    ActorRoleAPI api;
    api.set_last_cycle_work_us(12345u);
    EXPECT_EQ(api.last_cycle_work_us(), uint64_t{12345});
}

TEST(ActorRoleMetrics, SetLastCycleWorkUsOverwrites)
{
    ActorRoleAPI api;
    api.set_last_cycle_work_us(100u);
    api.set_last_cycle_work_us(200u);
    EXPECT_EQ(api.last_cycle_work_us(), uint64_t{200});
}

TEST(ActorRoleMetrics, SetLastCycleWorkUsZero)
{
    ActorRoleAPI api;
    api.set_last_cycle_work_us(9999u);
    api.set_last_cycle_work_us(0u);
    EXPECT_EQ(api.last_cycle_work_us(), uint64_t{0});
}

// ============================================================================
// reset_all_role_run_metrics — resets ALL three counters
// ============================================================================

TEST(ActorRoleMetrics, ResetClearsAllCounters)
{
    ActorRoleAPI api;
    api.increment_script_errors();
    api.increment_script_errors();
    api.increment_loop_overruns();
    api.set_last_cycle_work_us(9999u);

    // Pre-reset: counters are non-zero.
    EXPECT_EQ(api.script_error_count(), uint64_t{2});
    EXPECT_EQ(api.loop_overrun_count(), uint64_t{1});
    EXPECT_EQ(api.last_cycle_work_us(), uint64_t{9999});

    api.reset_all_role_run_metrics();

    // Post-reset: all back to zero.
    EXPECT_EQ(api.script_error_count(), uint64_t{0});
    EXPECT_EQ(api.loop_overrun_count(), uint64_t{0});
    EXPECT_EQ(api.last_cycle_work_us(), uint64_t{0});
}

TEST(ActorRoleMetrics, ResetOnlyAffectsMetrics)
{
    // reset_all_role_run_metrics resets counters but must not affect identity fields.
    ActorRoleAPI api;
    api.set_role_name("raw_out");
    api.set_actor_uid("ACTOR-TEST-AABBCCDD");
    api.increment_script_errors();

    api.reset_all_role_run_metrics();

    EXPECT_EQ(api.script_error_count(), uint64_t{0});
    EXPECT_EQ(api.role_name(), "raw_out");
    EXPECT_EQ(api.uid(),       "ACTOR-TEST-AABBCCDD");
}

TEST(ActorRoleMetrics, ResetDoesNotBlockFurtherAccumulation)
{
    ActorRoleAPI api;
    api.increment_script_errors();
    api.reset_all_role_run_metrics();

    // Accumulation after reset must work normally.
    api.increment_script_errors();
    api.increment_script_errors();
    EXPECT_EQ(api.script_error_count(), uint64_t{2});
    EXPECT_EQ(api.loop_overrun_count(), uint64_t{0});
}

TEST(ActorRoleMetrics, ResetOnFreshApiIsNoop)
{
    // Resetting a freshly constructed API must not crash or produce unexpected values.
    ActorRoleAPI api;
    api.reset_all_role_run_metrics();
    EXPECT_EQ(api.script_error_count(), uint64_t{0});
    EXPECT_EQ(api.loop_overrun_count(), uint64_t{0});
    EXPECT_EQ(api.last_cycle_work_us(), uint64_t{0});
}

// ============================================================================
// slot_valid flag
// ============================================================================

TEST(ActorRoleMetrics, SetSlotValidFalse)
{
    ActorRoleAPI api;
    api.set_slot_valid(false);
    EXPECT_FALSE(api.slot_valid());
}

TEST(ActorRoleMetrics, SetSlotValidRoundTrip)
{
    ActorRoleAPI api;
    api.set_slot_valid(false);
    EXPECT_FALSE(api.slot_valid());
    api.set_slot_valid(true);
    EXPECT_TRUE(api.slot_valid());
}

// ============================================================================
// Identity fields (role_name, uid, PylabhubEnv getters)
// ============================================================================

TEST(ActorRoleMetrics, RoleNameRoundTrip)
{
    ActorRoleAPI api;
    api.set_role_name("raw_out");
    EXPECT_EQ(api.role_name(), "raw_out");
}

TEST(ActorRoleMetrics, ActorUidRoundTrip)
{
    ActorRoleAPI api;
    api.set_actor_uid("ACTOR-TEST-AABBCCDD");
    EXPECT_EQ(api.uid(), "ACTOR-TEST-AABBCCDD");
}

TEST(ActorRoleMetrics, EnvFieldsRoundTrip)
{
    ActorRoleAPI api;
    api.set_actor_name("MySensor");
    api.set_channel("lab.sensor.temp");
    api.set_broker("tcp://10.0.0.1:5570");
    api.set_kind_str("producer");
    api.set_log_level("debug");
    api.set_script_dir("/opt/scripts");

    EXPECT_EQ(api.actor_name(), "MySensor");
    EXPECT_EQ(api.channel(),    "lab.sensor.temp");
    EXPECT_EQ(api.broker(),     "tcp://10.0.0.1:5570");
    EXPECT_EQ(api.kind(),       "producer");
    EXPECT_EQ(api.log_level(),  "debug");
    EXPECT_EQ(api.script_dir(), "/opt/scripts");
}

// ============================================================================
// Instance independence — two ActorRoleAPI objects must not share state
// ============================================================================

TEST(ActorRoleMetrics, InstancesAreIndependent)
{
    ActorRoleAPI api1;
    ActorRoleAPI api2;

    api1.increment_script_errors();
    api1.increment_script_errors();
    api2.increment_loop_overruns();

    EXPECT_EQ(api1.script_error_count(), uint64_t{2});
    EXPECT_EQ(api1.loop_overrun_count(), uint64_t{0});
    EXPECT_EQ(api2.script_error_count(), uint64_t{0});
    EXPECT_EQ(api2.loop_overrun_count(), uint64_t{1});
}

TEST(ActorRoleMetrics, ResetOnOneInstanceDoesNotAffectOther)
{
    ActorRoleAPI api1;
    ActorRoleAPI api2;

    api1.increment_script_errors();
    api2.increment_script_errors();
    api2.increment_script_errors();

    api1.reset_all_role_run_metrics();

    EXPECT_EQ(api1.script_error_count(), uint64_t{0});
    EXPECT_EQ(api2.script_error_count(), uint64_t{2});
}
