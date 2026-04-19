/**
 * @file metrics_api_workers.cpp
 * @brief Worker bodies for the metrics-API test suite (Pattern 3).
 *
 * Why a worker subprocess
 * -----------------------
 * RoleAPIBase ctor instantiates a ThreadManager which calls
 * LifecycleManager::register_dynamic_module — that requires the lifecycle
 * to be initialized. Building a RoleAPIBase in the gtest runner without
 * a guard half-registers the module and produces the long-documented
 * MetricsApi teardown flake. Each worker here owns a LifecycleGuard with
 * Logger, so RoleAPIBase ctor finds the lifecycle initialized and
 * shutdown drives ThreadManager finalize cleanly.
 *
 * What _exit() does and does not skip
 * -----------------------------------
 * run_gtest_worker runs the body, then LifecycleGuard's destructor
 * (which invokes every registered module's shutdown — including any
 * dynamic modules ThreadManager registered), then _exit. So our
 * finalize callbacks are exercised exactly as in production. _exit only
 * skips libzmq/libsodium/luajit static destructors that nobody
 * registered with LifecycleManager. See HEP-CORE-0001 § "Testing
 * implications".
 */
#include "metrics_api_workers.h"

#include "consumer/consumer_api.hpp"
#include "processor/processor_api.hpp"
#include "producer/producer_api.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <fmt/core.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pybind11/embed.h>

#include <memory>
#include <string>

using json = nlohmann::json;
namespace py = pybind11;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace metrics_api
{
namespace
{

/// Test fixture analogue: paired RoleHostCore + RoleAPIBase.
struct TestContext
{
    RoleHostCore                 core;
    std::unique_ptr<RoleAPIBase> base;

    explicit TestContext(const std::string &tag)
    {
        base = std::make_unique<RoleAPIBase>(core, tag, "TEST-" + tag);
        base->set_name("test-" + tag);
        base->set_channel("test.chan");
    }
};

} // namespace

// ── ProducerAPI metrics ─────────────────────────────────────────────────────

int producer_snapshot_base_no_shm()
{
    return run_gtest_worker(
        [&]()
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
            EXPECT_EQ(snap["loop"]["acquire_retry_count"], 0);

            EXPECT_FALSE(snap.contains("queue"));
            EXPECT_FALSE(snap.contains("custom"));
        },
        "metrics_api::producer_snapshot_base_no_shm",
        Logger::GetLifecycleModule());
}

int producer_report_metric()
{
    return run_gtest_worker(
        [&]()
        {
            TestContext ctx("prod");
            pylabhub::producer::ProducerAPI api(*ctx.base);
            api.report_metric("temperature", 23.5);

            json snap = api.snapshot_metrics_json();
            ASSERT_TRUE(snap.contains("custom"));
            EXPECT_DOUBLE_EQ(snap["custom"]["temperature"].get<double>(), 23.5);
        },
        "metrics_api::producer_report_metric",
        Logger::GetLifecycleModule());
}

int producer_report_metrics_batch()
{
    return run_gtest_worker(
        [&]()
        {
            TestContext ctx("prod");
            pylabhub::producer::ProducerAPI api(*ctx.base);
            api.report_metrics({{"a", 1.0}, {"b", 2.0}});

            json snap = api.snapshot_metrics_json();
            EXPECT_DOUBLE_EQ(snap["custom"]["a"].get<double>(), 1.0);
            EXPECT_DOUBLE_EQ(snap["custom"]["b"].get<double>(), 2.0);
        },
        "metrics_api::producer_report_metrics_batch",
        Logger::GetLifecycleModule());
}

int producer_clear_custom_metrics()
{
    return run_gtest_worker(
        [&]()
        {
            TestContext ctx("prod");
            pylabhub::producer::ProducerAPI api(*ctx.base);
            api.report_metric("x", 1.0);
            api.clear_custom_metrics();

            json snap = api.snapshot_metrics_json();
            EXPECT_TRUE(snap["custom"].empty());
        },
        "metrics_api::producer_clear_custom_metrics",
        Logger::GetLifecycleModule());
}

int producer_report_metric_overwrite()
{
    return run_gtest_worker(
        [&]()
        {
            TestContext ctx("prod");
            pylabhub::producer::ProducerAPI api(*ctx.base);
            api.report_metric("x", 1.0);
            api.report_metric("x", 99.0);

            json snap = api.snapshot_metrics_json();
            EXPECT_DOUBLE_EQ(snap["custom"]["x"].get<double>(), 99.0);
        },
        "metrics_api::producer_report_metric_overwrite",
        Logger::GetLifecycleModule());
}

// ── ConsumerAPI metrics ─────────────────────────────────────────────────────

int consumer_snapshot_base_no_shm()
{
    return run_gtest_worker(
        [&]()
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
            EXPECT_EQ(snap["loop"]["acquire_retry_count"], 0);

            EXPECT_FALSE(snap.contains("queue"));
            EXPECT_FALSE(snap.contains("custom"));
        },
        "metrics_api::consumer_snapshot_base_no_shm",
        Logger::GetLifecycleModule());
}

int consumer_report_and_clear()
{
    return run_gtest_worker(
        [&]()
        {
            TestContext ctx("cons");
            pylabhub::consumer::ConsumerAPI api(*ctx.base);
            api.report_metric("bytes_logged", 2048.0);

            json snap = api.snapshot_metrics_json();
            EXPECT_DOUBLE_EQ(snap["custom"]["bytes_logged"].get<double>(), 2048.0);

            api.clear_custom_metrics();
            snap = api.snapshot_metrics_json();
            EXPECT_TRUE(snap["custom"].empty());
        },
        "metrics_api::consumer_report_and_clear",
        Logger::GetLifecycleModule());
}

// ── ProcessorAPI metrics ────────────────────────────────────────────────────

int processor_snapshot_base_no_shm()
{
    return run_gtest_worker(
        [&]()
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
            EXPECT_EQ(snap["loop"]["acquire_retry_count"], 0);

            EXPECT_FALSE(snap.contains("in_queue"));
            EXPECT_FALSE(snap.contains("out_queue"));
            EXPECT_FALSE(snap.contains("custom"));
        },
        "metrics_api::processor_snapshot_base_no_shm",
        Logger::GetLifecycleModule());
}

int processor_report_and_snapshot()
{
    return run_gtest_worker(
        [&]()
        {
            TestContext ctx("proc");
            pylabhub::processor::ProcessorAPI api(*ctx.base);
            api.report_metric("rate", 100.0);

            json snap = api.snapshot_metrics_json();
            ASSERT_TRUE(snap.contains("custom"));
            EXPECT_DOUBLE_EQ(snap["custom"]["rate"].get<double>(), 100.0);
        },
        "metrics_api::processor_report_and_snapshot",
        Logger::GetLifecycleModule());
}

// ── PyDict workers ──────────────────────────────────────────────────────────
//
// Each PyDict worker owns its own py::scoped_interpreter; the interpreter
// is destroyed before LifecycleGuard finalize (RAII order: declared
// after guard inside run_gtest_worker's lambda → destroyed first), which
// keeps Python teardown ahead of Logger shutdown.

int producer_pydict_hierarchical_no_queue()
{
    return run_gtest_worker(
        [&]()
        {
            py::scoped_interpreter interp;
            TestContext            ctx("prod");
            pylabhub::producer::ProducerAPI api(*ctx.base);

            ctx.core.inc_out_slots_written();

            py::dict d = api.metrics();

            EXPECT_TRUE(d.contains("role"));
            EXPECT_TRUE(d.contains("loop"));
            EXPECT_FALSE(d.contains("queue"));

            auto role = d["role"].cast<py::dict>();
            EXPECT_EQ(role["out_slots_written"].cast<uint64_t>(), 1u);
            EXPECT_EQ(role["script_error_count"].cast<uint64_t>(), 0u);
        },
        "metrics_api::producer_pydict_hierarchical_no_queue",
        Logger::GetLifecycleModule());
}

int consumer_pydict_hierarchical_no_queue()
{
    return run_gtest_worker(
        [&]()
        {
            py::scoped_interpreter interp;
            TestContext            ctx("cons");
            pylabhub::consumer::ConsumerAPI api(*ctx.base);

            py::dict d = api.metrics();

            EXPECT_TRUE(d.contains("role"));
            EXPECT_TRUE(d.contains("loop"));
            EXPECT_FALSE(d.contains("queue"));

            auto role = d["role"].cast<py::dict>();
            EXPECT_EQ(role["in_slots_received"].cast<uint64_t>(), 0u);
        },
        "metrics_api::consumer_pydict_hierarchical_no_queue",
        Logger::GetLifecycleModule());
}

int processor_pydict_hierarchical_no_queue()
{
    return run_gtest_worker(
        [&]()
        {
            py::scoped_interpreter interp;
            TestContext            ctx("proc");
            pylabhub::processor::ProcessorAPI api(*ctx.base);

            py::dict d = api.metrics();

            EXPECT_TRUE(d.contains("role"));
            EXPECT_TRUE(d.contains("loop"));
            EXPECT_FALSE(d.contains("in_queue"));
            EXPECT_FALSE(d.contains("out_queue"));
        },
        "metrics_api::processor_pydict_hierarchical_no_queue",
        Logger::GetLifecycleModule());
}

} // namespace metrics_api
} // namespace pylabhub::tests::worker

namespace
{

struct MetricsApiWorkerRegistrar
{
    MetricsApiWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "metrics_api")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::metrics_api;

                if (sc == "producer_snapshot_base_no_shm")
                    return producer_snapshot_base_no_shm();
                if (sc == "producer_report_metric")
                    return producer_report_metric();
                if (sc == "producer_report_metrics_batch")
                    return producer_report_metrics_batch();
                if (sc == "producer_clear_custom_metrics")
                    return producer_clear_custom_metrics();
                if (sc == "producer_report_metric_overwrite")
                    return producer_report_metric_overwrite();
                if (sc == "consumer_snapshot_base_no_shm")
                    return consumer_snapshot_base_no_shm();
                if (sc == "consumer_report_and_clear")
                    return consumer_report_and_clear();
                if (sc == "processor_snapshot_base_no_shm")
                    return processor_snapshot_base_no_shm();
                if (sc == "processor_report_and_snapshot")
                    return processor_report_and_snapshot();
                if (sc == "producer_pydict_hierarchical_no_queue")
                    return producer_pydict_hierarchical_no_queue();
                if (sc == "consumer_pydict_hierarchical_no_queue")
                    return consumer_pydict_hierarchical_no_queue();
                if (sc == "processor_pydict_hierarchical_no_queue")
                    return processor_pydict_hierarchical_no_queue();

                fmt::print(stderr,
                           "[metrics_api] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static MetricsApiWorkerRegistrar g_metrics_api_registrar;

} // namespace
