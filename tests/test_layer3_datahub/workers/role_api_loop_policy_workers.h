#pragma once
/**
 * @file role_api_loop_policy_workers.h
 * @brief Workers for L3 role-API ContextMetrics + loop-overrun tests.
 *
 * Each body spawns a real `DataBlockProducer` / `DataBlockConsumer`
 * under a `LifecycleGuard` owned by the worker subprocess via
 * `run_gtest_worker`.
 *
 * Scope is the ContextMetrics surface on the DataBlock queue and the
 * `RoleHostCore::loop_overrun_count` counter.  The legacy template-RAII
 * loop (`with_transaction<F,D>` + `ctx.slots()` + `SlotIterator`) is
 * exercised separately in `role_api_raii_workers.{h,cpp}`.
 *
 * Two tests from the original V1 file were retired during Pattern 3
 * conversion (2026-04-21) because they ran through setup without ever
 * asserting on the state under test (vacuous passes):
 *   - `ProducerFixedRateOverrunDetect`
 *   - `RaiiProducerOverrunViaSlots`
 * The overrun behaviour they *would* have covered now lives on
 * `RoleHostCore::loop_overrun_count` and is exercised by
 * `loop_overrun_count_increment_and_accumulate` here.
 */

namespace pylabhub::tests::worker::role_api_loop_policy
{

int producer_metrics_accumulate();
int producer_metrics_clear();
int loop_overrun_count_increment_and_accumulate();
int consumer_metrics_accumulate();
int zero_on_creation();
int max_rate_metrics_period_zero();
int last_slot_work_us_populated();
int last_iteration_us_populated();
int max_iteration_us_peak();
int context_elapsed_us_monotonic();

} // namespace pylabhub::tests::worker::role_api_loop_policy
