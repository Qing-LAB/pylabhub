#pragma once
/**
 * @file metrics_api_workers.h
 * @brief Workers for ProducerAPI/ConsumerAPI/ProcessorAPI metric tests.
 *
 * RoleAPIBase ctor creates a ThreadManager which registers a dynamic
 * module against LifecycleManager. Without an initialized lifecycle the
 * registration silently leaves half-state (the documented MetricsApi
 * flake), so every test runs in a subprocess whose run_gtest_worker
 * owns a LifecycleGuard with Logger.
 *
 * The PyDict tests additionally need a py::scoped_interpreter; the
 * worker creates one inside its body. The interpreter is destroyed
 * before LifecycleGuard finalize, which is the natural order.
 */

namespace pylabhub::tests::worker
{
namespace metrics_api
{

// ── ProducerAPI metrics ─────────────────────────────────────────────────────
int producer_snapshot_base_no_shm();
int producer_report_metric();
int producer_report_metrics_batch();
int producer_clear_custom_metrics();
int producer_report_metric_overwrite();

// ── ConsumerAPI metrics ─────────────────────────────────────────────────────
int consumer_snapshot_base_no_shm();
int consumer_report_and_clear();

// ── ProcessorAPI metrics ────────────────────────────────────────────────────
int processor_snapshot_base_no_shm();
int processor_report_and_snapshot();

// ── PyDict (require py::scoped_interpreter inside the worker) ───────────────
int producer_pydict_hierarchical_no_queue();
int consumer_pydict_hierarchical_no_queue();
int processor_pydict_hierarchical_no_queue();

} // namespace metrics_api
} // namespace pylabhub::tests::worker
