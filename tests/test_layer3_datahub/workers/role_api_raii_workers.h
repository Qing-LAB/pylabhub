#pragma once
/**
 * @file role_api_raii_workers.h
 * @brief Workers for L3 role-API legacy template-RAII integration tests.
 *
 * Scope is the `DataBlockProducer::with_transaction<F,D>` +
 * `ctx.slots()` + `SlotIterator` surface — the template RAII layer
 * currently *without* any production callers (role hosts use
 * `RoleAPIBase` + `QueueWriter` / `QueueReader`).
 *
 * This layer is pending replacement per
 * `docs/tech_draft/raii_layer_redesign.md` Phases 2-5 (typed
 * `TypedQueueWriter<SlotT>`, new `SlotIterator` over typed queues,
 * `SimpleRoleHost<SlotT>` Level-B wrapper).  When that lands, every
 * test in this file is rewritten against the new surface.  The file
 * exists as a separate translation unit from
 * `role_api_loop_policy_workers.{h,cpp}` so the Group A metrics tests
 * are unaffected by the RAII-layer churn.
 */

namespace pylabhub::tests::worker::role_api_raii
{

int slot_iterator_fixed_rate_pacing();
int ctx_metrics_pass_through();
int raii_producer_last_slot_work_us_multi_iter();
int raii_producer_metrics_via_slots();
int raii_consumer_last_slot_work_us();

} // namespace pylabhub::tests::worker::role_api_raii
