/**
 * @file test_metrics_api.cpp
 * @brief Pattern 3 driver: ProducerAPI/ConsumerAPI/ProcessorAPI metrics tests.
 *
 * Each TEST_F spawns a worker (IsolatedProcessTest::SpawnWorker). Worker
 * bodies live in workers/metrics_api_workers.cpp and own a LifecycleGuard
 * with Logger so RoleAPIBase ctor (which constructs a ThreadManager that
 * registers a dynamic lifecycle module) finds the lifecycle initialised.
 *
 * The previous in-process layout fabricated RoleAPIBase without any
 * guard, half-registering ThreadManager and producing the long-documented
 * MetricsApi teardown flake. See HEP-CORE-0001 § "Testing implications".
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class MetricsApiTest : public IsolatedProcessTest
{
};

// ── ProducerAPI ─────────────────────────────────────────────────────────────

TEST_F(MetricsApiTest, ProducerAPI_SnapshotBase_NoSHM)
{
    auto w = SpawnWorker("metrics_api.producer_snapshot_base_no_shm", {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiTest, ProducerAPI_ReportMetric)
{
    auto w = SpawnWorker("metrics_api.producer_report_metric", {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiTest, ProducerAPI_ReportMetrics_Batch)
{
    auto w = SpawnWorker("metrics_api.producer_report_metrics_batch", {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiTest, ProducerAPI_ClearCustomMetrics)
{
    auto w = SpawnWorker("metrics_api.producer_clear_custom_metrics", {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiTest, ProducerAPI_ReportMetric_Overwrite)
{
    auto w = SpawnWorker("metrics_api.producer_report_metric_overwrite", {});
    ExpectWorkerOk(w);
}

// ── ConsumerAPI ─────────────────────────────────────────────────────────────

TEST_F(MetricsApiTest, ConsumerAPI_SnapshotBase_NoSHM)
{
    auto w = SpawnWorker("metrics_api.consumer_snapshot_base_no_shm", {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiTest, ConsumerAPI_ReportAndClear)
{
    auto w = SpawnWorker("metrics_api.consumer_report_and_clear", {});
    ExpectWorkerOk(w);
}

// ── ProcessorAPI ────────────────────────────────────────────────────────────

TEST_F(MetricsApiTest, ProcessorAPI_SnapshotBase_NoSHM)
{
    auto w = SpawnWorker("metrics_api.processor_snapshot_base_no_shm", {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiTest, ProcessorAPI_ReportAndSnapshot)
{
    auto w = SpawnWorker("metrics_api.processor_report_and_snapshot", {});
    ExpectWorkerOk(w);
}

// ── PyDict ──────────────────────────────────────────────────────────────────

class MetricsApiPyDictTest : public IsolatedProcessTest
{
};

TEST_F(MetricsApiPyDictTest, ProducerAPI_PyDict_Hierarchical_NoQueue)
{
    auto w = SpawnWorker("metrics_api.producer_pydict_hierarchical_no_queue",
                         {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiPyDictTest, ConsumerAPI_PyDict_Hierarchical_NoQueue)
{
    auto w = SpawnWorker("metrics_api.consumer_pydict_hierarchical_no_queue",
                         {});
    ExpectWorkerOk(w);
}

TEST_F(MetricsApiPyDictTest, ProcessorAPI_PyDict_Hierarchical_NoQueue)
{
    auto w = SpawnWorker("metrics_api.processor_pydict_hierarchical_no_queue",
                         {});
    ExpectWorkerOk(w);
}
