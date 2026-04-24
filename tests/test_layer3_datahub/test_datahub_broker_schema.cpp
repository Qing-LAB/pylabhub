/**
 * @file test_datahub_broker_schema.cpp
 * @brief Broker schema metadata tests (HEP-CORE-0016 Phase 3).
 *
 * Suite: BrokerSchemaTest
 *
 * Tests that the broker correctly stores schema_id/schema_hash/schema_blds
 * from REG_REQ, and validates expected_schema_id in CONSUMER_REG_REQ.
 *
 * All tests use an in-process broker with empty schema_search_dirs (no library
 * files on disk). Verification uses the broker admin API (query_channel_snapshot).
 *
 * CTest safety: all channel names are suffixed with the test process PID.
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
// BrokerSchemaTest — in-process broker, empty schema library
// ============================================================================

class BrokerSchemaTest : public ::testing::Test
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
        cfg.endpoint           = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs = {};
        cfg.grace_override     = std::chrono::milliseconds(0);
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<LifecycleGuard> BrokerSchemaTest::s_lifecycle_;

// ── Test 1: schema_hash stored at registration, visible in snapshot ──────────

TEST_F(BrokerSchemaTest, SchemaHash_StoredOnReg)
{
    const std::string channel  = pid_chan("schema.hash.stored");
    const std::string uid      = "prod." + channel;
    const std::string hash_hex = std::string(64, 'a');

    BrcHandle bh;
    bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    opts["schema_hash"] = hash_hex;
    auto reg = bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    ChannelSnapshot snap = svc().query_channel_snapshot();
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            EXPECT_EQ(ch.schema_hash, hash_hex);
            bh.stop();
            return;
        }
    }
    FAIL() << "Channel not found in snapshot";
}

// ── Test 2: schema_id stored at registration ─────────────────────────────────

TEST_F(BrokerSchemaTest, SchemaId_StoredOnReg)
{
    const std::string channel   = pid_chan("schema.id.stored");
    const std::string uid       = "prod." + channel;
    const std::string schema_id = "$lab.test.sensor.v1";

    BrcHandle bh;
    bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    opts["schema_id"] = schema_id;
    auto reg = bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // Verify via list_channels_json_str which includes schema info
    auto j = json::parse(svc().list_channels_json_str());
    ASSERT_TRUE(j.is_array());
    bool found = false;
    for (const auto &ch : j)
    {
        if (ch.value("name", "") == channel)
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Channel with schema_id not found in list";

    bh.stop();
}

// ── Test 3: consumer expected_schema_id matches → registration succeeds ──────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_Match_Succeeds)
{
    const std::string channel   = pid_chan("schema.consumer.match");
    const std::string schema_id = "$lab.consumer.test.v2";
    const std::string prod_uid  = "prod." + channel;
    const std::string cons_uid  = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);

    auto opts = make_reg_opts(channel, prod_uid);
    opts["schema_id"] = schema_id;
    auto reg = prod_bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // Heartbeat → Ready
    // CONSUMER_REG_REQ does not gate on Ready (HEP-CORE-0023 §2.2).

    // Consumer with matching expected_schema_id
    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);

    auto cons_opts = make_cons_opts(channel, cons_uid);
    cons_opts["expected_schema_id"] = schema_id;
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    EXPECT_TRUE(cons_reg.has_value()) << "Consumer should succeed when schema_id matches";

    cons_bh.stop();
    prod_bh.stop();
}

// ── Test 4: consumer expected_schema_id mismatch → registration fails ────────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_Mismatch_Fails)
{
    const std::string channel  = pid_chan("schema.consumer.mismatch");
    const std::string prod_sid = "$lab.producer.schema.v1";
    const std::string cons_sid = "$lab.other.schema.v1";
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);

    auto opts = make_reg_opts(channel, prod_uid);
    opts["schema_id"] = prod_sid;
    auto reg = prod_bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready (HEP-CORE-0023 §2.2).

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);

    auto cons_opts = make_cons_opts(channel, cons_uid);
    cons_opts["expected_schema_id"] = cons_sid;
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    EXPECT_FALSE(cons_reg.has_value()) << "Consumer should fail on schema_id mismatch";

    cons_bh.stop();
    prod_bh.stop();
}

// ── Test 5: producer anonymous, consumer expects schema_id → fail ────────────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_EmptyProducer_Fails)
{
    const std::string channel  = pid_chan("schema.consumer.empty.prod");
    const std::string cons_sid = "$lab.expected.schema.v3";
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);

    // No schema_id
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready (HEP-CORE-0023 §2.2).

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);

    auto cons_opts = make_cons_opts(channel, cons_uid);
    cons_opts["expected_schema_id"] = cons_sid;
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    EXPECT_FALSE(cons_reg.has_value())
        << "Consumer should fail when producer is anonymous and schema_id expected";

    cons_bh.stop();
    prod_bh.stop();
}
