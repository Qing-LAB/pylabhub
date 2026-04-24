/**
 * @file test_datahub_metrics.cpp
 * @brief Broker metrics plane tests (HEP-CORE-0019).
 *
 * Suite: MetricsPlaneTest
 *
 * Tests metrics storage and retrieval through the broker:
 *   - Heartbeat with metrics payload → stored, queryable
 *   - METRICS_REPORT_REQ from consumer → stored
 *   - Query: single channel, all channels, unknown channel
 *   - Metrics overwrite on subsequent heartbeats
 *   - Producer PID in query result
 *
 * All tests use in-process LocalBrokerHandle + BrcHandle pattern.
 * Metrics are queried via broker admin API (query_metrics_json_str).
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using json = nlohmann::json;

namespace
{

struct LocalBrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    LocalBrokerHandle() = default;
    LocalBrokerHandle(LocalBrokerHandle &&) noexcept = default;
    LocalBrokerHandle &operator=(LocalBrokerHandle &&) noexcept = default;
    ~LocalBrokerHandle() { stop_and_join(); }

    void stop_and_join()
    {
        if (service)
        {
            service->stop();
            if (thread.joinable())
                thread.join();
        }
    }
};

LocalBrokerHandle start_local_broker(BrokerService::Config cfg)
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<ReadyInfo>>();
    auto future  = promise->get_future();

    cfg.on_ready = [promise](const std::string &ep, const std::string &pk)
    { promise->set_value({ep, pk}); };

    auto svc     = std::make_unique<BrokerService>(std::move(cfg));
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get();

    LocalBrokerHandle h;
    h.service  = std::move(svc);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

struct BrcHandle
{
    BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread       thread;

    void start(const std::string &ep, const std::string &pk, const std::string &uid)
    {
        BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = ep;
        cfg.broker_pubkey   = pk;
        cfg.role_uid        = uid;
        ASSERT_TRUE(brc.connect(cfg));
        thread = std::thread([this] { brc.run_poll_loop([this] { return running.load(); }); });
    }

    void stop()
    {
        running.store(false);
        brc.stop();
        if (thread.joinable())
            thread.join();
        brc.disconnect();
    }

    ~BrcHandle()
    {
        if (thread.joinable())
            stop();
    }
};

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(getpid());
}

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    json opts;
    opts["channel_name"]  = channel;
    opts["consumer_uid"]  = consumer_uid;
    opts["consumer_name"] = "test_consumer";
    opts["consumer_pid"]  = ::getpid();
    return opts;
}

} // anonymous namespace

// ============================================================================
// MetricsPlaneTest fixture
// ============================================================================

class MetricsPlaneTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(), pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetZMQContextModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        BrokerService::Config cfg;
        cfg.endpoint               = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs     = {};
        cfg.grace_override         = std::chrono::milliseconds(0);
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    json query_metrics(const std::string &channel = {})
    {
        return json::parse(svc().query_metrics_json_str(channel));
    }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<LifecycleGuard> MetricsPlaneTest::s_lifecycle_;

// ── Heartbeat with metrics ──────────────────────────────────────────────────

TEST_F(MetricsPlaneTest, HeartbeatMetrics_StoredByBroker)
{
    const std::string channel = pid_chan("metrics.heartbeat.stored");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    json metrics;
    metrics["iteration_count"] = 42;
    metrics["avg_period_us"]   = 1000;
    bh.brc.send_heartbeat(channel, metrics);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto result = query_metrics(channel);
    ASSERT_EQ(result.value("status", ""), "success");
    ASSERT_TRUE(result.contains("metrics"));
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("producer"));
    EXPECT_EQ(m["producer"].value("iteration_count", 0), 42);

    bh.stop();
}

// ── Consumer metrics report ─────────────────────────────────────────────────

TEST_F(MetricsPlaneTest, MetricsReport_ConsumerStoredByBroker)
{
    const std::string channel  = pid_chan("metrics.consumer.stored");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready — no heartbeat/sleep needed.
    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);
    auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(channel, cons_uid), 3000);
    ASSERT_TRUE(cons_reg.has_value());

    json cons_metrics;
    cons_metrics["read_count"] = 100;
    cons_bh.brc.send_metrics_report(channel, cons_uid, cons_metrics);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto result = query_metrics(channel);
    ASSERT_TRUE(result.contains("metrics"));
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("consumers"));
    ASSERT_TRUE(m["consumers"].contains(cons_uid));
    EXPECT_EQ(m["consumers"][cons_uid].value("read_count", 0), 100);

    cons_bh.stop();
    prod_bh.stop();
}

// ── Unknown channel returns empty metrics ───────────────────────────────────

TEST_F(MetricsPlaneTest, QueryMetrics_UnknownChannel_ReturnsEmpty)
{
    auto result = query_metrics(pid_chan("metrics.unknown"));
    EXPECT_EQ(result.value("status", ""), "success");
    ASSERT_TRUE(result.contains("metrics"));
    EXPECT_TRUE(result["metrics"].empty());
}

// ── Query all channels ──────────────────────────────────────────────────────

TEST_F(MetricsPlaneTest, QueryMetrics_AllChannels)
{
    const std::string channel = pid_chan("metrics.all");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    json metrics;
    metrics["test_field"] = 99;
    bh.brc.send_heartbeat(channel, metrics);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto result = query_metrics();
    ASSERT_TRUE(result.contains("channels"));
    ASSERT_TRUE(result["channels"].contains(channel));

    bh.stop();
}

// ── Heartbeat without metrics (backward compat) ────────────────────────────

TEST_F(MetricsPlaneTest, HeartbeatNoMetrics_BackwardCompat)
{
    const std::string channel = pid_chan("metrics.no.payload");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    bh.brc.send_heartbeat(channel, {});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto result = query_metrics(channel);
    EXPECT_EQ(result.value("status", ""), "success");

    bh.stop();
}

// ── Metrics overwrite on heartbeat ──────────────────────────────────────────

TEST_F(MetricsPlaneTest, MetricsUpdate_OverwriteOnHeartbeat)
{
    const std::string channel = pid_chan("metrics.overwrite");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    json m1;
    m1["iteration_count"] = 10;
    bh.brc.send_heartbeat(channel, m1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    json m2;
    m2["iteration_count"] = 20;
    bh.brc.send_heartbeat(channel, m2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto result = query_metrics(channel);
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("producer"));
    EXPECT_EQ(m["producer"].value("iteration_count", 0), 20);

    bh.stop();
}

// ── Producer PID in query result ────────────────────────────────────────────

TEST_F(MetricsPlaneTest, ProducerPID_InQueryResult)
{
    const std::string channel = pid_chan("metrics.pid");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    json metrics;
    metrics["test"] = 1;
    bh.brc.send_heartbeat(channel, metrics);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto result = query_metrics(channel);
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("producer"));
    EXPECT_EQ(m["producer"].value("pid", 0), ::getpid());

    bh.stop();
}
