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
#include "utils/hub_state.hpp"
#include "utils/hub_metrics_filter.hpp"

#include "test_sync_utils.h"
#include "log_capture_fixture.h"

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
    std::unique_ptr<pylabhub::hub::HubState> hub_state;
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

    auto state     = std::make_unique<pylabhub::hub::HubState>();
    auto svc     = std::make_unique<BrokerService>(std::move(cfg), *state);
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get();

    LocalBrokerHandle h;
    h.hub_state  = std::move(state);
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

class MetricsPlaneTest : public ::testing::Test,
                          public pylabhub::tests::LogCaptureFixture
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
        LogCaptureFixture::Install();
        BrokerService::Config cfg;
        cfg.endpoint               = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs     = {};
        cfg.grace_override         = std::chrono::milliseconds(0);
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    void TearDown() override
    {
        broker_.reset();
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    json query_metrics(const std::string &channel = {})
    {
        return json::parse(svc().query_metrics_json_str(channel));
    }

    /// Wait until a freshly-queried metrics JSON satisfies @p pred or
    /// the deadline elapses.  Replaces the `sleep_for(N ms); query +
    /// assert` Class B ordering pattern: a regression in broker
    /// processing speed now fails this `poll_until` with the
    /// channel/predicate diagnostic instead of failing the downstream
    /// `EXPECT_EQ` on a stale snapshot.  Polls every 5 ms (cheap in-
    /// process call to `query_metrics_json_str`) up to 2 s by default.
    bool wait_for_metric(const std::string &channel,
                         std::function<bool(const json &)> pred,
                         std::chrono::milliseconds timeout =
                             std::chrono::seconds(2))
    {
        return ::pylabhub::tests::helper::poll_until(
            [&] { return pred(query_metrics(channel)); },
            timeout,
            std::chrono::milliseconds(5));
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

    ASSERT_TRUE(wait_for_metric(channel, [](const json &r) {
        return r.value("status", "") == "success"
            && r.contains("metrics")
            && r["metrics"].contains("producer")
            && r["metrics"]["producer"].value("iteration_count", 0) == 42;
    })) << "broker did not record producer iteration_count=42 within 2s";

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

    ASSERT_TRUE(wait_for_metric(channel, [&](const json &r) {
        return r.contains("metrics")
            && r["metrics"].contains("consumers")
            && r["metrics"]["consumers"].contains(cons_uid)
            && r["metrics"]["consumers"][cons_uid].value("read_count", 0) == 100;
    })) << "broker did not record consumer read_count=100 within 2s";

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

    // Wait until the channel appears under the all-channels query.
    // Empty channel arg → query_metrics returns "channels" map.
    ASSERT_TRUE(::pylabhub::tests::helper::poll_until([&] {
        auto r = query_metrics();
        return r.contains("channels") && r["channels"].contains(channel);
    }, std::chrono::seconds(2)))
        << "channel '" << channel << "' did not appear in all-channels "
        << "query within 2s after heartbeat";

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

    // Empty heartbeat: nothing to wait for in the metrics payload.
    // The query is unconditionally "success" once REG_REQ has succeeded
    // (which already happened above), so this just exercises the
    // backward-compat path.  No sleep needed.
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
    // Wait until m1 propagated, so the m2 overwrite is observable as
    // a transition rather than a coincident write.
    ASSERT_TRUE(wait_for_metric(channel, [](const json &r) {
        return r.contains("metrics") && r["metrics"].contains("producer")
            && r["metrics"]["producer"].value("iteration_count", 0) == 10;
    })) << "first heartbeat (iteration_count=10) did not propagate within 2s";

    json m2;
    m2["iteration_count"] = 20;
    bh.brc.send_heartbeat(channel, m2);
    ASSERT_TRUE(wait_for_metric(channel, [](const json &r) {
        return r.contains("metrics") && r["metrics"].contains("producer")
            && r["metrics"]["producer"].value("iteration_count", 0) == 20;
    })) << "second heartbeat (iteration_count=20) did not overwrite first within 2s";

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

    // Wait until the producer record carries this process's PID.
    // Replaces a bare sleep_for(200ms) Class B ordering.
    ASSERT_TRUE(wait_for_metric(channel, [](const json &r) {
        return r.contains("metrics") && r["metrics"].contains("producer")
            && r["metrics"]["producer"].value("pid", 0) == ::getpid();
    })) << "producer PID did not appear in metrics within 2s";

    auto result = query_metrics(channel);
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("producer"));
    EXPECT_EQ(m["producer"].value("pid", 0), ::getpid());

    bh.stop();
}

// ============================================================================
// Unified query engine — HEP-CORE-0033 §10.3
// ============================================================================

TEST_F(MetricsPlaneTest, QueryEngine_EmptyFilter_AllCategoriesPresent)
{
    const std::string channel = pid_chan("query.empty.all");
    const std::string uid     = "prod." + channel;
    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    pylabhub::hub::MetricsFilter f;
    json result = svc().query_metrics(f);

    EXPECT_EQ(result.value("status", ""), "success");
    EXPECT_TRUE(result.contains("queried_at"));
    EXPECT_FALSE(result.value("queried_at", "").empty());
    EXPECT_TRUE(result.contains("filter"));
    // All seven categories must be present when filter is empty.
    EXPECT_TRUE(result.contains("channels"));
    EXPECT_TRUE(result.contains("roles"));
    EXPECT_TRUE(result.contains("bands"));
    EXPECT_TRUE(result.contains("peers"));
    EXPECT_TRUE(result.contains("broker"));
    EXPECT_TRUE(result.contains("shm"));
    EXPECT_TRUE(result.contains("schemas"));

    bh.stop();
}

TEST_F(MetricsPlaneTest, QueryEngine_CategoryFilter_OnlyBroker)
{
    pylabhub::hub::MetricsFilter f;
    f.categories.insert(pylabhub::hub::metrics_category::kBroker);
    json result = svc().query_metrics(f);

    EXPECT_EQ(result.value("status", ""), "success");
    EXPECT_TRUE(result.contains("broker"));
    EXPECT_FALSE(result.contains("channels"));
    EXPECT_FALSE(result.contains("roles"));
    EXPECT_FALSE(result.contains("bands"));
    EXPECT_FALSE(result.contains("peers"));
    EXPECT_FALSE(result.contains("shm"));
    EXPECT_FALSE(result.contains("schemas"));

    // Broker counters carry _collected_at.
    EXPECT_TRUE(result["broker"].contains("_collected_at"));
}

TEST_F(MetricsPlaneTest, QueryEngine_ChannelIdentityFilter)
{
    const std::string ch1 = pid_chan("query.identity.A");
    const std::string ch2 = pid_chan("query.identity.B");

    BrcHandle b1; b1.start(ep(), pk(), "prod." + ch1);
    auto r1 = b1.brc.register_channel(make_reg_opts(ch1, "prod." + ch1), 3000);
    ASSERT_TRUE(r1.has_value());

    BrcHandle b2; b2.start(ep(), pk(), "prod." + ch2);
    auto r2 = b2.brc.register_channel(make_reg_opts(ch2, "prod." + ch2), 3000);
    ASSERT_TRUE(r2.has_value());

    pylabhub::hub::MetricsFilter f;
    f.categories.insert(pylabhub::hub::metrics_category::kChannel);
    f.channels = {ch1};
    json result = svc().query_metrics(f);

    ASSERT_TRUE(result.contains("channels"));
    EXPECT_TRUE(result["channels"].contains(ch1));
    EXPECT_FALSE(result["channels"].contains(ch2));

    b1.stop();
    b2.stop();
}

TEST_F(MetricsPlaneTest, QueryEngine_RolesCarryCollectedAt)
{
    const std::string channel = pid_chan("query.role.collected");
    const std::string uid     = "prod." + channel;
    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    json metrics;
    metrics["iteration_count"] = 7;
    bh.brc.send_heartbeat(channel, metrics);

    pylabhub::hub::MetricsFilter f;
    f.categories.insert(pylabhub::hub::metrics_category::kRole);
    f.roles = {uid};
    // Wait until the heartbeat has been processed and _collected_at
    // is populated (the post-condition the test then asserts).
    ASSERT_TRUE(::pylabhub::tests::helper::poll_until([&] {
        json r = svc().query_metrics(f);
        return r.contains("roles") && r["roles"].contains(uid)
            && !r["roles"][uid].value("_collected_at", "").empty();
    }, std::chrono::seconds(2)))
        << "role record _collected_at did not populate within 2s";

    json result = svc().query_metrics(f);
    ASSERT_TRUE(result.contains("roles"));
    ASSERT_TRUE(result["roles"].contains(uid));
    const auto &r = result["roles"][uid];
    EXPECT_EQ(r.value("uid", ""), uid);
    EXPECT_EQ(r.value("role_tag", ""), "prod");
    EXPECT_TRUE(r.contains("_collected_at"));
    EXPECT_FALSE(r.value("_collected_at", "").empty());

    bh.stop();
}

TEST_F(MetricsPlaneTest, QueryEngine_ChannelsHaveProducerAndConsumerMetrics)
{
    const std::string channel = pid_chan("query.channel.metrics");
    const std::string uid     = "prod." + channel;
    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    json metrics;
    metrics["iteration_count"] = 99;
    bh.brc.send_heartbeat(channel, metrics);

    pylabhub::hub::MetricsFilter f;
    f.categories.insert(pylabhub::hub::metrics_category::kChannel);
    // Wait until producer_metrics shows iteration_count=99.
    ASSERT_TRUE(::pylabhub::tests::helper::poll_until([&] {
        json r = svc().query_metrics(f);
        return r.contains("channels") && r["channels"].contains(channel)
            && r["channels"][channel].contains("producer_metrics")
            && r["channels"][channel]["producer_metrics"]
                   .value("iteration_count", 0) == 99;
    }, std::chrono::seconds(2)))
        << "channel producer_metrics iteration_count=99 not visible within 2s";

    json result = svc().query_metrics(f);
    ASSERT_TRUE(result.contains("channels"));
    ASSERT_TRUE(result["channels"].contains(channel));
    const auto &c = result["channels"][channel];
    EXPECT_TRUE(c.contains("producer_metrics"));
    EXPECT_EQ(c["producer_metrics"].value("iteration_count", 0), 99);
    EXPECT_TRUE(c.contains("_collected_at"));

    bh.stop();
}

TEST_F(MetricsPlaneTest, QueryEngine_FilterEcho)
{
    pylabhub::hub::MetricsFilter f;
    f.categories.insert("role");
    f.roles = {"prod.specific.uid12345678"};
    json result = svc().query_metrics(f);

    ASSERT_TRUE(result.contains("filter"));
    const auto &echo = result["filter"];
    EXPECT_TRUE(echo.contains("categories"));
    ASSERT_TRUE(echo.contains("roles"));
    ASSERT_EQ(echo["roles"].size(), 1u);
    EXPECT_EQ(echo["roles"][0].get<std::string>(),
              "prod.specific.uid12345678");
}
