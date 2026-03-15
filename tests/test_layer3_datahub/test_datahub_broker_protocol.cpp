/**
 * @file test_datahub_broker_protocol.cpp
 * @brief Broker control-plane protocol tests: broadcast, notify, list_channels.
 *
 * Suite: BrokerProtocolTest
 *
 * Tests the three user-facing control-plane APIs at the protocol level:
 *   - CHANNEL_BROADCAST_REQ → CHANNEL_BROADCAST_NOTIFY (fan-out to all members)
 *   - CHANNEL_NOTIFY_REQ → CHANNEL_EVENT_NOTIFY (producer-only delivery)
 *   - CHANNEL_LIST_REQ → CHANNEL_LIST_ACK (via Messenger)
 *   - Admin request_broadcast_channel() (broker internal queue)
 *
 * All tests use the in-process LocalBrokerHandle pattern (no subprocess workers).
 * Each test uses Messenger instances to register channels and receive callbacks.
 *
 * CTest safety: all channel names are suffixed with the test process PID.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/messenger.hpp"

#include <atomic>
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

/// Simple event collector: captures events from on_channel_error callback.
struct EventCollector
{
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, json>> events; // (event, details)

    void push(const std::string &event, const json &details)
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            events.emplace_back(event, details);
        }
        cv.notify_all();
    }

    bool wait_for(int count, int timeout_ms = 3000)
    {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return static_cast<int>(events.size()) >= count; });
    }

    int size()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(events.size());
    }

    json get_details(int index)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (index < static_cast<int>(events.size()))
            return events[index].second;
        return {};
    }

    std::string get_event(int index)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (index < static_cast<int>(events.size()))
            return events[index].first;
        return {};
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
            pylabhub::hub::GetLifecycleModule()), std::source_location::current());
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

    void TearDown() override
    {
        broker_.reset();
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> BrokerProtocolTest::s_lifecycle_;

// ============================================================================
// 1a. CHANNEL_BROADCAST_REQ → CHANNEL_BROADCAST_NOTIFY
// ============================================================================

TEST_F(BrokerProtocolTest, BroadcastReq_FansOutToProducer)
{
    const std::string channel = pid_chan("proto.bcast.prod");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    EventCollector prod_events;
    producer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { prod_events.push(event, details); });

    // Send broadcast from a separate Messenger (simulating another role).
    Messenger sender;
    ASSERT_TRUE(sender.connect(ep(), pk()));
    sender.enqueue_channel_broadcast(channel, "SENDER-UID", "hello", "world");

    ASSERT_TRUE(prod_events.wait_for(1))
        << "Producer did not receive broadcast within timeout";
    EXPECT_EQ(prod_events.get_event(0), "broadcast");
    EXPECT_EQ(prod_events.get_details(0).value("message", ""), "hello");
}

TEST_F(BrokerProtocolTest, BroadcastReq_FansOutToConsumer)
{
    const std::string channel = pid_chan("proto.bcast.cons");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

    EventCollector cons_events;
    consumer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { cons_events.push(event, details); });

    Messenger sender;
    ASSERT_TRUE(sender.connect(ep(), pk()));
    sender.enqueue_channel_broadcast(channel, "SENDER-UID", "hello", "world");

    ASSERT_TRUE(cons_events.wait_for(1))
        << "Consumer did not receive broadcast within timeout";
    EXPECT_EQ(cons_events.get_event(0), "broadcast");
    EXPECT_EQ(cons_events.get_details(0).value("message", ""), "hello");
}

TEST_F(BrokerProtocolTest, BroadcastReq_FansOutToAll)
{
    const std::string channel = pid_chan("proto.bcast.all");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

    EventCollector prod_events;
    EventCollector cons_events;
    producer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { prod_events.push(event, details); });
    consumer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { cons_events.push(event, details); });

    Messenger sender;
    ASSERT_TRUE(sender.connect(ep(), pk()));
    sender.enqueue_channel_broadcast(channel, "SENDER-UID", "hello", "world");

    ASSERT_TRUE(prod_events.wait_for(1))
        << "Producer did not receive broadcast within timeout";
    ASSERT_TRUE(cons_events.wait_for(1))
        << "Consumer did not receive broadcast within timeout";
    EXPECT_EQ(prod_events.get_event(0), "broadcast");
    EXPECT_EQ(cons_events.get_event(0), "broadcast");
}

TEST_F(BrokerProtocolTest, BroadcastReq_UnknownChannel_Silent)
{
    // Broadcast to a nonexistent channel — no crash, no delivery.
    Messenger sender;
    ASSERT_TRUE(sender.connect(ep(), pk()));
    EXPECT_NO_THROW(
        sender.enqueue_channel_broadcast(pid_chan("proto.bcast.bogus"), "UID", "msg"));

    // Give broker a poll cycle.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Broker still operational: listing channels should not crash.
    auto channels = sender.list_channels(2000);
    (void)channels;
}

TEST_F(BrokerProtocolTest, BroadcastReq_FieldsMatchSpec)
{
    const std::string channel = pid_chan("proto.bcast.fields");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    EventCollector prod_events;
    producer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { prod_events.push(event, details); });

    Messenger sender;
    ASSERT_TRUE(sender.connect(ep(), pk()));
    sender.enqueue_channel_broadcast(channel, "MY-SENDER-UID", "test_msg", "payload_data");

    ASSERT_TRUE(prod_events.wait_for(1));
    auto details = prod_events.get_details(0);

    EXPECT_EQ(details.value("channel_name", ""), channel);
    EXPECT_EQ(details.value("event", ""), "broadcast");
    EXPECT_EQ(details.value("sender_uid", ""), "MY-SENDER-UID");
    EXPECT_EQ(details.value("message", ""), "test_msg");
    EXPECT_EQ(details.value("data", ""), "payload_data");
}

// ============================================================================
// 1b. CHANNEL_NOTIFY_REQ → CHANNEL_EVENT_NOTIFY (producer-only)
// ============================================================================

TEST_F(BrokerProtocolTest, NotifyReq_DeliveredToProducerOnly)
{
    const std::string channel = pid_chan("proto.notify.prod_only");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

    EventCollector prod_events;
    EventCollector cons_events;
    producer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { prod_events.push(event, details); });
    consumer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { cons_events.push(event, details); });

    // Consumer sends notify targeting the producer.
    consumer.enqueue_channel_notify(channel, "CONS-UID", "test_ping", "ping_data");

    ASSERT_TRUE(prod_events.wait_for(1))
        << "Producer did not receive notify event within timeout";
    EXPECT_EQ(prod_events.get_event(0), "test_ping");
    EXPECT_EQ(prod_events.get_details(0).value("sender_uid", ""), "CONS-UID");
    EXPECT_EQ(prod_events.get_details(0).value("data", ""), "ping_data");

    // Consumer should NOT have received anything.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(cons_events.size(), 0) << "Consumer should not receive CHANNEL_EVENT_NOTIFY";
}

TEST_F(BrokerProtocolTest, NotifyReq_EventFieldCorrect)
{
    const std::string channel = pid_chan("proto.notify.event_field");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    EventCollector prod_events;
    producer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { prod_events.push(event, details); });

    Messenger sender;
    ASSERT_TRUE(sender.connect(ep(), pk()));
    sender.enqueue_channel_notify(channel, "SENDER-UID", "custom_event_name");

    ASSERT_TRUE(prod_events.wait_for(1));
    EXPECT_EQ(prod_events.get_event(0), "custom_event_name");
    EXPECT_EQ(prod_events.get_details(0).value("channel_name", ""), channel);
}

TEST_F(BrokerProtocolTest, NotifyReq_UnknownChannel_Silent)
{
    Messenger sender;
    ASSERT_TRUE(sender.connect(ep(), pk()));
    EXPECT_NO_THROW(
        sender.enqueue_channel_notify(pid_chan("proto.notify.bogus"), "UID", "event"));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Broker still operational.
    auto channels = sender.list_channels(2000);
    (void)channels;
}

// ============================================================================
// 1c. CHANNEL_LIST_REQ/ACK via Messenger
// ============================================================================

TEST_F(BrokerProtocolTest, ListChannels_ViaMessenger_Empty)
{
    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));

    auto channels = m.list_channels(3000);
    EXPECT_TRUE(channels.empty()) << "Expected empty channel list";
}

TEST_F(BrokerProtocolTest, ListChannels_ViaMessenger_OneChannel)
{
    const std::string channel = pid_chan("proto.list.one");

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));
    auto handle = m.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    auto channels = m.list_channels(3000);
    ASSERT_GE(channels.size(), 1u);

    bool found = false;
    for (const auto &ch : channels)
    {
        if (ch.value("name", "") == channel)
        {
            found = true;
            EXPECT_TRUE(ch.contains("status"));
            EXPECT_TRUE(ch.contains("consumer_count"));
            EXPECT_EQ(ch.value("consumer_count", -1), 0);
            break;
        }
    }
    EXPECT_TRUE(found) << "Channel not in list_channels() result";
}

TEST_F(BrokerProtocolTest, ListChannels_ViaMessenger_WithConsumer)
{
    const std::string channel = pid_chan("proto.list.with_cons");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

    auto channels = producer.list_channels(3000);
    ASSERT_GE(channels.size(), 1u);

    bool found = false;
    for (const auto &ch : channels)
    {
        if (ch.value("name", "") == channel)
        {
            found = true;
            EXPECT_EQ(ch.value("consumer_count", -1), 1);
            break;
        }
    }
    EXPECT_TRUE(found) << "Channel not in list_channels() result";
}

// ============================================================================
// 1d. Admin request_broadcast_channel (broker internal queue)
// ============================================================================

TEST_F(BrokerProtocolTest, AdminBroadcast_DeliveredViaInternalQueue)
{
    const std::string channel = pid_chan("proto.admin.bcast");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    EventCollector prod_events;
    producer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { prod_events.push(event, details); });

    // Use the admin API (thread-safe internal queue path).
    svc().request_broadcast_channel(channel, "admin_test_msg", "admin_data");

    ASSERT_TRUE(prod_events.wait_for(1))
        << "Producer did not receive admin broadcast within timeout";
    EXPECT_EQ(prod_events.get_event(0), "broadcast");
    EXPECT_EQ(prod_events.get_details(0).value("sender_uid", ""), "admin_shell");
    EXPECT_EQ(prod_events.get_details(0).value("message", ""), "admin_test_msg");
    EXPECT_EQ(prod_events.get_details(0).value("data", ""), "admin_data");
}

// ============================================================================
// 2. CHECKSUM_ERROR_REPORT — broker forwards as CHANNEL_EVENT_NOTIFY
// ============================================================================

TEST_F(BrokerProtocolTest, ChecksumErrorReport_ForwardedToProducer)
{
    // Need NotifyOnly policy for broker to forward checksum reports.
    broker_.reset(); // tear down default broker
    BrokerService::Config cfg;
    cfg.endpoint                = "tcp://127.0.0.1:0";
    cfg.schema_search_dirs      = {};
    cfg.checksum_repair_policy  = ChecksumRepairPolicy::NotifyOnly;
    broker_.emplace(start_local_broker(std::move(cfg)));

    const std::string channel = pid_chan("proto.checksum.prod");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    EventCollector prod_events;
    producer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { prod_events.push(event, details); });

    // Consumer reports a checksum error.
    Messenger reporter;
    ASSERT_TRUE(reporter.connect(ep(), pk()));
    reporter.report_checksum_error(channel, 42, "bad CRC in slot 42");

    ASSERT_TRUE(prod_events.wait_for(1))
        << "Producer did not receive checksum error notify";
    auto details = prod_events.get_details(0);
    EXPECT_EQ(details.value("slot_index", -1), 42);
    EXPECT_EQ(details.value("error", ""), "bad CRC in slot 42");
    EXPECT_TRUE(details.contains("reporter_pid"));
    EXPECT_EQ(details.value("broker_action", ""), "notify_only");
}

TEST_F(BrokerProtocolTest, ChecksumErrorReport_ForwardedToConsumer)
{
    broker_.reset();
    BrokerService::Config cfg;
    cfg.endpoint                = "tcp://127.0.0.1:0";
    cfg.schema_search_dirs      = {};
    cfg.checksum_repair_policy  = ChecksumRepairPolicy::NotifyOnly;
    broker_.emplace(start_local_broker(std::move(cfg)));

    const std::string channel = pid_chan("proto.checksum.cons");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

    EventCollector cons_events;
    consumer.on_channel_error(channel,
                              [&](std::string event, json details)
                              { cons_events.push(event, details); });

    // A third party reports a checksum error.
    Messenger reporter;
    ASSERT_TRUE(reporter.connect(ep(), pk()));
    reporter.report_checksum_error(channel, 7, "checksum mismatch slot 7");

    ASSERT_TRUE(cons_events.wait_for(1))
        << "Consumer did not receive checksum error notify";
    auto details = cons_events.get_details(0);
    EXPECT_EQ(details.value("slot_index", -1), 7);
    EXPECT_EQ(details.value("error", ""), "checksum mismatch slot 7");
    EXPECT_EQ(details.value("broker_action", ""), "notify_only");
}

TEST_F(BrokerProtocolTest, ChecksumErrorReport_UnknownChannel_Silent)
{
    // Reporting a checksum error for a nonexistent channel should not crash.
    Messenger reporter;
    ASSERT_TRUE(reporter.connect(ep(), pk()));
    EXPECT_NO_THROW(
        reporter.report_checksum_error(pid_chan("proto.checksum.bogus"), 0, "test"));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Broker still operational.
    auto channels = reporter.list_channels(2000);
    (void)channels;
}

// ============================================================================
// 3. CHANNEL_CLOSING_NOTIFY — delivery to ALL channel members
// ============================================================================

TEST_F(BrokerProtocolTest, ClosingNotify_DeliveredToProducerAndConsumers)
{
    const std::string channel = pid_chan("proto.close.all");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Register two consumers.
    Messenger consumer1, consumer2;
    ASSERT_TRUE(consumer1.connect(ep(), pk()));
    ASSERT_TRUE(consumer2.connect(ep(), pk()));
    auto cons1_handle = consumer1.connect_channel(channel, /*timeout_ms=*/3000);
    auto cons2_handle = consumer2.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons1_handle.has_value());
    ASSERT_TRUE(cons2_handle.has_value());

    // Install per-channel closing callbacks on all three.
    std::atomic<int> prod_closing{0}, cons1_closing{0}, cons2_closing{0};
    producer.on_channel_closing(channel, [&] { prod_closing.fetch_add(1); });
    consumer1.on_channel_closing(channel, [&] { cons1_closing.fetch_add(1); });
    consumer2.on_channel_closing(channel, [&] { cons2_closing.fetch_add(1); });

    // Admin close the channel.
    svc().request_close_channel(channel);

    // Wait for all three to receive CHANNEL_CLOSING_NOTIFY.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (prod_closing.load() > 0 && cons1_closing.load() > 0 && cons2_closing.load() > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_GE(prod_closing.load(), 1) << "Producer did not receive CHANNEL_CLOSING_NOTIFY";
    EXPECT_GE(cons1_closing.load(), 1) << "Consumer 1 did not receive CHANNEL_CLOSING_NOTIFY";
    EXPECT_GE(cons2_closing.load(), 1) << "Consumer 2 did not receive CHANNEL_CLOSING_NOTIFY";
}

TEST_F(BrokerProtocolTest, ClosingNotify_ChannelRemovedFromList)
{
    const std::string channel = pid_chan("proto.close.removed");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    // Verify channel is in the list.
    auto channels_before = producer.list_channels(3000);
    bool found_before = false;
    for (const auto &ch : channels_before)
        if (ch.value("name", "") == channel) found_before = true;
    ASSERT_TRUE(found_before) << "Channel not in list_channels() before close";

    std::atomic<bool> closing_received{false};
    producer.on_channel_closing(channel, [&] { closing_received.store(true); });

    svc().request_close_channel(channel);

    // Wait for closing notify.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!closing_received.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    ASSERT_TRUE(closing_received.load()) << "Producer did not receive CHANNEL_CLOSING_NOTIFY";

    // Give broker a cycle to clean up.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Channel should no longer appear in the list.
    auto channels_after = producer.list_channels(3000);
    bool found_after = false;
    for (const auto &ch : channels_after)
        if (ch.value("name", "") == channel) found_after = true;
    EXPECT_FALSE(found_after) << "Channel should be removed from list after close";
}

// ============================================================================
// 4. Duplicate REG_REQ — SCHEMA_MISMATCH error + CHANNEL_ERROR_NOTIFY
// ============================================================================

TEST_F(BrokerProtocolTest, DuplicateReg_SameSchemaHash_Succeeds)
{
    const std::string channel  = pid_chan("proto.dup.same");
    const std::string hash_hex = "aabbccdd00112233445566778899aabbccddeeff"
                                 "00112233445566778899aabbccddeeff";

    Messenger m1;
    ASSERT_TRUE(m1.connect(ep(), pk()));
    auto h1 = m1.create_channel(channel, {.schema_hash = hash_hex, .timeout_ms = 3000});
    ASSERT_TRUE(h1.has_value());

    // Second registration with same hash — should succeed (update).
    Messenger m2;
    ASSERT_TRUE(m2.connect(ep(), pk()));
    auto h2 = m2.create_channel(channel, {.schema_hash = hash_hex, .timeout_ms = 3000});
    ASSERT_TRUE(h2.has_value()) << "Same schema hash re-registration should succeed";
}

TEST_F(BrokerProtocolTest, DuplicateReg_DifferentSchemaHash_Rejected)
{
    const std::string channel = pid_chan("proto.dup.diff");
    const std::string hash_a  = "aabbccdd00112233445566778899aabbccddeeff"
                                "00112233445566778899aabbccddeeff";
    const std::string hash_b  = "11223344556677889900aabbccddeeff11223344"
                                "556677889900aabbccddeeff11223344";

    Messenger m1;
    ASSERT_TRUE(m1.connect(ep(), pk()));
    auto h1 = m1.create_channel(channel, {.schema_hash = hash_a, .timeout_ms = 3000});
    ASSERT_TRUE(h1.has_value());

    // Collect error notifications on the original producer.
    EventCollector m1_events;
    m1.on_channel_error(channel,
                        [&](std::string event, json details)
                        { m1_events.push(event, details); });

    // Second registration with DIFFERENT hash — should be rejected.
    Messenger m2;
    ASSERT_TRUE(m2.connect(ep(), pk()));
    auto h2 = m2.create_channel(channel, {.schema_hash = hash_b, .timeout_ms = 3000});
    EXPECT_FALSE(h2.has_value())
        << "Different schema hash re-registration should be rejected";

    // Original producer should receive CHANNEL_ERROR_NOTIFY about the schema mismatch.
    ASSERT_TRUE(m1_events.wait_for(1, 3000))
        << "Original producer did not receive schema mismatch notification";
    auto details = m1_events.get_details(0);
    EXPECT_TRUE(details.contains("attempted_schema_hash")
                || details.contains("existing_schema_hash"))
        << "Error notification should contain schema hash info";
}

// ============================================================================
// 5. HEARTBEAT_REQ — PendingReady → Ready status transition
// ============================================================================

TEST_F(BrokerProtocolTest, Heartbeat_TransitionsToReady)
{
    const std::string channel = pid_chan("proto.heartbeat.ready");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    // After registration + immediate heartbeat, channel should be "Ready".
    // The Messenger sends an immediate heartbeat after REG_ACK.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto channels = producer.list_channels(3000);
    ASSERT_GE(channels.size(), 1u);

    bool found = false;
    for (const auto &ch : channels)
    {
        if (ch.value("name", "") == channel)
        {
            found = true;
            EXPECT_EQ(ch.value("status", ""), "Ready")
                << "Channel should be 'Ready' after heartbeat";
            break;
        }
    }
    EXPECT_TRUE(found) << "Channel not found in list_channels()";
}

TEST_F(BrokerProtocolTest, Heartbeat_ExplicitSend_UpdatesTimestamp)
{
    const std::string channel = pid_chan("proto.heartbeat.ts");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value());

    // The Messenger has already sent the immediate heartbeat after REG.
    // Suppress periodic heartbeats so we can control timing.
    producer.suppress_periodic_heartbeat(channel);

    // Wait a bit, then send explicit heartbeat.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    producer.enqueue_heartbeat(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Channel should still be "Ready" (heartbeat keeps it alive).
    auto channels = producer.list_channels(3000);
    bool found = false;
    for (const auto &ch : channels)
    {
        if (ch.value("name", "") == channel)
        {
            found = true;
            EXPECT_EQ(ch.value("status", ""), "Ready");
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ============================================================================
// 6. HELLO/BYE — Peer-to-peer handshake via hub::Producer/Consumer
// ============================================================================

TEST_F(BrokerProtocolTest, PeerHandshake_HelloTriggersConsumerJoined)
{
    const std::string channel = pid_chan("proto.hello.joined");

    Messenger prod_messenger;
    ASSERT_TRUE(prod_messenger.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name = channel;
    opts.pattern      = ChannelPattern::PubSub;
    opts.has_shm      = false;
    opts.actor_name   = "test-producer";
    opts.actor_uid    = "TEST-PROD-001";

    auto maybe_producer = Producer::create(prod_messenger, opts);
    ASSERT_TRUE(maybe_producer.has_value());
    auto &prod = *maybe_producer;

    std::atomic<int> join_count{0};
    prod.on_consumer_joined([&](const std::string &/*identity*/) {
        join_count.fetch_add(1);
    });

    ASSERT_TRUE(prod.start_embedded());

    // Consumer connects — triggers HELLO.
    Messenger cons_messenger;
    ASSERT_TRUE(cons_messenger.connect(ep(), pk()));

    ConsumerOptions copts;
    copts.channel_name = channel;
    copts.consumer_uid = "TEST-CONS-001";
    copts.consumer_name = "test-consumer";

    auto maybe_consumer = Consumer::connect(cons_messenger, copts);
    ASSERT_TRUE(maybe_consumer.has_value());
    auto &cons = *maybe_consumer;
    ASSERT_TRUE(cons.start_embedded());

    // Wait for HELLO to be processed.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (join_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(join_count.load(), 1) << "on_consumer_joined not triggered by HELLO";

    cons.stop();
    cons.close();
    prod.stop();
    prod.close();
}

TEST_F(BrokerProtocolTest, PeerHandshake_ByeTriggersConsumerLeft)
{
    const std::string channel = pid_chan("proto.bye.left");

    Messenger prod_messenger;
    ASSERT_TRUE(prod_messenger.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name = channel;
    opts.pattern      = ChannelPattern::PubSub;
    opts.has_shm      = false;
    opts.actor_name   = "test-producer";
    opts.actor_uid    = "TEST-PROD-002";

    auto maybe_producer = Producer::create(prod_messenger, opts);
    ASSERT_TRUE(maybe_producer.has_value());
    auto &prod = *maybe_producer;

    std::atomic<int> join_count{0}, left_count{0};
    prod.on_consumer_joined([&](const std::string &/*identity*/) {
        join_count.fetch_add(1);
    });
    prod.on_consumer_left([&](const std::string &/*identity*/) {
        left_count.fetch_add(1);
    });

    ASSERT_TRUE(prod.start_embedded());

    // Consumer connects and then disconnects.
    Messenger cons_messenger;
    ASSERT_TRUE(cons_messenger.connect(ep(), pk()));

    ConsumerOptions copts;
    copts.channel_name  = channel;
    copts.consumer_uid  = "TEST-CONS-002";
    copts.consumer_name = "test-consumer-2";

    auto maybe_consumer = Consumer::connect(cons_messenger, copts);
    ASSERT_TRUE(maybe_consumer.has_value());
    auto &cons = *maybe_consumer;
    ASSERT_TRUE(cons.start_embedded());

    // Wait for HELLO.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (join_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(join_count.load(), 1) << "HELLO not received";

    // Consumer disconnects — triggers BYE.
    cons.stop();
    cons.close();

    // Wait for BYE.
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (left_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(left_count.load(), 1) << "on_consumer_left not triggered by BYE";

    prod.stop();
    prod.close();
}

TEST_F(BrokerProtocolTest, PeerHandshake_MultipleConsumers_IndependentHelloBye)
{
    const std::string channel = pid_chan("proto.hello.multi");

    Messenger prod_messenger;
    ASSERT_TRUE(prod_messenger.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name = channel;
    opts.pattern      = ChannelPattern::PubSub;
    opts.has_shm      = false;
    opts.actor_name   = "test-producer";
    opts.actor_uid    = "TEST-PROD-003";

    auto maybe_producer = Producer::create(prod_messenger, opts);
    ASSERT_TRUE(maybe_producer.has_value());
    auto &prod = *maybe_producer;

    std::atomic<int> join_count{0}, left_count{0};
    prod.on_consumer_joined([&](const std::string &/*identity*/) {
        join_count.fetch_add(1);
    });
    prod.on_consumer_left([&](const std::string &/*identity*/) {
        left_count.fetch_add(1);
    });

    ASSERT_TRUE(prod.start_embedded());

    // Connect two consumers.
    Messenger cm1, cm2;
    ASSERT_TRUE(cm1.connect(ep(), pk()));
    ASSERT_TRUE(cm2.connect(ep(), pk()));

    ConsumerOptions co1;
    co1.channel_name  = channel;
    co1.consumer_uid  = "TEST-CONS-003A";
    co1.consumer_name = "consumer-a";

    ConsumerOptions co2;
    co2.channel_name  = channel;
    co2.consumer_uid  = "TEST-CONS-003B";
    co2.consumer_name = "consumer-b";

    auto mc1 = Consumer::connect(cm1, co1);
    auto mc2 = Consumer::connect(cm2, co2);
    ASSERT_TRUE(mc1.has_value());
    ASSERT_TRUE(mc2.has_value());
    auto &c1 = *mc1;
    auto &c2 = *mc2;
    ASSERT_TRUE(c1.start_embedded());
    ASSERT_TRUE(c2.start_embedded());

    // Wait for both HELLOs.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (join_count.load() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(join_count.load(), 2) << "Expected 2 HELLO events";

    // Disconnect first consumer.
    c1.stop();
    c1.close();

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (left_count.load() < 1 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(left_count.load(), 1) << "Expected 1 BYE event (first consumer)";

    // Disconnect second consumer.
    c2.stop();
    c2.close();

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (left_count.load() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(left_count.load(), 2) << "Expected 2 BYE events total";

    prod.stop();
    prod.close();
}

// ============================================================================
// Phase 4: ROLE_PRESENCE_REQ + ROLE_INFO_REQ
// ============================================================================

// 4a. Unknown UID — presence returns false, info returns nullopt.
TEST_F(BrokerProtocolTest, RolePresenceReq_UnknownUid_ReturnsFalse)
{
    Messenger querier;
    ASSERT_TRUE(querier.connect(ep(), pk()));
    EXPECT_FALSE(querier.query_role_presence("PROD-UNKNOWN-DEADBEEF", /*timeout_ms=*/2000));
}

TEST_F(BrokerProtocolTest, RoleInfoReq_UnknownUid_ReturnsNullopt)
{
    Messenger querier;
    ASSERT_TRUE(querier.connect(ep(), pk()));
    EXPECT_EQ(querier.query_role_info("PROD-UNKNOWN-DEADBEEF", /*timeout_ms=*/2000),
              std::nullopt);
}

// 4b. Producer UID registered — presence returns true.
TEST_F(BrokerProtocolTest, RolePresenceReq_ProducerUid_ReturnsTrue)
{
    const std::string channel = pid_chan("proto.presence.prod");
    const std::string uid     = "PROD-PRESTEST-AAAA0001";

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(
        channel, {.timeout_ms = 3000, .actor_name = "PresTestProd", .actor_uid = uid});
    ASSERT_TRUE(handle.has_value());

    Messenger querier;
    ASSERT_TRUE(querier.connect(ep(), pk()));
    EXPECT_TRUE(querier.query_role_presence(uid, /*timeout_ms=*/2000));
}

// 4c. Consumer UID registered — presence returns true.
TEST_F(BrokerProtocolTest, RolePresenceReq_ConsumerUid_ReturnsTrue)
{
    const std::string channel      = pid_chan("proto.presence.cons");
    const std::string consumer_uid = "CONS-PRESTEST-BBBB0002";

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(
        channel, /*timeout_ms=*/3000, /*schema_hash=*/{}, consumer_uid);
    ASSERT_TRUE(cons_handle.has_value());

    Messenger querier;
    ASSERT_TRUE(querier.connect(ep(), pk()));
    EXPECT_TRUE(querier.query_role_presence(consumer_uid, /*timeout_ms=*/2000));
}

// 4d. Producer has no inbox — role_info returns nullopt.
TEST_F(BrokerProtocolTest, RoleInfoReq_NoInbox_ReturnsNullopt)
{
    const std::string channel = pid_chan("proto.roleinfo.noinbox");
    const std::string uid     = "PROD-ROLEINFO-CCCC0003";

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto handle = producer.create_channel(
        channel, {.timeout_ms = 3000, .actor_name = "NoInboxProd", .actor_uid = uid});
    ASSERT_TRUE(handle.has_value());

    Messenger querier;
    ASSERT_TRUE(querier.connect(ep(), pk()));
    EXPECT_EQ(querier.query_role_info(uid, /*timeout_ms=*/2000), std::nullopt);
}

// 4e. Producer has inbox configured — role_info returns endpoint/schema/packing.
TEST_F(BrokerProtocolTest, RoleInfoReq_WithInbox_ReturnsInfo)
{
    const std::string channel      = pid_chan("proto.roleinfo.withinbox");
    const std::string uid          = "PROD-ROLEINFO-DDDD0004";
    const std::string inbox_ep     = "tcp://127.0.0.1:9987";
    const std::string schema_json  = R"([{"type":"float64","count":1,"length":0}])";
    const std::string packing      = "aligned";

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    ChannelRegistrationOptions opts;
    opts.timeout_ms        = 3000;
    opts.actor_uid         = uid;
    opts.actor_name        = "InboxProd";
    opts.inbox_endpoint    = inbox_ep;
    opts.inbox_schema_json = schema_json;
    opts.inbox_packing     = packing;
    auto handle = producer.create_channel(channel, opts);
    ASSERT_TRUE(handle.has_value());

    Messenger querier;
    ASSERT_TRUE(querier.connect(ep(), pk()));
    auto info = querier.query_role_info(uid, /*timeout_ms=*/2000);
    ASSERT_TRUE(info.has_value()) << "Expected RoleInfoResult, got nullopt";
    EXPECT_EQ(info->inbox_endpoint, inbox_ep);
    EXPECT_EQ(info->inbox_packing, packing);
    ASSERT_TRUE(info->inbox_schema.is_array());
    ASSERT_EQ(info->inbox_schema.size(), 1u);
    EXPECT_EQ(info->inbox_schema[0].value("type", ""), "float64");
    EXPECT_EQ(info->inbox_schema[0].value("count", 0u), 1u);
}

// ── Transport arbitration (Phase 6) ─────────────────────────────────────────

TEST_F(BrokerProtocolTest, TransportMismatch_ShmChannel_ZmqConsumer_Fails)
{
    // SHM channel (default) + consumer declares "zmq" → broker rejects.
    const std::string channel = pid_chan("proto.transport.shm_zmq");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    // Pass consumer_queue_type="zmq" explicitly; channel is SHM → mismatch.
    auto cons_handle = consumer.connect_channel(
        channel, /*timeout_ms=*/3000, {}, {}, {}, {}, "zmq");
    EXPECT_FALSE(cons_handle.has_value())
        << "connect_channel should fail: consumer wants ZMQ but channel uses SHM";
}

TEST_F(BrokerProtocolTest, TransportMatch_ShmConsumer_ShmChannel_Succeeds)
{
    // SHM channel + consumer declares "shm" → accepted.
    const std::string channel = pid_chan("proto.transport.shm_shm");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(
        channel, /*timeout_ms=*/3000, {}, {}, {}, {}, "shm");
    EXPECT_TRUE(cons_handle.has_value())
        << "connect_channel should succeed: both sides use SHM";
}

TEST_F(BrokerProtocolTest, TransportMatch_NoDriverField_AlwaysSucceeds)
{
    // When consumer_queue_type is empty, broker skips validation.
    const std::string channel = pid_chan("proto.transport.nofield");

    Messenger producer;
    ASSERT_TRUE(producer.connect(ep(), pk()));
    auto prod_handle = producer.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    Messenger consumer;
    ASSERT_TRUE(consumer.connect(ep(), pk()));
    auto cons_handle = consumer.connect_channel(channel, /*timeout_ms=*/3000);
    EXPECT_TRUE(cons_handle.has_value())
        << "connect_channel should succeed when consumer_queue_type is omitted";
}

// ── CR-005: Embedded-mode peer-dead detection ─────────────────────────────────
//
// Validates that on_peer_dead fires through the handle_peer_events_nowait() path
// (embedded mode — no internal peer thread). The consumer sends HELLO on connect()
// but sends no further messages, so the producer declares peer-dead after
// peer_dead_timeout_ms has elapsed.

TEST_F(BrokerProtocolTest, EmbeddedMode_Producer_PeerDead_FiresViaHandlePeerEvents)
{
    const std::string channel = pid_chan("proto.embedded.peer_dead.prod");

    Messenger prod_messenger;
    ASSERT_TRUE(prod_messenger.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name        = channel;
    opts.pattern             = ChannelPattern::PubSub;
    opts.has_shm             = false;
    opts.actor_name          = "test-producer-peerdead";
    opts.actor_uid           = "TEST-PROD-PD-001";
    opts.peer_dead_timeout_ms = 100; // Short timeout for fast test

    auto maybe_producer = Producer::create(prod_messenger, opts);
    ASSERT_TRUE(maybe_producer.has_value());
    auto &prod = *maybe_producer;

    std::atomic<int> join_count{0};
    std::atomic<int> dead_count{0};

    prod.on_consumer_joined([&](const std::string &) { join_count.fetch_add(1); });
    prod.on_peer_dead([&]() { dead_count.fetch_add(1); });

    ASSERT_TRUE(prod.start_embedded());

    // Consumer connects — sends HELLO once during connect().
    Messenger cons_messenger;
    ASSERT_TRUE(cons_messenger.connect(ep(), pk()));

    ConsumerOptions copts;
    copts.channel_name  = channel;
    copts.consumer_uid  = "TEST-CONS-PD-001";
    copts.consumer_name = "test-consumer-peerdead";
    // shm_shared_secret defaults to 0 → no SHM

    auto maybe_consumer = Consumer::connect(cons_messenger, copts);
    ASSERT_TRUE(maybe_consumer.has_value());
    auto &cons = *maybe_consumer;
    ASSERT_TRUE(cons.start_embedded());

    // Phase 1: drive producer until HELLO is processed (peer_ever_seen_ = true).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (join_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(join_count.load(), 1) << "HELLO not processed by handle_peer_events_nowait";

    // Phase 2: stop driving consumer — no more HELLOs. Keep driving producer.
    // After peer_dead_timeout_ms (100ms), peer-dead should fire.
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (dead_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(dead_count.load(), 1)
        << "on_peer_dead not fired via handle_peer_events_nowait after "
           "peer_dead_timeout_ms elapsed";

    cons.stop();
    cons.close();
    prod.stop();
    prod.close();
}

TEST_F(BrokerProtocolTest, EmbeddedMode_Consumer_PeerDead_FiresViaHandleCtrlEvents)
{
    const std::string channel = pid_chan("proto.embedded.peer_dead.cons");

    // Producer: SHM-less channel.
    Messenger prod_messenger;
    ASSERT_TRUE(prod_messenger.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name = channel;
    opts.pattern      = ChannelPattern::PubSub;
    opts.has_shm      = false;
    opts.actor_name   = "test-producer-cpd";
    opts.actor_uid    = "TEST-PROD-CPD-001";

    auto maybe_producer = Producer::create(prod_messenger, opts);
    ASSERT_TRUE(maybe_producer.has_value());
    auto &prod = *maybe_producer;

    // Capture consumer identity when HELLO arrives so we can send ctrl back.
    std::string consumer_identity;
    std::atomic<int> join_count{0};
    prod.on_consumer_joined([&](const std::string &id) {
        consumer_identity = id;
        join_count.fetch_add(1);
    });
    ASSERT_TRUE(prod.start_embedded());

    // Consumer: very short peer_dead_timeout_ms.
    Messenger cons_messenger;
    ASSERT_TRUE(cons_messenger.connect(ep(), pk()));

    ConsumerOptions copts;
    copts.channel_name         = channel;
    copts.consumer_uid         = "TEST-CONS-CPD-001";
    copts.consumer_name        = "test-consumer-cpd";
    copts.peer_dead_timeout_ms = 100;
    // shm_shared_secret defaults to 0 → no SHM

    auto maybe_consumer = Consumer::connect(cons_messenger, copts);
    ASSERT_TRUE(maybe_consumer.has_value());
    auto &cons = *maybe_consumer;

    std::atomic<int> dead_count{0};
    cons.on_peer_dead([&]() { dead_count.fetch_add(1); });

    ASSERT_TRUE(cons.start_embedded());

    // Phase 1: drive producer until HELLO is processed → capture consumer identity.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (join_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        cons.handle_ctrl_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(join_count.load(), 1) << "Consumer HELLO not received by producer";
    ASSERT_FALSE(consumer_identity.empty()) << "Consumer identity not captured";

    // Phase 2: producer sends one ctrl message to consumer → consumer sees peer contact.
    const char ping[] = "ping";
    ASSERT_TRUE(prod.send_ctrl(consumer_identity, "PING", ping, sizeof(ping) - 1));

    // Drive both sides to deliver the ctrl frame.
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline)
    {
        prod.handle_peer_events_nowait();
        cons.handle_ctrl_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Phase 3: stop driving producer → no more ctrl messages. Keep driving consumer.
    // After peer_dead_timeout_ms (100ms), consumer should declare peer dead.
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (dead_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        cons.handle_ctrl_events_nowait();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(dead_count.load(), 1)
        << "on_peer_dead not fired on consumer via handle_ctrl_events_nowait after "
           "peer_dead_timeout_ms elapsed";

    cons.stop();
    cons.close();
    prod.stop();
    prod.close();
}
