/**
 * @file test_datahub_metrics.cpp
 * @brief HEP-CORE-0019 Metrics Plane tests.
 *
 * Suite: MetricsPlaneTest
 *
 * Tests the metrics infrastructure at the broker/protocol level:
 *   - Heartbeat with metrics → broker MetricsStore
 *   - METRICS_REPORT_REQ (consumer) → broker MetricsStore
 *   - query_metrics_json_str() public API
 *   - Empty / single / all-channel queries
 *   - Backward compatibility (heartbeat without metrics)
 *   - Multiple consumers, overwrite semantics
 *
 * Uses the in-process LocalBrokerHandle + Messenger pattern (no subprocess workers).
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/messenger.hpp"

#include <chrono>
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

// ── LocalBrokerHandle — in-process broker ────────────────────────────────────

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

std::string pid_chan(const std::string &base)
{
    return base + "." + std::to_string(getpid());
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
            pylabhub::hub::GetLifecycleModule()));
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        BrokerService::Config cfg;
        cfg.endpoint               = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs     = {};
        cfg.channel_shutdown_grace = std::chrono::seconds(0);
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    void TearDown() override { broker_.reset(); }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> MetricsPlaneTest::s_lifecycle_;

// ============================================================================
// 1. Heartbeat with metrics → broker stores producer metrics
// ============================================================================

TEST_F(MetricsPlaneTest, HeartbeatMetrics_StoredByBroker)
{
    const std::string channel = pid_chan("metrics.hb.store");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel);
    ASSERT_TRUE(handle.has_value());

    // Send heartbeat with metrics payload.
    json metrics;
    metrics["base"]   = {{"out_written", 100}, {"drops", 2}, {"script_errors", 0}};
    metrics["custom"] = {{"my_key", 42.5}};
    producer.enqueue_heartbeat(channel, metrics);

    // Give broker time to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Query via public API.
    std::string result_str = svc().query_metrics_json_str(channel);
    json result = json::parse(result_str);
    EXPECT_EQ(result["status"], "success");
    ASSERT_TRUE(result.contains("metrics"));
    ASSERT_TRUE(result["metrics"].contains("producer"));
    EXPECT_EQ(result["metrics"]["producer"]["base"]["out_written"], 100);
    EXPECT_EQ(result["metrics"]["producer"]["base"]["drops"], 2);
    EXPECT_DOUBLE_EQ(result["metrics"]["producer"]["custom"]["my_key"].get<double>(), 42.5);
}

// ============================================================================
// 2. METRICS_REPORT_REQ (consumer) → broker stores consumer metrics
// ============================================================================

TEST_F(MetricsPlaneTest, MetricsReport_ConsumerStoredByBroker)
{
    const std::string channel = pid_chan("metrics.report.cons");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto phandle = producer.create_channel(channel);
    ASSERT_TRUE(phandle.has_value());

    // Heartbeat to transition channel to Ready.
    producer.enqueue_heartbeat(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Consumer connects.
    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto chandle = consumer.connect_channel(channel, /*timeout_ms=*/5000,
                                            /*schema_hash=*/{},
                                            /*consumer_uid=*/"CONS-1");
    ASSERT_TRUE(chandle.has_value());

    // Consumer sends metrics report.
    json metrics;
    metrics["base"]   = {{"in_received", 50}, {"script_errors", 1}};
    metrics["custom"] = {{"bytes_processed", 8192.0}};
    consumer.enqueue_metrics_report(channel, "CONS-1", metrics);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Query single channel.
    json result = json::parse(svc().query_metrics_json_str(channel));
    EXPECT_EQ(result["status"], "success");
    ASSERT_TRUE(result["metrics"].contains("consumers"));
    ASSERT_TRUE(result["metrics"]["consumers"].contains("CONS-1"));
    EXPECT_EQ(result["metrics"]["consumers"]["CONS-1"]["base"]["in_received"], 50);
    EXPECT_DOUBLE_EQ(
        result["metrics"]["consumers"]["CONS-1"]["custom"]["bytes_processed"].get<double>(),
        8192.0);
}

// ============================================================================
// 3. Query: unknown channel returns empty metrics
// ============================================================================

TEST_F(MetricsPlaneTest, QueryMetrics_UnknownChannel_ReturnsEmpty)
{
    json result = json::parse(svc().query_metrics_json_str("nonexistent.channel"));
    EXPECT_EQ(result["status"], "success");
    EXPECT_TRUE(result["metrics"].is_object());
    EXPECT_TRUE(result["metrics"].empty());
}

// ============================================================================
// 4. Query: all channels
// ============================================================================

TEST_F(MetricsPlaneTest, QueryMetrics_AllChannels)
{
    const std::string ch1 = pid_chan("metrics.all.ch1");
    const std::string ch2 = pid_chan("metrics.all.ch2");

    Messenger prod1;
    ASSERT_TRUE(prod1.connect(ep(), pk()));
    auto h1 = prod1.create_channel(ch1);
    ASSERT_TRUE(h1.has_value());

    Messenger prod2;
    ASSERT_TRUE(prod2.connect(ep(), pk()));
    auto h2 = prod2.create_channel(ch2);
    ASSERT_TRUE(h2.has_value());

    json m1;
    m1["base"]   = {{"out_written", 10}};
    m1["custom"] = json::object();
    prod1.enqueue_heartbeat(ch1, m1);

    json m2;
    m2["base"]   = {{"out_written", 20}};
    m2["custom"] = json::object();
    prod2.enqueue_heartbeat(ch2, m2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Query all channels (empty string).
    json result = json::parse(svc().query_metrics_json_str(""));
    EXPECT_EQ(result["status"], "success");
    ASSERT_TRUE(result.contains("channels"));
    EXPECT_TRUE(result["channels"].contains(ch1));
    EXPECT_TRUE(result["channels"].contains(ch2));
    EXPECT_EQ(result["channels"][ch1]["producer"]["base"]["out_written"], 10);
    EXPECT_EQ(result["channels"][ch2]["producer"]["base"]["out_written"], 20);
}

// ============================================================================
// 5. Heartbeat without metrics (backward compat): no crash, no stored data
// ============================================================================

TEST_F(MetricsPlaneTest, HeartbeatNoMetrics_BackwardCompat)
{
    const std::string channel = pid_chan("metrics.nodata");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel);
    ASSERT_TRUE(handle.has_value());

    // Send heartbeat without metrics (old API).
    producer.enqueue_heartbeat(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Query — no metrics stored.
    json result = json::parse(svc().query_metrics_json_str(channel));
    EXPECT_EQ(result["status"], "success");
    EXPECT_TRUE(result["metrics"].empty() || !result["metrics"].contains("producer"));
}

// ============================================================================
// 6. Multiple consumer metrics reports merge correctly
// ============================================================================

TEST_F(MetricsPlaneTest, MultipleConsumers_MergeMetrics)
{
    const std::string channel = pid_chan("metrics.multi.cons");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto phandle = producer.create_channel(channel);
    ASSERT_TRUE(phandle.has_value());

    producer.enqueue_heartbeat(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Messenger cons1, cons2;
    ASSERT_TRUE(cons1.connect(ep(), pk()));
    ASSERT_TRUE(cons2.connect(ep(), pk()));
    auto h1 = cons1.connect_channel(channel, 5000, {}, "CONS-A");
    auto h2 = cons2.connect_channel(channel, 5000, {}, "CONS-B");
    ASSERT_TRUE(h1.has_value());
    ASSERT_TRUE(h2.has_value());

    json m1, m2;
    m1["base"]   = {{"in_received", 100}};
    m1["custom"] = json::object();
    m2["base"]   = {{"in_received", 200}};
    m2["custom"] = {{"lag_ms", 5.0}};
    cons1.enqueue_metrics_report(channel, "CONS-A", m1);
    cons2.enqueue_metrics_report(channel, "CONS-B", m2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    json result = json::parse(svc().query_metrics_json_str(channel));
    ASSERT_TRUE(result["metrics"].contains("consumers"));
    auto &consumers = result["metrics"]["consumers"];
    ASSERT_TRUE(consumers.contains("CONS-A"));
    ASSERT_TRUE(consumers.contains("CONS-B"));
    EXPECT_EQ(consumers["CONS-A"]["base"]["in_received"], 100);
    EXPECT_EQ(consumers["CONS-B"]["base"]["in_received"], 200);
    EXPECT_DOUBLE_EQ(consumers["CONS-B"]["custom"]["lag_ms"].get<double>(), 5.0);
}

// ============================================================================
// 7. Metrics update: second heartbeat overwrites previous
// ============================================================================

TEST_F(MetricsPlaneTest, MetricsUpdate_OverwriteOnHeartbeat)
{
    const std::string channel = pid_chan("metrics.overwrite");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel);
    ASSERT_TRUE(handle.has_value());

    json m1;
    m1["base"]   = {{"out_written", 10}};
    m1["custom"] = {{"k", 1.0}};
    producer.enqueue_heartbeat(channel, m1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Second heartbeat overwrites.
    json m2;
    m2["base"]   = {{"out_written", 50}};
    m2["custom"] = {{"k", 5.0}};
    producer.enqueue_heartbeat(channel, m2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    json result = json::parse(svc().query_metrics_json_str(channel));
    EXPECT_EQ(result["metrics"]["producer"]["base"]["out_written"], 50);
    EXPECT_DOUBLE_EQ(result["metrics"]["producer"]["custom"]["k"].get<double>(), 5.0);
}

// ============================================================================
// 8. Producer + consumer metrics in same channel
// ============================================================================

TEST_F(MetricsPlaneTest, ProducerAndConsumer_SameChannel)
{
    const std::string channel = pid_chan("metrics.both");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto phandle = producer.create_channel(channel);
    ASSERT_TRUE(phandle.has_value());

    // Producer heartbeat with metrics.
    json pm;
    pm["base"]   = {{"out_written", 1000}};
    pm["custom"] = json::object();
    producer.enqueue_heartbeat(channel, pm);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Consumer connects and reports.
    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto chandle = consumer.connect_channel(channel, 5000, {}, "CONS-X");
    ASSERT_TRUE(chandle.has_value());

    json cm;
    cm["base"]   = {{"in_received", 990}};
    cm["custom"] = json::object();
    consumer.enqueue_metrics_report(channel, "CONS-X", cm);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    json result = json::parse(svc().query_metrics_json_str(channel));
    EXPECT_EQ(result["status"], "success");
    EXPECT_TRUE(result["metrics"].contains("producer"));
    EXPECT_TRUE(result["metrics"].contains("consumers"));
    EXPECT_EQ(result["metrics"]["producer"]["base"]["out_written"], 1000);
    EXPECT_EQ(result["metrics"]["consumers"]["CONS-X"]["base"]["in_received"], 990);
}

// ============================================================================
// 9. Consumer metrics report with missing fields is silently ignored
// ============================================================================

TEST_F(MetricsPlaneTest, MetricsReport_MissingFields_Ignored)
{
    const std::string channel = pid_chan("metrics.bad.report");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto phandle = producer.create_channel(channel);
    ASSERT_TRUE(phandle.has_value());

    producer.enqueue_heartbeat(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send report with empty uid (should be ignored by broker).
    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto chandle = consumer.connect_channel(channel, 5000, {}, "CONS-Z");
    ASSERT_TRUE(chandle.has_value());

    // enqueue_metrics_report with empty uid — broker should log warning and ignore.
    json metrics;
    metrics["base"]   = {{"in_received", 10}};
    metrics["custom"] = json::object();
    consumer.enqueue_metrics_report(channel, "", metrics);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // No consumer metrics stored (empty uid was rejected).
    json result = json::parse(svc().query_metrics_json_str(channel));
    EXPECT_TRUE(!result["metrics"].contains("consumers") ||
                result["metrics"]["consumers"].empty());
}

// ============================================================================
// 10. PID is stored in producer metrics query
// ============================================================================

TEST_F(MetricsPlaneTest, ProducerPID_InQueryResult)
{
    const std::string channel = pid_chan("metrics.pid");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel);
    ASSERT_TRUE(handle.has_value());

    json metrics;
    metrics["base"]   = {{"out_written", 1}};
    metrics["custom"] = json::object();
    producer.enqueue_heartbeat(channel, metrics);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    json result = json::parse(svc().query_metrics_json_str(channel));
    ASSERT_TRUE(result["metrics"].contains("producer"));
    // PID should be set (injected by query_metrics from the heartbeat's producer_pid).
    EXPECT_TRUE(result["metrics"]["producer"].contains("pid"));
    EXPECT_GT(result["metrics"]["producer"]["pid"].get<uint64_t>(), 0u);
}
