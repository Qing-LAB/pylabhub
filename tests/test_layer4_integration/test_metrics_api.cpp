/**
 * @file test_metrics_api.cpp
 * @brief HEP-CORE-0019 API-level metrics unit tests.
 *
 * Suite: MetricsApiTest
 *
 * Tests the C++ API methods (snapshot_metrics_json, report_metric,
 * report_metrics, clear_custom_metrics) on ProducerAPI, ConsumerAPI,
 * and ProcessorAPI — without requiring a live broker or SHM.
 *
 * These API classes embed pybind11 module definitions, so this test
 * binary links against pybind11_embed.
 */
#include "producer/producer_api.hpp"
#include "consumer/consumer_api.hpp"
#include "processor/processor_api.hpp"
#include "utils/role_host_core.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>

using json = nlohmann::json;

// ============================================================================
// ProducerAPI metrics
// ============================================================================

TEST(MetricsApiTest, ProducerAPI_SnapshotBase_NoSHM)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::producer::ProducerAPI api(core);
    api.set_uid("PROD-test");
    api.set_name("test-prod");
    api.set_channel("test.chan");

    core.inc_out_written();
    core.inc_out_written();
    core.inc_drops();

    json snap = api.snapshot_metrics_json();
    ASSERT_TRUE(snap.contains("base"));
    EXPECT_EQ(snap["base"]["out_written"], 2);
    EXPECT_EQ(snap["base"]["drops"], 1);
    EXPECT_EQ(snap["base"]["script_errors"], 0);
    // No SHM → no loop timing fields.
    EXPECT_FALSE(snap["base"].contains("iteration_count"));
    // Custom should be empty.
    ASSERT_TRUE(snap.contains("custom"));
    EXPECT_TRUE(snap["custom"].empty());
}

TEST(MetricsApiTest, ProducerAPI_ReportMetric)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::producer::ProducerAPI api(core);
    api.report_metric("events", 42.0);
    api.report_metric("rate_hz", 1000.5);

    json snap = api.snapshot_metrics_json();
    ASSERT_TRUE(snap["custom"].contains("events"));
    EXPECT_DOUBLE_EQ(snap["custom"]["events"].get<double>(), 42.0);
    EXPECT_DOUBLE_EQ(snap["custom"]["rate_hz"].get<double>(), 1000.5);
}

TEST(MetricsApiTest, ProducerAPI_ReportMetrics_Batch)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::producer::ProducerAPI api(core);
    std::unordered_map<std::string, double> kv{{"a", 1.0}, {"b", 2.0}, {"c", 3.0}};
    api.report_metrics(kv);

    json snap = api.snapshot_metrics_json();
    EXPECT_DOUBLE_EQ(snap["custom"]["a"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(snap["custom"]["b"].get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(snap["custom"]["c"].get<double>(), 3.0);
}

TEST(MetricsApiTest, ProducerAPI_ClearCustomMetrics)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::producer::ProducerAPI api(core);
    api.report_metric("x", 1.0);
    api.clear_custom_metrics();

    json snap = api.snapshot_metrics_json();
    EXPECT_TRUE(snap["custom"].empty());
}

TEST(MetricsApiTest, ProducerAPI_ReportMetric_Overwrite)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::producer::ProducerAPI api(core);
    api.report_metric("x", 1.0);
    api.report_metric("x", 99.0);

    json snap = api.snapshot_metrics_json();
    EXPECT_DOUBLE_EQ(snap["custom"]["x"].get<double>(), 99.0);
}

// ============================================================================
// ConsumerAPI metrics
// ============================================================================

TEST(MetricsApiTest, ConsumerAPI_SnapshotBase_NoSHM)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::consumer::ConsumerAPI api(core);
    api.set_uid("CONS-test");
    api.set_name("test-cons");
    api.set_channel("test.chan");

    json snap = api.snapshot_metrics_json();
    ASSERT_TRUE(snap.contains("base"));
    EXPECT_EQ(snap["base"]["in_received"], 0);
    EXPECT_EQ(snap["base"]["script_errors"], 0);
    EXPECT_FALSE(snap["base"].contains("out_written"));
    EXPECT_TRUE(snap["custom"].empty());
}

TEST(MetricsApiTest, ConsumerAPI_ReportAndClear)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::consumer::ConsumerAPI api(core);
    api.report_metric("bytes_logged", 2048.0);

    json snap = api.snapshot_metrics_json();
    EXPECT_DOUBLE_EQ(snap["custom"]["bytes_logged"].get<double>(), 2048.0);

    api.clear_custom_metrics();
    snap = api.snapshot_metrics_json();
    EXPECT_TRUE(snap["custom"].empty());
}

// ============================================================================
// ProcessorAPI metrics
// ============================================================================

TEST(MetricsApiTest, ProcessorAPI_SnapshotBase_NoSHM)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::processor::ProcessorAPI api(core);
    api.set_uid("PROC-test");
    api.set_name("test-proc");

    json snap = api.snapshot_metrics_json();
    ASSERT_TRUE(snap.contains("base"));
    EXPECT_EQ(snap["base"]["in_received"], 0);
    EXPECT_EQ(snap["base"]["out_written"], 0);
    EXPECT_EQ(snap["base"]["drops"], 0);
    EXPECT_EQ(snap["base"]["script_errors"], 0);
    EXPECT_TRUE(snap["custom"].empty());
}

TEST(MetricsApiTest, ProcessorAPI_ReportAndSnapshot)
{
    pylabhub::scripting::RoleHostCore core;
    pylabhub::processor::ProcessorAPI api(core);
    api.report_metric("avg_latency_ms", 2.5);
    api.report_metrics({{"throughput", 500.0}, {"errors", 0.0}});

    json snap = api.snapshot_metrics_json();
    EXPECT_DOUBLE_EQ(snap["custom"]["avg_latency_ms"].get<double>(), 2.5);
    EXPECT_DOUBLE_EQ(snap["custom"]["throughput"].get<double>(), 500.0);
    EXPECT_DOUBLE_EQ(snap["custom"]["errors"].get<double>(), 0.0);
}
