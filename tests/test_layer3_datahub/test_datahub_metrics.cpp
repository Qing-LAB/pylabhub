/**
 * @file test_datahub_metrics.cpp
 * @brief Broker metrics plane tests (HEP-CORE-0019).
 *
 * Suite: MetricsPlaneTest
 *
 * Tests metrics storage and retrieval through the broker.  Post-M1.4
 * (2026-05-11), the dedicated METRICS_REPORT_REQ path is retired and
 * metrics piggyback on HEARTBEAT_REQ exclusively (HEP-CORE-0019 §2.3
 * Phase 6).
 *
 *   - Heartbeat with metrics payload → stored on per-presence row,
 *     queryable.  Same wire path for producers AND consumers.
 *   - Query: single channel, all channels, unknown channel.
 *   - Metrics overwrite on subsequent heartbeats.
 *   - Producer PID in query result.
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

#include <cppzmq/zmq_addon.hpp>
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

// Wave M1.4 (2026-05-11): inline raw-wire request helper for tests that
// need to send a literal msg_type (e.g., verifying the retired
// `METRICS_REPORT_REQ` now returns UNKNOWN_MSG_TYPE).  Mirrors the
// pattern from `tests/test_layer3_datahub/workers/datahub_broker_workers.cpp`
// `raw_req`.  Single-shot DEALER, two-frame [type, payload].  Returns
// the parsed reply body, or null JSON on timeout.
json raw_request(const std::string &endpoint,
                 const std::string &server_pubkey,
                 const std::string &msg_type,
                 const json        &payload,
                 int                timeout_ms = 2000)
{
    zmq::context_t ctx(1);
    zmq::socket_t  dealer(ctx, zmq::socket_type::dealer);

    if (server_pubkey.size() == 40)
    {
        std::array<char, 41> client_pub{};
        std::array<char, 41> client_sec{};
        if (zmq_curve_keypair(client_pub.data(), client_sec.data()) != 0)
            return {};
        dealer.set(zmq::sockopt::curve_serverkey, server_pubkey);
        dealer.set(zmq::sockopt::curve_publickey, std::string(client_pub.data(), 40));
        dealer.set(zmq::sockopt::curve_secretkey, std::string(client_sec.data(), 40));
    }

    dealer.connect(endpoint);

    static constexpr char kCtrl = 'C';
    const std::string     body  = payload.dump();
    std::vector<zmq::const_buffer> frames = {zmq::buffer(&kCtrl, 1),
                                              zmq::buffer(msg_type),
                                              zmq::buffer(body)};
    if (!zmq::send_multipart(dealer, frames)) return {};

    std::vector<zmq::pollitem_t> items = {{dealer.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, std::chrono::milliseconds(timeout_ms));
    if ((items[0].revents & ZMQ_POLLIN) == 0) return {};  // timeout

    std::vector<zmq::message_t> recv_frames;
    if (!zmq::recv_multipart(dealer, std::back_inserter(recv_frames))) return {};
    if (recv_frames.size() < 2) return {};
    // Frame layout from broker: [type_str, json_body] (no ctrl byte on reply).
    return json::parse(std::string(recv_frames.back().data<char>(),
                                    recv_frames.back().size()));
}

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
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
    bh.brc.send_heartbeat(channel, uid, "producer", metrics);

    // Wave M2.5 G1 — producer metrics are per-uid tree, not single
    // blob.  Tests now look up `producers[uid]`.
    ASSERT_TRUE(wait_for_metric(channel, [&uid](const json &r) {
        return r.value("status", "") == "success"
            && r.contains("metrics")
            && r["metrics"].contains("producers")
            && r["metrics"]["producers"].contains(uid)
            && r["metrics"]["producers"][uid].value("iteration_count", 0) == 42;
    })) << "broker did not record producer iteration_count=42 within 2s";

    auto result = query_metrics(channel);
    ASSERT_EQ(result.value("status", ""), "success");
    ASSERT_TRUE(result.contains("metrics"));
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("producers"));
    ASSERT_TRUE(m["producers"].contains(uid));
    EXPECT_EQ(m["producers"][uid].value("iteration_count", 0), 42);

    bh.stop();
}

// ── Consumer metrics via heartbeat-piggyback (post-M1.4) ───────────────────

TEST_F(MetricsPlaneTest, ConsumerHeartbeatMetrics_StoredByBroker)
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

    // M1.4 (2026-05-11): metrics piggyback on HEARTBEAT_REQ
    // (HEP-CORE-0019 §2.3 Phase 6).  Consumer-side heartbeat carries
    // the metrics — `BrokerRequestComm::send_metrics_report` was
    // retired alongside the dedicated METRICS_REPORT_REQ wire path.
    json cons_metrics;
    cons_metrics["read_count"] = 100;
    cons_bh.brc.send_heartbeat(channel, cons_uid, "consumer", cons_metrics);

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
    bh.brc.send_heartbeat(channel, uid, "producer", metrics);

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

    bh.brc.send_heartbeat(channel, uid, "producer", {});

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
    bh.brc.send_heartbeat(channel, uid, "producer", m1);
    // Wait until m1 propagated, so the m2 overwrite is observable as
    // a transition rather than a coincident write.
    // Wave M2.5 G1: per-uid producer tree.
    ASSERT_TRUE(wait_for_metric(channel, [&uid](const json &r) {
        return r.contains("metrics") && r["metrics"].contains("producers")
            && r["metrics"]["producers"].contains(uid)
            && r["metrics"]["producers"][uid].value("iteration_count", 0) == 10;
    })) << "first heartbeat (iteration_count=10) did not propagate within 2s";

    json m2;
    m2["iteration_count"] = 20;
    bh.brc.send_heartbeat(channel, uid, "producer", m2);
    ASSERT_TRUE(wait_for_metric(channel, [&uid](const json &r) {
        return r.contains("metrics") && r["metrics"].contains("producers")
            && r["metrics"]["producers"].contains(uid)
            && r["metrics"]["producers"][uid].value("iteration_count", 0) == 20;
    })) << "second heartbeat (iteration_count=20) did not overwrite first within 2s";

    auto result = query_metrics(channel);
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("producers"));
    ASSERT_TRUE(m["producers"].contains(uid));
    EXPECT_EQ(m["producers"][uid].value("iteration_count", 0), 20);

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
    bh.brc.send_heartbeat(channel, uid, "producer", metrics);

    // Wait until the producer record carries this process's PID.
    // Replaces a bare sleep_for(200ms) Class B ordering.
    // Wave M2.5 G1: per-uid producer tree carries per-producer pid.
    ASSERT_TRUE(wait_for_metric(channel, [&uid](const json &r) {
        return r.contains("metrics") && r["metrics"].contains("producers")
            && r["metrics"]["producers"].contains(uid)
            && r["metrics"]["producers"][uid].value("pid", 0) == ::getpid();
    })) << "producer PID did not appear in metrics within 2s";

    auto result = query_metrics(channel);
    auto &m = result["metrics"];
    ASSERT_TRUE(m.contains("producers"));
    ASSERT_TRUE(m["producers"].contains(uid));
    EXPECT_EQ(m["producers"][uid].value("pid", 0), ::getpid());

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
    bh.brc.send_heartbeat(channel, uid, "producer", metrics);

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
    bh.brc.send_heartbeat(channel, uid, "producer", metrics);

    pylabhub::hub::MetricsFilter f;
    f.categories.insert(pylabhub::hub::metrics_category::kChannel);
    // Wave M2.5 G1: producer_metrics is a per-uid tree (mirrors
    // consumer_metrics), not a single blob.  Wait until uid's slot
    // shows iteration_count=99.
    ASSERT_TRUE(::pylabhub::tests::helper::poll_until([&] {
        json r = svc().query_metrics(f);
        return r.contains("channels") && r["channels"].contains(channel)
            && r["channels"][channel].contains("producer_metrics")
            && r["channels"][channel]["producer_metrics"].contains(uid)
            && r["channels"][channel]["producer_metrics"][uid]
                   .value("iteration_count", 0) == 99;
    }, std::chrono::seconds(2)))
        << "channel producer_metrics iteration_count=99 not visible within 2s";

    json result = svc().query_metrics(f);
    ASSERT_TRUE(result.contains("channels"));
    ASSERT_TRUE(result["channels"].contains(channel));
    const auto &c = result["channels"][channel];
    EXPECT_TRUE(c.contains("producer_metrics"));
    ASSERT_TRUE(c["producer_metrics"].contains(uid));
    EXPECT_EQ(c["producer_metrics"][uid].value("iteration_count", 0), 99);
    EXPECT_TRUE(c.contains("_collected_at"));

    bh.stop();
}

// ── Wave M2.5 G1 — multi-producer metrics isolation ────────────────────────
//
// Pin the contract that two Fan-In producers' metrics reports do NOT
// overwrite each other.  Pre-G1, `ChannelMetrics::producer` was a
// single scalar field; this test would have FAILED (the second
// reporter overwrote the first).  Post-G1 the storage is keyed by
// role_uid; each producer's slot survives independently.
TEST_F(MetricsPlaneTest, FanIn_TwoProducers_MetricsDoNotOverwrite)
{
    const std::string channel = pid_chan("metrics.fanin");
    // Per HEP-CORE-0033 §G2.2.0b: each `tag.name.unique` component
    // starts with a letter.  `pidNNN` prefix keeps the last component
    // letter-led.
    const std::string uid_a   = "prod.fanin.a.pid" + std::to_string(::getpid());
    const std::string uid_b   = "prod.fanin.b.pid" + std::to_string(::getpid());

    BrcHandle bh_a, bh_b;
    bh_a.start(ep(), pk(), uid_a);
    bh_b.start(ep(), pk(), uid_b);

    // Both register on the same channel with the same schema (Fan-In).
    // ZMQ transport — SHM forbids multi-producer (HEP-CORE-0023 §2.1.1).
    auto opts_a = make_reg_opts(channel, uid_a);
    opts_a["data_transport"] = "zmq";
    auto reg_a = bh_a.brc.register_channel(opts_a, 3000);
    ASSERT_TRUE(reg_a.has_value()) << reg_a.value_or(json{}).dump();
    ASSERT_EQ(reg_a->value("status", ""), "success") << reg_a->dump();

    auto opts_b = make_reg_opts(channel, uid_b);
    opts_b["data_transport"] = "zmq";
    auto reg_b = bh_b.brc.register_channel(opts_b, 3000);
    ASSERT_TRUE(reg_b.has_value()) << reg_b.value_or(json{}).dump();
    ASSERT_EQ(reg_b->value("status", ""), "success") << reg_b->dump();

    // A reports iteration_count=100; B reports iteration_count=200.
    json ma, mb;
    ma["iteration_count"] = 100;
    mb["iteration_count"] = 200;
    bh_a.brc.send_heartbeat(channel, uid_a, "producer", ma);
    bh_b.brc.send_heartbeat(channel, uid_b, "producer", mb);

    // Wait until BOTH producers' slots exist with the expected values.
    ASSERT_TRUE(wait_for_metric(channel, [&](const json &r) {
        if (!r.contains("metrics")) return false;
        const auto &m = r["metrics"];
        if (!m.contains("producers")) return false;
        const auto &p = m["producers"];
        return p.contains(uid_a) && p[uid_a].value("iteration_count", 0) == 100
            && p.contains(uid_b) && p[uid_b].value("iteration_count", 0) == 200;
    })) << "Both producers' metrics must coexist in the per-uid tree "
           "(pre-G1 the second report overwrote the first); within 2s";

    // Final direct read also confirms.
    auto result = query_metrics(channel);
    const auto &p = result["metrics"]["producers"];
    EXPECT_EQ(p[uid_a].value("iteration_count", 0), 100)
        << "Producer A's slot must NOT be overwritten by B's report";
    EXPECT_EQ(p[uid_b].value("iteration_count", 0), 200)
        << "Producer B's slot must NOT be overwritten by A's report";
    EXPECT_EQ(p[uid_a].value("pid", 0), ::getpid());
    EXPECT_EQ(p[uid_b].value("pid", 0), ::getpid());

    bh_b.stop();
    bh_a.stop();
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

// ──────────────────────────────────────────────────────────────────────
// Wave M1.4 (2026-05-11) — wire-protocol retirement tests.
// ──────────────────────────────────────────────────────────────────────

TEST_F(MetricsPlaneTest, OldMetricsReportReq_GetsUnknownMsgType)
{
    // M1.4 contract: METRICS_REPORT_REQ is RETIRED from the protocol.
    // Old clients sending this wire message receive an UNKNOWN_MSG_TYPE
    // error reply (and broker_proto_major is bumped 1 → 2 to signal
    // the break — see plh_version_registry.hpp).
    //
    // Sensitivity: a regression that re-adds METRICS_REPORT_REQ to
    // the dispatch table OR to kFireAndForgetTypes would make this
    // test fail.
    //
    // The broker EMITS a WARN ("unknown msg_type 'METRICS_REPORT_REQ'")
    // intentionally — declare it as expected so the log-capture
    // fixture doesn't treat it as a regression signal.
    ExpectLogWarn("unknown msg_type 'METRICS_REPORT_REQ'");

    const std::string channel = pid_chan("metrics.retired");
    const std::string uid     = "prod." + channel;

    // Pre-register the channel so the broker has SOMETHING for the
    // payload's channel_name to reference (proves the rejection is
    // about msg_type, not channel-not-found).
    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    ASSERT_TRUE(bh.brc.register_channel(make_reg_opts(channel, uid), 3000)
                    .has_value());

    json payload;
    payload["channel_name"] = channel;
    payload["uid"]          = uid;
    payload["metrics"]      = json{{"legacy_field", 1}};

    json reply = raw_request(ep(), pk(), "METRICS_REPORT_REQ", payload);
    ASSERT_FALSE(reply.is_null())
        << "Expected an UNKNOWN_MSG_TYPE reply within 2 s; got nothing "
           "(the broker may be silently dropping the request rather "
           "than responding with an error)";
    EXPECT_EQ(reply.value("status", std::string{}), "error");
    EXPECT_EQ(reply.value("error_code", std::string{}), "UNKNOWN_MSG_TYPE")
        << "Wire-protocol break must surface as UNKNOWN_MSG_TYPE per "
           "broker_service.cpp dispatch fallback";

    bh.stop();
}

TEST_F(MetricsPlaneTest, MultiPresence_EndToEnd_NoCrossAttribution)
{
    // M1.4 + Wave M3 H34 end-to-end contract: multiple producers and
    // consumers heartbeat distinct metrics; each presence row must
    // hold ONLY its own data — no cross-attribution at any layer of
    // the stack (wire → broker → HubState → query).
    //
    // The pre-Phase-6 bug class (H34 root) would have mis-attributed
    // consumer heartbeats to the channel's producer row.  This test
    // exercises that scenario at the full broker layer with raw
    // BrcHandle clients, then asserts via the admin-query that every
    // uid's metrics land on the right (channel, role_type) row.
    const std::string ch_a   = pid_chan("metrics.multi.A");
    const std::string ch_b   = pid_chan("metrics.multi.B");
    const std::string p_a    = "prod.A." + ch_a;
    const std::string p_b    = "prod.B." + ch_b;
    const std::string c_a    = "cons.A." + ch_a;
    const std::string c_b    = "cons.B." + ch_b;

    // Producers: each registers its channel.
    BrcHandle prod_a_bh, prod_b_bh;
    prod_a_bh.start(ep(), pk(), p_a);
    prod_b_bh.start(ep(), pk(), p_b);
    ASSERT_TRUE(prod_a_bh.brc.register_channel(make_reg_opts(ch_a, p_a), 3000)
                    .has_value());
    ASSERT_TRUE(prod_b_bh.brc.register_channel(make_reg_opts(ch_b, p_b), 3000)
                    .has_value());

    // Consumers: each registers AS CONSUMER on the appropriate channel.
    BrcHandle cons_a_bh, cons_b_bh;
    cons_a_bh.start(ep(), pk(), c_a);
    cons_b_bh.start(ep(), pk(), c_b);
    ASSERT_TRUE(cons_a_bh.brc.register_consumer(make_cons_opts(ch_a, c_a), 3000)
                    .has_value());
    ASSERT_TRUE(cons_b_bh.brc.register_consumer(make_cons_opts(ch_b, c_b), 3000)
                    .has_value());

    // Each role heartbeats DISTINCT metrics.  Using literal values
    // makes cross-attribution impossible to miss in the assertions.
    prod_a_bh.brc.send_heartbeat(ch_a, p_a, "producer", json{{"tag", "P-A"}, {"v", 11}});
    prod_b_bh.brc.send_heartbeat(ch_b, p_b, "producer", json{{"tag", "P-B"}, {"v", 22}});
    cons_a_bh.brc.send_heartbeat(ch_a, c_a, "consumer", json{{"tag", "C-A"}, {"v", 33}});
    cons_b_bh.brc.send_heartbeat(ch_b, c_b, "consumer", json{{"tag", "C-B"}, {"v", 44}});

    // Wait for all four metrics rows to land.
    ASSERT_TRUE(::pylabhub::tests::helper::poll_until([&] {
        auto r = query_metrics();
        if (!r.contains("channels")) return false;
        const auto &chans = r["channels"];
        return chans.contains(ch_a) && chans.contains(ch_b)
            && chans[ch_a].contains("producers")
            && chans[ch_a].contains("consumers")
            && chans[ch_b].contains("producers")
            && chans[ch_b].contains("consumers")
            && chans[ch_a]["producers"].contains(p_a)
            && chans[ch_a]["consumers"].contains(c_a)
            && chans[ch_b]["producers"].contains(p_b)
            && chans[ch_b]["consumers"].contains(c_b);
    }, std::chrono::seconds(2)))
        << "Not all four metrics rows appeared within 2 s";

    auto r = query_metrics();
    const auto &cha = r["channels"][ch_a];
    const auto &chb = r["channels"][ch_b];

    // Channel A: producer P-A's metrics on producer row, consumer C-A's
    // metrics on consumer row.  NO cross-attribution.
    EXPECT_EQ(cha["producers"][p_a]["tag"], "P-A");
    EXPECT_EQ(cha["producers"][p_a]["v"],   11);
    EXPECT_EQ(cha["consumers"][c_a]["tag"], "C-A");
    EXPECT_EQ(cha["consumers"][c_a]["v"],   33);
    EXPECT_FALSE(cha["producers"][p_a].contains("tag")
                  && cha["producers"][p_a]["tag"] == "C-A")
        << "Sanity: producer P-A row must NOT hold C-A's tag (H34-root)";

    // Channel B: same isolation property.
    EXPECT_EQ(chb["producers"][p_b]["tag"], "P-B");
    EXPECT_EQ(chb["producers"][p_b]["v"],   22);
    EXPECT_EQ(chb["consumers"][c_b]["tag"], "C-B");
    EXPECT_EQ(chb["consumers"][c_b]["v"],   44);

    // Cross-channel isolation: ch_a doesn't show ch_b's roles.
    EXPECT_FALSE(cha["producers"].contains(p_b))
        << "Channel A must NOT show channel B's producer";
    EXPECT_FALSE(cha["consumers"].contains(c_b))
        << "Channel A must NOT show channel B's consumer";
    EXPECT_FALSE(chb["producers"].contains(p_a));
    EXPECT_FALSE(chb["consumers"].contains(c_a));

    prod_a_bh.stop();
    prod_b_bh.stop();
    cons_a_bh.stop();
    cons_b_bh.stop();
}

TEST_F(MetricsPlaneTest, AllChannels_IncludesChannelsWithoutMetrics)
{
    // M1.4 behavioral change: pre-fix `query_metrics()` (all-channels)
    // iterated `metrics_store_` (only channels with reports);
    // post-fix iterates HubState's `snapshot().channels` (ALL
    // registered channels).  Channels with no metrics appear with an
    // empty `{}` metrics object.  This is a diagnostic improvement —
    // admin can see registered channels even before any heartbeats.
    const std::string channel_with    = pid_chan("metrics.has.metrics");
    const std::string channel_without = pid_chan("metrics.no.metrics");
    const std::string uid_with        = "prod." + channel_with;
    const std::string uid_without     = "prod." + channel_without;

    BrcHandle bh1, bh2;
    bh1.start(ep(), pk(), uid_with);
    bh2.start(ep(), pk(), uid_without);
    ASSERT_TRUE(bh1.brc.register_channel(make_reg_opts(channel_with, uid_with), 3000)
                    .has_value());
    ASSERT_TRUE(bh2.brc.register_channel(make_reg_opts(channel_without, uid_without), 3000)
                    .has_value());

    // channel_with: heartbeat with metrics.
    bh1.brc.send_heartbeat(channel_with, uid_with, "producer",
                            json{{"data_point", 7}});
    // channel_without: NO heartbeat.  Registered but never reported.

    ASSERT_TRUE(::pylabhub::tests::helper::poll_until([&] {
        auto r = query_metrics();
        return r.contains("channels")
            && r["channels"].contains(channel_with)
            && r["channels"][channel_with].contains("producers");
    }, std::chrono::seconds(2)))
        << "channel_with did not record its metrics within 2s";

    auto r = query_metrics();
    ASSERT_TRUE(r.contains("channels"));

    // Both channels MUST appear, even though channel_without has no
    // metrics reports.  This is the M1.4 shape change.
    ASSERT_TRUE(r["channels"].contains(channel_with))
        << "Channel with metrics appears (was true pre-M1.4 too)";
    ASSERT_TRUE(r["channels"].contains(channel_without))
        << "M1.4 contract: channel WITHOUT metrics also appears in "
           "all-channels query (with empty metrics object).  Pre-fix "
           "this channel would have been silently absent because "
           "`metrics_store_` only contained reported channels.";

    // channel_with carries its metrics.
    EXPECT_EQ(r["channels"][channel_with]["producers"][uid_with]["data_point"], 7);

    // channel_without exists but its metrics object is empty (no
    // "producers" or "consumers" keys because no presence reported).
    auto &cw = r["channels"][channel_without];
    EXPECT_FALSE(cw.contains("producers"))
        << "Empty channel has no producers metrics";
    EXPECT_FALSE(cw.contains("consumers"));

    bh1.stop();
    bh2.stop();
}
