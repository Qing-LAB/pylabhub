/**
 * @file test_metrics_api.cpp
 * @brief HEP-CORE-0019 API-level metrics unit tests.
 *
 * Tests snapshot_metrics_json, report_metric, report_metrics,
 * clear_custom_metrics on ProducerAPI, ConsumerAPI, ProcessorAPI.
 *
 * Phase 2: all API classes take RoleAPIBase& instead of RoleHostCore&.
 */
#include "producer/producer_api.hpp"
#include "consumer/consumer_api.hpp"
#include "processor/processor_api.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pybind11/embed.h>

#include <string>
#include <unordered_map>

using json = nlohmann::json;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;

namespace
{

struct TestContext
{
    RoleHostCore core;
    std::unique_ptr<RoleAPIBase> base;

    TestContext(const std::string &tag)
    {
        base = std::make_unique<RoleAPIBase>(core, tag, "TEST-" + tag);
        base->set_name("test-" + tag);
        base->set_channel("test.chan");
    }
};

} // anonymous namespace

// ============================================================================
// ProducerAPI metrics
// ============================================================================

TEST(MetricsApiTest, ProducerAPI_SnapshotBase_NoSHM)
{
    TestContext ctx("prod");
    pylabhub::producer::ProducerAPI api(*ctx.base);

    ctx.core.inc_out_slots_written();
    ctx.core.inc_out_slots_written();
    ctx.core.inc_out_drop_count();

    json snap = api.snapshot_metrics_json();

    ASSERT_TRUE(snap.contains("role"));
    EXPECT_EQ(snap["role"]["out_slots_written"], 2);
    EXPECT_EQ(snap["role"]["out_drop_count"], 1);
    EXPECT_EQ(snap["role"]["script_error_count"], 0);

    ASSERT_TRUE(snap.contains("loop"));
    EXPECT_EQ(snap["loop"]["iteration_count"], 0);

    EXPECT_FALSE(snap.contains("queue"));
    EXPECT_FALSE(snap.contains("custom"));
}

TEST(MetricsApiTest, ProducerAPI_ReportMetric)
{
    TestContext ctx("prod");
    pylabhub::producer::ProducerAPI api(*ctx.base);
    api.report_metric("temperature", 23.5);

    json snap = api.snapshot_metrics_json();
    ASSERT_TRUE(snap.contains("custom"));
    EXPECT_DOUBLE_EQ(snap["custom"]["temperature"].get<double>(), 23.5);
}

TEST(MetricsApiTest, ProducerAPI_ReportMetrics_Batch)
{
    TestContext ctx("prod");
    pylabhub::producer::ProducerAPI api(*ctx.base);
    api.report_metrics({{"a", 1.0}, {"b", 2.0}});

    json snap = api.snapshot_metrics_json();
    EXPECT_DOUBLE_EQ(snap["custom"]["a"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(snap["custom"]["b"].get<double>(), 2.0);
}

TEST(MetricsApiTest, ProducerAPI_ClearCustomMetrics)
{
    TestContext ctx("prod");
    pylabhub::producer::ProducerAPI api(*ctx.base);
    api.report_metric("x", 1.0);
    api.clear_custom_metrics();

    json snap = api.snapshot_metrics_json();
    EXPECT_TRUE(snap["custom"].empty());
}

TEST(MetricsApiTest, ProducerAPI_ReportMetric_Overwrite)
{
    TestContext ctx("prod");
    pylabhub::producer::ProducerAPI api(*ctx.base);
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
    TestContext ctx("cons");
    pylabhub::consumer::ConsumerAPI api(*ctx.base);

    json snap = api.snapshot_metrics_json();

    ASSERT_TRUE(snap.contains("role"));
    EXPECT_EQ(snap["role"]["in_slots_received"], 0);
    EXPECT_EQ(snap["role"]["out_slots_written"], 0);
    EXPECT_EQ(snap["role"]["out_drop_count"], 0);
    EXPECT_EQ(snap["role"]["script_error_count"], 0);

    ASSERT_TRUE(snap.contains("loop"));
    EXPECT_EQ(snap["loop"]["iteration_count"], 0);

    EXPECT_FALSE(snap.contains("queue"));
    EXPECT_FALSE(snap.contains("custom"));
}

TEST(MetricsApiTest, ConsumerAPI_ReportAndClear)
{
    TestContext ctx("cons");
    pylabhub::consumer::ConsumerAPI api(*ctx.base);
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
    TestContext ctx("proc");
    pylabhub::processor::ProcessorAPI api(*ctx.base);

    json snap = api.snapshot_metrics_json();

    ASSERT_TRUE(snap.contains("role"));
    EXPECT_EQ(snap["role"]["in_slots_received"], 0);
    EXPECT_EQ(snap["role"]["out_slots_written"], 0);
    EXPECT_EQ(snap["role"]["out_drop_count"], 0);
    EXPECT_EQ(snap["role"]["script_error_count"], 0);

    ASSERT_TRUE(snap.contains("loop"));
    EXPECT_EQ(snap["loop"]["iteration_count"], 0);

    EXPECT_FALSE(snap.contains("in_queue"));
    EXPECT_FALSE(snap.contains("out_queue"));
    EXPECT_FALSE(snap.contains("custom"));
}

TEST(MetricsApiTest, ProcessorAPI_ReportAndSnapshot)
{
    TestContext ctx("proc");
    pylabhub::processor::ProcessorAPI api(*ctx.base);
    api.report_metric("rate", 100.0);

    json snap = api.snapshot_metrics_json();
    ASSERT_TRUE(snap.contains("custom"));
    EXPECT_DOUBLE_EQ(snap["custom"]["rate"].get<double>(), 100.0);
}

// ============================================================================
// PyDict tests (require pybind11 interpreter)
// ============================================================================

class MetricsApiPyDictTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        guard_ = std::make_unique<py::scoped_interpreter>();
    }
    static void TearDownTestSuite()
    {
        guard_.reset();
    }
  private:
    static std::unique_ptr<py::scoped_interpreter> guard_;
};

std::unique_ptr<py::scoped_interpreter> MetricsApiPyDictTest::guard_;

TEST_F(MetricsApiPyDictTest, ProducerAPI_PyDict_Hierarchical_NoQueue)
{
    TestContext ctx("prod");
    pylabhub::producer::ProducerAPI api(*ctx.base);

    ctx.core.inc_out_slots_written();

    py::dict d = api.metrics();

    // Must have "role" and "loop" groups.
    EXPECT_TRUE(d.contains("role"));
    EXPECT_TRUE(d.contains("loop"));
    // No queue when disconnected.
    EXPECT_FALSE(d.contains("queue"));

    auto role = d["role"].cast<py::dict>();
    EXPECT_EQ(role["out_slots_written"].cast<uint64_t>(), 1u);
    EXPECT_EQ(role["script_error_count"].cast<uint64_t>(), 0u);
}

TEST_F(MetricsApiPyDictTest, ConsumerAPI_PyDict_Hierarchical_NoQueue)
{
    TestContext ctx("cons");
    pylabhub::consumer::ConsumerAPI api(*ctx.base);

    py::dict d = api.metrics();

    EXPECT_TRUE(d.contains("role"));
    EXPECT_TRUE(d.contains("loop"));
    EXPECT_FALSE(d.contains("queue"));

    auto role = d["role"].cast<py::dict>();
    EXPECT_EQ(role["in_slots_received"].cast<uint64_t>(), 0u);
}

TEST_F(MetricsApiPyDictTest, ProcessorAPI_PyDict_Hierarchical_NoQueue)
{
    TestContext ctx("proc");
    pylabhub::processor::ProcessorAPI api(*ctx.base);

    py::dict d = api.metrics();

    EXPECT_TRUE(d.contains("role"));
    EXPECT_TRUE(d.contains("loop"));
    EXPECT_FALSE(d.contains("in_queue"));
    EXPECT_FALSE(d.contains("out_queue"));
}
