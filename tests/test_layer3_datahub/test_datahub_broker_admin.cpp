/**
 * @file test_datahub_broker_admin.cpp
 * @brief BrokerService admin API tests: list_channels_json_str,
 *        query_channel_snapshot, request_close_channel.
 *
 * Suite: BrokerAdminTest
 *
 * Uses the LocalBrokerHandle in-process pattern (same as BrokerSchemaTest):
 * no subprocess workers needed. BrokerRequestComm registers/discovers channels,
 * admin methods are called from the test thread.
 *
 * Cross-platform notes:
 *   - All endpoints use tcp://127.0.0.1:0 (ephemeral port)
 *   - Timeouts are generous (1000ms+) for Windows scheduler latency
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "test_sync_utils.h"

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

// ── LocalBrokerHandle — same pattern as BrokerSchemaTest ─────────────────────

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
    h.service = std::move(svc);
    h.thread  = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

std::string pid_chan(const std::string &base)
{
    return base + "." + std::to_string(getpid());
}

/// Build minimal JSON opts for BrokerRequestComm::register_channel().
nlohmann::json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    nlohmann::json opts;
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

/// Build minimal JSON opts for BrokerRequestComm::register_consumer().
nlohmann::json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    nlohmann::json opts;
    opts["channel_name"]   = channel;
    opts["consumer_uid"]   = consumer_uid;
    opts["consumer_name"]  = "test_consumer";
    opts["consumer_pid"]   = ::getpid();
    return opts;
}

/// Helper: connect a BrokerRequestComm, start its poll thread.
struct BrcHandle
{
    BrokerRequestComm       brc;
    std::atomic<bool>       running{true};
    std::thread             thread;

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

} // anonymous namespace

// ============================================================================
// BrokerAdminTest fixture
// ============================================================================

class BrokerAdminTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(), pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetDataBlockModule(),
            pylabhub::hub::GetZMQContextModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        BrokerService::Config cfg;
        cfg.endpoint                = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs      = {};
        cfg.grace_override          = std::chrono::milliseconds(0); // immediate deregister in L3 tests
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> BrokerAdminTest::s_lifecycle_;

// ============================================================================
// list_channels_json_str tests
// ============================================================================

TEST_F(BrokerAdminTest, ListChannels_Empty)
{
    // No channels registered — expect empty array
    std::string result = svc().list_channels_json_str();
    auto j = json::parse(result);
    ASSERT_TRUE(j.is_array());
    EXPECT_TRUE(j.empty()) << "Expected empty channel list, got: " << result;
}

TEST_F(BrokerAdminTest, ListChannels_OneChannel)
{
    const std::string channel = pid_chan("admin.list.one");

    BrcHandle bh;
    bh.start(ep(), pk(), "PROD-" + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "PROD-" + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    std::string result = svc().list_channels_json_str();
    auto j = json::parse(result);
    ASSERT_TRUE(j.is_array());
    ASSERT_GE(j.size(), 1u);

    // Find our channel in the array
    bool found = false;
    for (const auto &entry : j)
    {
        if (entry.value("name", "") == channel)
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Channel '" << channel << "' not found in: " << result;

    bh.stop();
}

TEST_F(BrokerAdminTest, ListChannels_FieldPresence)
{
    const std::string channel = pid_chan("admin.list.fields");

    BrcHandle bh;
    bh.start(ep(), pk(), "PROD-" + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "PROD-" + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    std::string result = svc().list_channels_json_str();
    auto j = json::parse(result);

    // Find our channel entry
    const json *entry = nullptr;
    for (const auto &e : j)
    {
        if (e.value("name", "") == channel)
        {
            entry = &e;
            break;
        }
    }
    ASSERT_NE(entry, nullptr) << "Channel not found in JSON";

    // Verify required fields are present
    EXPECT_TRUE(entry->contains("name"));
    EXPECT_TRUE(entry->contains("status"));
    EXPECT_TRUE(entry->contains("consumer_count"));
    EXPECT_TRUE(entry->contains("producer_pid"));

    bh.stop();
}

// ============================================================================
// query_channel_snapshot tests
// ============================================================================

TEST_F(BrokerAdminTest, Snapshot_Empty)
{
    ChannelSnapshot snap = svc().query_channel_snapshot();
    EXPECT_TRUE(snap.channels.empty());
}

TEST_F(BrokerAdminTest, Snapshot_OneChannel)
{
    const std::string channel = pid_chan("admin.snap.one");

    BrcHandle bh;
    bh.start(ep(), pk(), "PROD-" + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "PROD-" + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    ChannelSnapshot snap = svc().query_channel_snapshot();
    ASSERT_GE(snap.channels.size(), 1u);

    // Find our channel
    const ChannelSnapshotEntry *found = nullptr;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            found = &ch;
            break;
        }
    }
    ASSERT_NE(found, nullptr) << "Channel not in snapshot";
    EXPECT_FALSE(found->status.empty());
    EXPECT_EQ(found->consumer_count, 0);

    bh.stop();
}

TEST_F(BrokerAdminTest, Snapshot_AfterConsumer)
{
    const std::string channel = pid_chan("admin.snap.consumer");

    // Register producer
    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), "PROD-" + channel);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, "PROD-" + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    // Send heartbeat so broker marks channel Ready (required for consumer registration)
    prod_bh.brc.send_heartbeat(channel, {});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Register consumer (separate BRC instance)
    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), "CONS-" + channel);
    auto cons_reg = cons_bh.brc.register_consumer(
        make_cons_opts(channel, "CONS-" + channel), 3000);
    ASSERT_TRUE(cons_reg.has_value()) << "register_consumer failed";

    ChannelSnapshot snap = svc().query_channel_snapshot();
    const ChannelSnapshotEntry *found = nullptr;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            found = &ch;
            break;
        }
    }
    ASSERT_NE(found, nullptr) << "Channel not in snapshot";
    EXPECT_EQ(found->consumer_count, 1);

    cons_bh.stop();
    prod_bh.stop();
}

// ============================================================================
// request_close_channel tests
// ============================================================================

TEST_F(BrokerAdminTest, CloseChannel_Existing)
{
    const std::string channel = pid_chan("admin.close.existing");

    BrcHandle bh;
    bh.start(ep(), pk(), "PROD-" + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "PROD-" + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    // Request close
    svc().request_close_channel(channel);

    // Poll until the channel is either gone from the snapshot or marked
    // "Closing" (both are legitimate end-states of request_close_channel:
    // gone when members dereg'd, Closing while grace timer is active).
    using pylabhub::tests::helper::poll_until;
    auto channel_closed_or_gone = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel && ch.status != "Closing") return false;
        return true;
    };
    EXPECT_TRUE(poll_until(channel_closed_or_gone, std::chrono::seconds(3)))
        << "Channel neither removed nor in Closing state within 3s of "
           "request_close_channel";

    bh.stop();
}

TEST_F(BrokerAdminTest, CloseChannel_NonExistent)
{
    // Closing a nonexistent channel should not crash — silently ignored.
    // Because the broker takes no observable action on a bogus name
    // (no callback, no log, no snapshot change), this is a legitimate
    // "give async processing a window, then probe liveness" scenario —
    // the sleep is not ordering events, it's bounding the silent path.
    EXPECT_NO_THROW(svc().request_close_channel(pid_chan("admin.close.bogus")));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Explicit liveness probe: broker must still serve admin requests
    // (replaces the prior `(void)snap` which only caught outright
    // crashes on the snapshot call itself).
    EXPECT_NO_THROW({
        ChannelSnapshot snap = svc().query_channel_snapshot();
        (void)snap.channels.size();  // force materialization
    });
}
