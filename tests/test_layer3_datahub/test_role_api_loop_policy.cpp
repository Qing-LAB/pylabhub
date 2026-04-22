/**
 * @file test_role_api_loop_policy.cpp
 * @brief L3 role-API integration — ContextMetrics + loop-overrun
 *        (HEP-CORE-0008).
 *
 * Subject: `DataBlockProducer` / `DataBlockConsumer` metric state
 * (`last_iteration_us`, `max_iteration_us`, `last_slot_exec_us`,
 * `context_elapsed_us`, `configured_period_us`) and the
 * `RoleHostCore::loop_overrun_count` counter that replaced the former
 * DataBlock-level overrun tracking.
 *
 * Scope is the queue-layer metrics surface only.  The legacy template
 * RAII loop (`with_transaction<F,D>` + `ctx.slots()` + `SlotIterator`)
 * lives in the sibling file `test_role_api_raii.cpp`.  Splitting the
 * two isolates the Group A metrics tests from the pending RAII-layer
 * rework (`docs/tech_draft/raii_layer_redesign.md` Phases 2-5).
 *
 * ## Retired during Pattern 3 conversion (2026-04-21)
 *
 * Two V1 tests were removed because they ran through setup without
 * ever asserting on the state under test (vacuous passes):
 *   - `ProducerFixedRateOverrunDetect`
 *   - `RaiiProducerOverrunViaSlots`
 * The overrun behaviour they *nominally* tested is now on
 * `RoleHostCore::loop_overrun_count` and is covered by
 * `LoopOverrunCount_IncrementAndAccumulate`.
 *
 * ## Lifecycle pattern
 *
 * Pattern 3: each TEST_F spawns a subprocess that owns its own
 * `LifecycleGuard` via `run_gtest_worker`.  Worker bodies live in
 * `workers/role_api_loop_policy_workers.cpp`.
 */
#include "test_patterns.h"
#include "test_process_utils.h"

#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class LoopPolicyMetricsTest : public IsolatedProcessTest
{
};

TEST_F(LoopPolicyMetricsTest, ProducerMetricsAccumulate)
{
    auto w = SpawnWorker("role_api_loop_policy.producer_metrics_accumulate", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, ProducerMetricsClear)
{
    auto w = SpawnWorker("role_api_loop_policy.producer_metrics_clear", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, LoopOverrunCount_IncrementAndAccumulate)
{
    auto w = SpawnWorker(
        "role_api_loop_policy.loop_overrun_count_increment_and_accumulate", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, ConsumerMetricsAccumulate)
{
    auto w = SpawnWorker("role_api_loop_policy.consumer_metrics_accumulate", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, ZeroOnCreation)
{
    auto w = SpawnWorker("role_api_loop_policy.zero_on_creation", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, MaxRateMetricsPeriodZero)
{
    auto w = SpawnWorker("role_api_loop_policy.max_rate_metrics_period_zero", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, LastSlotWorkUsPopulated)
{
    auto w = SpawnWorker("role_api_loop_policy.last_slot_work_us_populated", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, LastIterationUsPopulated)
{
    auto w = SpawnWorker("role_api_loop_policy.last_iteration_us_populated", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, MaxIterationUsPeak)
{
    auto w = SpawnWorker("role_api_loop_policy.max_iteration_us_peak", {});
    ExpectWorkerOk(w);
}

TEST_F(LoopPolicyMetricsTest, ContextElapsedUsMonotonic)
{
    auto w = SpawnWorker("role_api_loop_policy.context_elapsed_us_monotonic", {});
    ExpectWorkerOk(w);
}
