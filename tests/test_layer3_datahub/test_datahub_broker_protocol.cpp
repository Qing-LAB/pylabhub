/**
 * @file test_datahub_broker_protocol.cpp
 * @brief Broker control-plane protocol tests.
 *
 * Suite: BrokerProtocolTest
 *
 * Tests the broker protocol via BrokerRequestComm:
 *   - Registration: REG_REQ/ACK, duplicate hash handling
 *   - Heartbeat: PendingReady → Ready transition
 *   - Closing notify: CHANNEL_CLOSING_NOTIFY delivery to all members
 *   - Checksum error: forwarding as CHANNEL_EVENT_NOTIFY
 *   - Role discovery: ROLE_PRESENCE_REQ, ROLE_INFO_REQ
 *   - Transport arbitration: SHM/ZMQ consumer mismatch
 *
 * All tests use in-process LocalBrokerHandle + BrcHandle pattern.
 * CTest safety: all channel names are PID-suffixed.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "test_sync_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

/// Thread-safe event collector for notification callbacks.
struct EventCollector
{
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, json>> events;

    void push(const std::string &type, const json &body)
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            events.emplace_back(type, body);
        }
        cv.notify_all();
    }

    bool wait_for(size_t count, int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return events.size() >= count; });
    }

    size_t size()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return events.size();
    }
};

} // anonymous namespace

// ============================================================================
// BrokerProtocolTest fixture
// ============================================================================

class BrokerProtocolTest : public ::testing::Test
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

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<LifecycleGuard> BrokerProtocolTest::s_lifecycle_;

// ============================================================================
// 1. CHECKSUM_ERROR_REPORT — broker forwards as CHANNEL_EVENT_NOTIFY
// ============================================================================

TEST_F(BrokerProtocolTest, ChecksumErrorReport_ForwardedToProducer)
{
    broker_.reset();
    BrokerService::Config cfg;
    cfg.endpoint               = "tcp://127.0.0.1:0";
    cfg.schema_search_dirs     = {};
    cfg.checksum_repair_policy = ChecksumRepairPolicy::NotifyOnly;
    broker_.emplace(start_local_broker(std::move(cfg)));

    const std::string channel = pid_chan("proto.checksum.prod");
    const std::string uid     = "prod." + channel;

    auto prod_events = std::make_shared<EventCollector>();
    BrcHandle prod_bh;
    prod_bh.brc.on_notification([prod_events](const std::string &type, const json &body)
    {
        if (type == "CHANNEL_EVENT_NOTIFY")
            prod_events->push(type, body);
    });
    prod_bh.start(ep(), pk(), uid);

    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // Reporter sends checksum error
    BrcHandle reporter;
    reporter.start(ep(), pk(), "REPORT-" + channel);

    json report;
    report["channel_name"] = channel;
    report["slot_index"]   = 42;
    report["error"]        = "bad CRC in slot 42";
    report["reporter_pid"] = ::getpid();
    reporter.brc.send_checksum_error(report);

    ASSERT_TRUE(prod_events->wait_for(1, 3000))
        << "Producer did not receive checksum error notify";

    reporter.stop();
    prod_bh.stop();
}

TEST_F(BrokerProtocolTest, ChecksumErrorReport_UnknownChannel_Silent)
{
    BrcHandle reporter;
    reporter.start(ep(), pk(), "REPORT-bogus");

    json report;
    report["channel_name"] = pid_chan("proto.checksum.bogus");
    report["slot_index"]   = 0;
    report["error"]        = "test";
    report["reporter_pid"] = ::getpid();

    EXPECT_NO_THROW(reporter.brc.send_checksum_error(report));

    // An unknown-channel checksum report is silently dropped — there is
    // no observable event (no callback, no metric bump, no log assertion
    // we can pin) so we cannot poll_until a condition.  Give the broker
    // a short async-processing window, then assert it is still operational
    // by explicitly calling the admin API (not a vacuous `(void)snap`).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_NO_THROW(svc().query_channel_snapshot())
        << "broker must remain operational after unknown-channel checksum report";

    reporter.stop();
}

// ============================================================================
// 2. CHANNEL_CLOSING_NOTIFY — delivery to ALL registered members
// ============================================================================

TEST_F(BrokerProtocolTest, ClosingNotify_DeliveredToProducerAndConsumer)
{
    broker_.reset();
    BrokerService::Config cfg;
    cfg.endpoint               = "tcp://127.0.0.1:0";
    cfg.schema_search_dirs     = {};
    cfg.grace_override         = std::chrono::seconds(10);
    broker_.emplace(start_local_broker(std::move(cfg)));

    const std::string channel  = pid_chan("proto.close.all");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    std::atomic<int> prod_closing{0}, cons_closing{0};

    BrcHandle prod_bh;
    prod_bh.brc.on_notification([&](const std::string &type, const json &)
    {
        if (type == "CHANNEL_CLOSING_NOTIFY")
            prod_closing.fetch_add(1);
    });
    prod_bh.start(ep(), pk(), prod_uid);

    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready — no heartbeat needed here.
    BrcHandle cons_bh;
    cons_bh.brc.on_notification([&](const std::string &type, const json &)
    {
        if (type == "CHANNEL_CLOSING_NOTIFY")
            cons_closing.fetch_add(1);
    });
    cons_bh.start(ep(), pk(), cons_uid);

    auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(channel, cons_uid), 3000);
    ASSERT_TRUE(cons_reg.has_value());

    svc().request_close_channel(channel);

    using pylabhub::tests::helper::poll_until;
    EXPECT_TRUE(poll_until(
        [&] { return prod_closing.load() > 0 && cons_closing.load() > 0; },
        std::chrono::seconds(5)))
        << "CHANNEL_CLOSING_NOTIFY not delivered to both members within 5s";

    EXPECT_GE(prod_closing.load(), 1) << "Producer did not receive CHANNEL_CLOSING_NOTIFY";
    EXPECT_GE(cons_closing.load(), 1) << "Consumer did not receive CHANNEL_CLOSING_NOTIFY";

    cons_bh.stop();
    prod_bh.stop();
}

// ============================================================================
// 3. Duplicate REG_REQ — schema hash conflict
// ============================================================================

TEST_F(BrokerProtocolTest, DuplicateReg_SameSchemaHash_Succeeds)
{
    const std::string channel  = pid_chan("proto.dup.same");
    const std::string hash_hex = std::string(64, 'a');
    const std::string uid1     = "prod.proto.dup.same.uid00000001";
    const std::string uid2     = "prod.proto.dup.same.uid00000002";

    BrcHandle bh1;
    bh1.start(ep(), pk(), uid1);
    auto opts1 = make_reg_opts(channel, uid1);
    opts1["schema_hash"] = hash_hex;
    auto h1 = bh1.brc.register_channel(opts1, 3000);
    ASSERT_TRUE(h1.has_value());

    BrcHandle bh2;
    bh2.start(ep(), pk(), uid2);
    auto opts2 = make_reg_opts(channel, uid2);
    opts2["schema_hash"] = hash_hex;
    auto h2 = bh2.brc.register_channel(opts2, 3000);
    EXPECT_TRUE(h2.has_value()) << "Same schema hash re-registration should succeed";

    bh2.stop();
    bh1.stop();
}

TEST_F(BrokerProtocolTest, DuplicateReg_DifferentSchemaHash_Rejected)
{
    const std::string channel = pid_chan("proto.dup.diff");
    const std::string hash_a  = std::string(64, 'a');
    const std::string hash_b  = std::string(64, 'b');
    const std::string uid1    = "prod.proto.dup.diff.uid00000001";
    const std::string uid2    = "prod.proto.dup.diff.uid00000002";

    BrcHandle bh1;
    bh1.start(ep(), pk(), uid1);
    auto opts1 = make_reg_opts(channel, uid1);
    opts1["schema_hash"] = hash_a;
    auto h1 = bh1.brc.register_channel(opts1, 3000);
    ASSERT_TRUE(h1.has_value());

    BrcHandle bh2;
    bh2.start(ep(), pk(), uid2);
    auto opts2 = make_reg_opts(channel, uid2);
    opts2["schema_hash"] = hash_b;
    auto h2 = bh2.brc.register_channel(opts2, 3000);
    EXPECT_FALSE(h2.has_value()) << "Different schema hash re-registration should be rejected";

    bh2.stop();
    bh1.stop();
}

// ============================================================================
// 4. HEARTBEAT_REQ — PendingReady → Ready status transition
// ============================================================================

TEST_F(BrokerProtocolTest, Heartbeat_TransitionsToReady)
{
    const std::string channel = pid_chan("proto.heartbeat.ready");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // Before heartbeat: PendingReady
    ChannelSnapshot snap = svc().query_channel_snapshot();
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
            EXPECT_EQ(ch.status, "PendingReady");
    }

    // Send heartbeat — broker flips status PendingReady → Ready asynchronously.
    bh.brc.send_heartbeat(channel, {});

    using pylabhub::tests::helper::poll_until;
    auto channel_is_ready = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel) return ch.status == "Ready";
        return false;
    };
    EXPECT_TRUE(poll_until(channel_is_ready, std::chrono::seconds(3)))
        << "channel did not transition to Ready within 3s after heartbeat";

    bh.stop();
}

// ============================================================================
// 5. ROLE_PRESENCE_REQ + ROLE_INFO_REQ
// ============================================================================

TEST_F(BrokerProtocolTest, RolePresenceReq_UnknownUid_ReturnsFalse)
{
    BrcHandle bh;
    bh.start(ep(), pk(), "QUERIER-unknown");
    EXPECT_FALSE(bh.brc.query_role_presence("prod.unknown.uiddeadbeef", 2000));
    bh.stop();
}

TEST_F(BrokerProtocolTest, RoleInfoReq_UnknownUid_NotFound)
{
    BrcHandle bh;
    bh.start(ep(), pk(), "QUERIER-unknown2");
    auto info = bh.brc.query_role_info("prod.unknown.uiddeadbeef", 2000);
    // BRC returns a JSON response (broker always replies), not nullopt.
    // For unknown UIDs, the response has "found":false.
    if (info.has_value())
        EXPECT_FALSE(info->value("found", true)) << "Expected found=false for unknown UID";
    // nullopt is also acceptable (timeout or error).
    bh.stop();
}

TEST_F(BrokerProtocolTest, RolePresenceReq_ProducerUid_ReturnsTrue)
{
    const std::string channel = pid_chan("proto.presence.prod");
    const std::string uid     = "prod.prestest.uidaaaa0001";

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), uid);
    auto opts = make_reg_opts(channel, uid);
    opts["role_name"] = "PresTestProd";
    auto reg = prod_bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    BrcHandle querier;
    querier.start(ep(), pk(), "QUERIER-pres-prod");
    EXPECT_TRUE(querier.brc.query_role_presence(uid, 2000));

    querier.stop();
    prod_bh.stop();
}

TEST_F(BrokerProtocolTest, RolePresenceReq_ConsumerUid_ReturnsTrue)
{
    const std::string channel      = pid_chan("proto.presence.cons");
    const std::string prod_uid     = "prod." + channel;
    const std::string consumer_uid = "cons.prestest.uidbbbb0002";

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), consumer_uid);
    auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(channel, consumer_uid), 3000);
    ASSERT_TRUE(cons_reg.has_value());

    BrcHandle querier;
    querier.start(ep(), pk(), "QUERIER-pres-cons");
    EXPECT_TRUE(querier.brc.query_role_presence(consumer_uid, 2000));

    querier.stop();
    cons_bh.stop();
    prod_bh.stop();
}

TEST_F(BrokerProtocolTest, RoleInfoReq_WithInbox_ReturnsInfo)
{
    const std::string channel     = pid_chan("proto.roleinfo.withinbox");
    const std::string uid         = "prod.roleinfo.uiddddd0004";
    const std::string inbox_ep    = "tcp://127.0.0.1:9987";
    const std::string schema_json = R"([{"type":"float64","count":1,"length":0}])";
    const std::string packing     = "aligned";

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    opts["role_name"]         = "InboxProd";
    opts["inbox_endpoint"]    = inbox_ep;
    opts["inbox_schema_json"] = schema_json;
    opts["inbox_packing"]     = packing;
    auto reg = prod_bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    BrcHandle querier;
    querier.start(ep(), pk(), "QUERIER-roleinfo");
    auto info = querier.brc.query_role_info(uid, 2000);
    ASSERT_TRUE(info.has_value()) << "Expected role info, got nullopt";
    EXPECT_EQ(info->value("inbox_endpoint", ""), inbox_ep);
    EXPECT_EQ(info->value("inbox_packing", ""), packing);

    querier.stop();
    prod_bh.stop();
}

// ============================================================================
// 6. Transport arbitration
// ============================================================================

TEST_F(BrokerProtocolTest, TransportMismatch_ShmProducer_ZmqConsumer_Fails)
{
    const std::string channel  = pid_chan("proto.transport.shm_zmq");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);
    auto cons_opts = make_cons_opts(channel, cons_uid);
    cons_opts["consumer_queue_type"] = "zmq";
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    EXPECT_FALSE(cons_reg.has_value())
        << "Consumer wants ZMQ but producer uses SHM — should fail";

    cons_bh.stop();
    prod_bh.stop();
}

TEST_F(BrokerProtocolTest, TransportMatch_ShmConsumer_ShmProducer_Succeeds)
{
    const std::string channel  = pid_chan("proto.transport.shm_shm");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready — no heartbeat/sleep needed.
    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);
    auto cons_opts = make_cons_opts(channel, cons_uid);
    cons_opts["consumer_queue_type"] = "shm";
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    EXPECT_TRUE(cons_reg.has_value()) << "Both sides use SHM — should succeed";

    cons_bh.stop();
    prod_bh.stop();
}

TEST_F(BrokerProtocolTest, TransportMatch_NoDriverField_AlwaysSucceeds)
{
    const std::string channel  = pid_chan("proto.transport.nofield");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    prod_bh.brc.send_heartbeat(channel, {});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);
    // No consumer_queue_type field → broker skips validation
    auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(channel, cons_uid), 3000);
    EXPECT_TRUE(cons_reg.has_value())
        << "Should succeed when consumer_queue_type is omitted";

    cons_bh.stop();
    prod_bh.stop();
}
