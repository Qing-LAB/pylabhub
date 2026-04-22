/**
 * @file test_role_api_raii.cpp
 * @brief L3 role-API integration — legacy template-RAII data loop
 *        (HEP-CORE-0008).
 *
 * Subject: `DataBlockProducer::with_transaction<F,D>` + `ctx.slots()` +
 * `SlotIterator` + `SlotRef` — the template RAII layer.
 *
 * ## ⚠ Legacy template RAII — pending rework
 *
 * This layer has **no production callers today** — role hosts use
 * `RoleAPIBase` + `QueueWriter` / `QueueReader` instead.  The template
 * layer is scheduled for replacement per
 * `docs/tech_draft/raii_layer_redesign.md` Phases 2-5 (typed
 * `TypedQueueWriter<SlotT>`, new `SlotIterator` over typed queues,
 * `SimpleRoleHost<SlotT>` Level-B wrapper).  When that lands, every
 * test in this file is rewritten against the new surface.
 *
 * Kept as a separate translation unit from
 * `test_role_api_loop_policy.cpp` so the Group A metrics tests remain
 * unaffected by the RAII-layer churn.
 *
 * ## Lifecycle pattern
 *
 * Pattern 3: each TEST_F spawns a subprocess that owns its own
 * `LifecycleGuard` via `run_gtest_worker`.  Worker bodies live in
 * `workers/role_api_raii_workers.cpp`.
 */
#include "test_patterns.h"
#include "test_process_utils.h"

#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class RoleApiRaiiTest : public IsolatedProcessTest
{
};

TEST_F(RoleApiRaiiTest, SlotIteratorFixedRatePacing)
{
    auto w = SpawnWorker("role_api_raii.slot_iterator_fixed_rate_pacing", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiRaiiTest, CtxMetricsPassThrough)
{
    auto w = SpawnWorker("role_api_raii.ctx_metrics_pass_through", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiRaiiTest, RaiiProducerLastSlotWorkUsMultiIter)
{
    auto w = SpawnWorker(
        "role_api_raii.raii_producer_last_slot_work_us_multi_iter", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiRaiiTest, RaiiProducerMetricsViaSlots)
{
    auto w = SpawnWorker("role_api_raii.raii_producer_metrics_via_slots", {});
    ExpectWorkerOk(w);
}

TEST_F(RoleApiRaiiTest, RaiiConsumerLastSlotWorkUs)
{
    auto w = SpawnWorker("role_api_raii.raii_consumer_last_slot_work_us", {});
    ExpectWorkerOk(w);
}
