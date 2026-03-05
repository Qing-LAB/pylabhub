/**
 * @file test_datahub_broker_admin.cpp
 * @brief BrokerService admin API tests: list_channels_json_str,
 *        query_channel_snapshot, request_close_channel.
 *
 * Suite: BrokerAdminTest
 *
 * Uses the LocalBrokerHandle in-process pattern (same as BrokerSchemaTest):
 * no subprocess workers needed. Messenger creates/discovers channels,
 * admin methods are called from the test thread.
 *
 * Cross-platform notes:
 *   - All endpoints use tcp://127.0.0.1:0 (ephemeral port)
 *   - Timeouts are generous (1000ms+) for Windows scheduler latency
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/messenger.hpp"

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
            pylabhub::hub::GetLifecycleModule()));
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        BrokerService::Config cfg;
        cfg.endpoint                = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs      = {};
        cfg.channel_shutdown_grace  = std::chrono::seconds(0); // immediate deregister in L3 tests
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

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));
    auto handle = m.create_channel(channel, ChannelPattern::PubSub,
                                   /*has_shared_memory=*/false, /*schema_hash=*/{},
                                   /*schema_version=*/0, /*timeout_ms=*/3000);
    ASSERT_TRUE(handle.has_value()) << "create_channel failed";

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
}

TEST_F(BrokerAdminTest, ListChannels_FieldPresence)
{
    const std::string channel = pid_chan("admin.list.fields");

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));
    auto handle = m.create_channel(channel, ChannelPattern::PubSub,
                                   /*has_shared_memory=*/false, /*schema_hash=*/{},
                                   /*schema_version=*/0, /*timeout_ms=*/3000);
    ASSERT_TRUE(handle.has_value());

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

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));
    auto handle = m.create_channel(channel, ChannelPattern::PubSub,
                                   /*has_shared_memory=*/false, /*schema_hash=*/{},
                                   /*schema_version=*/0, /*timeout_ms=*/3000);
    ASSERT_TRUE(handle.has_value());

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
}

TEST_F(BrokerAdminTest, Snapshot_AfterConsumer)
{
    const std::string channel = pid_chan("admin.snap.consumer");

    // Register producer
    Messenger prod;
    ASSERT_TRUE(prod.connect(ep(), pk()));
    auto prod_handle = prod.create_channel(channel, ChannelPattern::PubSub,
                                           /*has_shared_memory=*/false, /*schema_hash=*/{},
                                           /*schema_version=*/0, /*timeout_ms=*/3000);
    ASSERT_TRUE(prod_handle.has_value());

    // Register consumer
    Messenger cons;
    ASSERT_TRUE(cons.connect(ep(), pk()));
    auto cons_handle = cons.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

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
}

// ============================================================================
// request_close_channel tests
// ============================================================================

TEST_F(BrokerAdminTest, CloseChannel_Existing)
{
    const std::string channel = pid_chan("admin.close.existing");

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));
    auto handle = m.create_channel(channel, ChannelPattern::PubSub,
                                   /*has_shared_memory=*/false, /*schema_hash=*/{},
                                   /*schema_version=*/0, /*timeout_ms=*/3000);
    ASSERT_TRUE(handle.has_value());

    // Request close
    svc().request_close_channel(channel);

    // Give broker a poll cycle to process the close request
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Channel should be gone from snapshot
    ChannelSnapshot snap = svc().query_channel_snapshot();
    bool still_present = false;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel && ch.status != "Closing")
        {
            still_present = true;
            break;
        }
    }
    EXPECT_FALSE(still_present) << "Channel should be closed or closing";
}

TEST_F(BrokerAdminTest, CloseChannel_NonExistent)
{
    // Closing a nonexistent channel should not crash — silently ignored
    EXPECT_NO_THROW(svc().request_close_channel(pid_chan("admin.close.bogus")));

    // Give broker a poll cycle
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Broker still operational
    ChannelSnapshot snap = svc().query_channel_snapshot();
    (void)snap; // just check it doesn't crash
}
