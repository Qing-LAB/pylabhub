/**
 * @file test_pattern4_metrics.cpp
 * @brief Pattern 4 broker metrics-storage wire tests (HEP-CORE-0019 §2.3).
 *
 * Successors of the storage-oriented workers from
 * `tests/test_layer3_datahub/workers/datahub_metrics_workers.cpp` (task
 * #52 sweep).  Heartbeats carry a metrics object; the broker stores it on
 * the per-(uid, role_type) presence row and now logs it
 * (event=HeartbeatMetricsStored), so the parent verifies the wire→storage
 * path by reading the broker subprocess's trace instead of the in-process
 * svc.query_metrics.
 *
 * NOT migrated here — the query-engine tests (query_metrics filter /
 * category / echo semantics) exercise the query API's output, which has no
 * storage trace to read; their disposition (L2 against the query engine,
 * or a METRICS_REQ wire query) is tracked separately.
 * multi_presence_end_to_end_no_cross_attribution is also deferred (intricate
 * two-channel cross-attribution).
 */
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4MetricsTest : public pylabhub::tests::pattern4::Pattern4WireTest
{
protected:
    // Fire-and-forget HEARTBEAT_NOTIFY carrying a metrics object.
    void heartbeat_with_metrics(BrokerWireClient     &c,
                                const std::string    &channel,
                                const std::string    &uid,
                                const std::string    &role_type,
                                const nlohmann::json &metrics)
    {
        nlohmann::json hb;
        hb["channel_name"] = channel;
        hb["role_uid"]     = uid;
        hb["role_type"]    = role_type;
        hb["producer_pid"] = pylabhub::platform::get_pid();
        hb["metrics"]      = metrics;
        c.send("HEARTBEAT_NOTIFY", hb);
    }
};

}  // namespace

#define SPAWN_BROKER(temp_dir)                                                  \
    SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",               \
                              {(temp_dir).string(), "default"})

TEST_F(Pattern4MetricsTest, HeartbeatMetricsStoredByBroker)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "metrics.heartbeat.stored" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("metrics_hb_stored");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    heartbeat_with_metrics(prod, channel, uid, "producer",
                            {{"iteration_count", 42}, {"avg_period_us", 1000}});

    // The broker's stored-metrics trace echoes the producer's metrics.
    expect_log(broker,
                "event=HeartbeatMetricsStored channel='" + channel +
                    "' role_uid='" + uid + "' role_type='producer'",
                milliseconds{pylabhub::kMidTimeoutMs});
    expect_log(broker, "\"iteration_count\":42",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

TEST_F(Pattern4MetricsTest, ConsumerHeartbeatMetricsStoredByBroker)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "metrics.consumer.stored" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("metrics_cons_stored");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));
    heartbeat_with_metrics(cons, channel, cons_uid, "consumer",
                            {{"read_count", 100}});

    expect_log(broker,
                "event=HeartbeatMetricsStored channel='" + channel +
                    "' role_uid='" + cons_uid + "' role_type='consumer'",
                milliseconds{pylabhub::kMidTimeoutMs});
    expect_log(broker, "\"read_count\":100",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

TEST_F(Pattern4MetricsTest, MetricsUpdateOverwriteOnHeartbeat)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "metrics.overwrite" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("metrics_overwrite");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));

    heartbeat_with_metrics(prod, channel, uid, "producer",
                            {{"iteration_count", 10}});
    expect_log(broker, "\"iteration_count\":10",
                milliseconds{pylabhub::kMidTimeoutMs});

    heartbeat_with_metrics(prod, channel, uid, "producer",
                            {{"iteration_count", 20}});
    // The second heartbeat stores the overwriting value.
    expect_log(broker, "\"iteration_count\":20",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

TEST_F(Pattern4MetricsTest, ProducerPidInStoredMetrics)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "metrics.pid" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("metrics_pid");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    heartbeat_with_metrics(prod, channel, uid, "producer", {{"test", 1}});

    // The stored-metrics trace attributes the producer's pid.
    expect_log(broker,
                "role_uid='" + uid + "' role_type='producer' producer_pid=" +
                    std::to_string(pylabhub::platform::get_pid()),
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

TEST_F(Pattern4MetricsTest, FanInTwoProducersMetricsDoNotOverwrite)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "metrics.fanin" + suffix;
    const std::string uid_a   = "prod.a." + channel;
    const std::string uid_b   = "prod.b." + channel;

    const fs::path temp_dir = make_test_temp_dir("metrics_fanin");
    const auto     setup    = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           a = make_wire_client(ctx, setup, uid_a);
    ASSERT_NO_FATAL_FAILURE(
        register_producer(a, setup, channel, uid_a, nullptr, "fan-in"));
    auto b = make_wire_client(ctx, setup, uid_b);
    ASSERT_NO_FATAL_FAILURE(
        register_producer(b, setup, channel, uid_b, nullptr, "fan-in"));

    heartbeat_with_metrics(a, channel, uid_a, "producer",
                            {{"iteration_count", 100}});
    heartbeat_with_metrics(b, channel, uid_b, "producer",
                            {{"iteration_count", 200}});

    // Each producer's metrics are stored under its own uid — no overwrite.
    expect_log(broker,
                "role_uid='" + uid_a + "' role_type='producer' producer_pid=" +
                    std::to_string(pylabhub::platform::get_pid()) +
                    " metrics={\"iteration_count\":100}",
                milliseconds{pylabhub::kMidTimeoutMs});
    expect_log(broker,
                "role_uid='" + uid_b + "' role_type='producer' producer_pid=" +
                    std::to_string(pylabhub::platform::get_pid()) +
                    " metrics={\"iteration_count\":200}",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

TEST_F(Pattern4MetricsTest, HeartbeatNoMetricsBackwardCompat)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "metrics.no_metrics" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("metrics_none");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    // A metrics-less heartbeat is accepted (backward compat): the broker
    // logs the first-heartbeat trace and does NOT emit a metrics-stored
    // trace (no metrics object on the wire).
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));
    expect_log(broker,
                "first heartbeat received from role='" + uid + "'",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

// METRICS_REPORT_REQ is retired (M1.4) — the broker replies with an ERROR
// envelope (UNKNOWN_MSG_TYPE) and logs a WARN.
TEST_F(Pattern4MetricsTest, OldMetricsReportReqGetsUnknownMsgType)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid    = "prod.metrics.legacy" + suffix;

    const fs::path temp_dir = make_test_temp_dir("metrics_legacy");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           c = make_wire_client(ctx, setup, uid);
    nlohmann::json payload;
    payload["channel_name"] = "any" + suffix;
    // request() returns on the broker's ERROR frame (no METRICS_REPORT_ACK
    // exists); the reply is the error envelope.
    auto reply = c.request("METRICS_REPORT_REQ", payload, "METRICS_REPORT_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value()) << "broker did not reply to METRICS_REPORT_REQ";
    EXPECT_EQ(reply->value("status", std::string{}), "error");
    EXPECT_EQ(reply->value("error_code", std::string{}), "UNKNOWN_MSG_TYPE")
        << "body=" << reply->dump();
    expect_log(broker, "unknown msg_type 'METRICS_REPORT_REQ'",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}
