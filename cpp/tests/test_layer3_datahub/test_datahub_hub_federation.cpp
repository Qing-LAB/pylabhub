/**
 * @file test_datahub_hub_federation.cpp
 * @brief Hub Federation Broadcast protocol tests (HEP-CORE-0022).
 *
 * Suite: BrokerFederationTest
 *
 * Tests the federation ctrl-plane relay between two in-process brokers:
 *   - HUB_PEER_HELLO / HUB_PEER_HELLO_ACK handshake (on_hub_connected)
 *   - CHANNEL_NOTIFY_REQ relayed as CHANNEL_EVENT_NOTIFY (relay=true) with originator_uid
 *   - CHANNEL_BROADCAST_REQ relayed across hubs to Hub B subscribers
 *   - HUB_TARGETED_MSG delivery to on_hub_message callback
 *   - Channel-filtered relay: channels not in relay list are not forwarded
 *   - HUB_PEER_BYE triggers on_hub_disconnected on the peer
 *
 * All tests use the in-process LocalBrokerHandle pattern (no subprocess workers).
 * Topology: Hub B connects outbound to Hub A. Hub A is the "server" (ROUTER).
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
    auto promise    = std::make_shared<std::promise<ReadyInfo>>();
    auto future     = promise->get_future();

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

// ── EventCollector: captures on_channel_error callback data ──────────────────

struct EventCollector
{
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, json>> events;

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

// ── HubEventCollector: captures federation lifecycle callbacks ────────────────

struct HubEventCollector
{
    struct HubEvent
    {
        std::string type;    ///< "connected" | "disconnected" | "message"
        std::string hub_uid; ///< source hub UID
        std::string channel; ///< channel (message events)
        std::string payload; ///< payload (message events)
    };

    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<HubEvent>   events;

    void push_connected(const std::string &hub_uid)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            events.push_back({"connected", hub_uid, {}, {}});
        }
        cv.notify_all();
    }

    void push_disconnected(const std::string &hub_uid)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            events.push_back({"disconnected", hub_uid, {}, {}});
        }
        cv.notify_all();
    }

    void push_message(const std::string &channel, const std::string &payload,
                      const std::string &src_uid)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            events.push_back({"message", src_uid, channel, payload});
        }
        cv.notify_all();
    }

    bool wait_for(int count, int timeout_ms = 3000)
    {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return static_cast<int>(events.size()) >= count; });
    }

    int size()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return static_cast<int>(events.size());
    }

    HubEvent get(int index)
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (index < static_cast<int>(events.size()))
            return events[index];
        return {};
    }

    bool has_event(const std::string &type, const std::string &hub_uid)
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto &e : events)
            if (e.type == type && e.hub_uid == hub_uid)
                return true;
        return false;
    }
};

} // anonymous namespace

// ============================================================================
// BrokerFederationTest fixture
// ============================================================================

class BrokerFederationTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(), pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetLifecycleModule()));
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> BrokerFederationTest::s_lifecycle_;

// ============================================================================
// Test 1: HUB_PEER_HELLO / HUB_PEER_HELLO_ACK handshake
// ============================================================================

TEST_F(BrokerFederationTest, HelloHandshake_FiresOnHubConnected)
{
    // Hub B connects outbound to Hub A.
    // Hub A fires on_hub_connected when HELLO arrives.
    // Hub B fires on_hub_connected when ACK arrives.

    HubEventCollector hub_a_events;
    HubEventCollector hub_b_events;

    // Start Hub A — no outbound peers; just wires on_hub_connected callback.
    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_a.self_hub_uid           = "HUB-TEST-A";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };

    auto hub_a = start_local_broker(std::move(cfg_a));

    // Start Hub B with Hub A as outbound peer.
    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_b.self_hub_uid           = "HUB-TEST-B";

    FederationPeer peer_a;
    peer_a.hub_uid         = "HUB-TEST-A";
    peer_a.broker_endpoint = hub_a.endpoint;
    peer_a.pubkey_z85      = hub_a.pubkey; // required for CURVE auth on DEALER→ROUTER
    cfg_b.peers.push_back(std::move(peer_a));

    cfg_b.on_hub_connected = [&](const std::string &uid)
    { hub_b_events.push_connected(uid); };

    auto hub_b = start_local_broker(std::move(cfg_b));

    // Hub A fires connected when it receives HELLO from Hub B.
    ASSERT_TRUE(hub_a_events.wait_for(1, 3000))
        << "Hub A did not fire on_hub_connected within timeout";
    EXPECT_EQ(hub_a_events.get(0).hub_uid, "HUB-TEST-B");

    // Hub B fires connected when it receives ACK from Hub A.
    ASSERT_TRUE(hub_b_events.wait_for(1, 3000))
        << "Hub B did not fire on_hub_connected within timeout";
    EXPECT_EQ(hub_b_events.get(0).hub_uid, "HUB-TEST-A");

    hub_b.stop_and_join();
    hub_a.stop_and_join();
}

// ============================================================================
// Test 2: CHANNEL_NOTIFY_REQ relayed across hubs
// ============================================================================

TEST_F(BrokerFederationTest, RelayChannelNotify_DeliveredToHubB)
{
    // Topology:
    //   Consumer C_A sends CHANNEL_NOTIFY_REQ to Hub A.
    //   Hub A delivers locally to P_A (producer on Hub A).
    //   Hub A also relays HUB_RELAY_MSG to Hub B (Hub B is an inbound peer
    //   subscribed to this channel).
    //   Hub B delivers as CHANNEL_EVENT_NOTIFY to P_B (producer on Hub B),
    //   with an extra `originator_uid` field identifying Hub A.

    const std::string channel = pid_chan("fed.relay.notify");

    HubEventCollector hub_a_events;

    // Hub A: configure Hub B uid + relay channel (no outbound connection needed).
    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_a.self_hub_uid           = "HUB-RELAY-A";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };

    FederationPeer peer_b_on_a;
    peer_b_on_a.hub_uid  = "HUB-RELAY-B";
    peer_b_on_a.channels = {channel};
    cfg_a.peers.push_back(std::move(peer_b_on_a));

    auto hub_a = start_local_broker(std::move(cfg_a));

    // Hub B: outbound connection to Hub A.
    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_b.self_hub_uid           = "HUB-RELAY-B";

    FederationPeer peer_a_on_b;
    peer_a_on_b.hub_uid         = "HUB-RELAY-A";
    peer_a_on_b.broker_endpoint = hub_a.endpoint;
    peer_a_on_b.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a_on_b));

    auto hub_b = start_local_broker(std::move(cfg_b));

    // Wait for Hub A to register Hub B as an inbound peer (handshake complete).
    ASSERT_TRUE(hub_a_events.wait_for(1, 3000)) << "Handshake not completed";

    // Register the channel on Hub A (producer P_A).
    Messenger prod_a;
    ASSERT_TRUE(prod_a.connect(hub_a.endpoint, hub_a.pubkey));
    auto handle_a = prod_a.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle_a.has_value());

    // Register the same channel on Hub B (producer P_B listens for relay events).
    Messenger prod_b;
    ASSERT_TRUE(prod_b.connect(hub_b.endpoint, hub_b.pubkey));
    auto handle_b = prod_b.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle_b.has_value());

    EventCollector prod_b_events;
    prod_b.on_channel_error(channel, [&](std::string ev, json details)
    { prod_b_events.push(ev, details); });

    // Consumer on Hub A sends CHANNEL_NOTIFY_REQ → broker relays to Hub B.
    Messenger consumer_a;
    ASSERT_TRUE(consumer_a.connect(hub_a.endpoint, hub_a.pubkey));
    auto cons_handle = consumer_a.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());
    consumer_a.enqueue_channel_notify(channel, "CONS-A-UID", "sensor_alarm", "temp=99");

    // Hub B's producer must receive the relayed event with originator_uid set.
    ASSERT_TRUE(prod_b_events.wait_for(1, 3000))
        << "Hub B producer did not receive relayed CHANNEL_EVENT_NOTIFY";

    auto details = prod_b_events.get_details(0);
    EXPECT_EQ(details.value("data", ""), "temp=99");
    EXPECT_EQ(details.value("originator_uid", ""), "HUB-RELAY-A");

    hub_b.stop_and_join();
    hub_a.stop_and_join();
}

// ============================================================================
// Test 3: CHANNEL_BROADCAST_REQ relayed across hubs
// ============================================================================

TEST_F(BrokerFederationTest, RelayChannelBroadcast_DeliveredToHubB)
{
    // CHANNEL_BROADCAST_REQ on Hub A is relayed to Hub B.
    // Hub B's producer P_B receives the relayed broadcast notification.

    const std::string channel = pid_chan("fed.relay.bcast");

    HubEventCollector hub_a_events;

    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_a.self_hub_uid           = "HUB-BCAST-A";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };

    FederationPeer peer_b_on_a;
    peer_b_on_a.hub_uid  = "HUB-BCAST-B";
    peer_b_on_a.channels = {channel};
    cfg_a.peers.push_back(std::move(peer_b_on_a));

    auto hub_a = start_local_broker(std::move(cfg_a));

    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_b.self_hub_uid           = "HUB-BCAST-B";

    FederationPeer peer_a_on_b;
    peer_a_on_b.hub_uid         = "HUB-BCAST-A";
    peer_a_on_b.broker_endpoint = hub_a.endpoint;
    peer_a_on_b.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a_on_b));

    auto hub_b = start_local_broker(std::move(cfg_b));

    ASSERT_TRUE(hub_a_events.wait_for(1, 3000)) << "Handshake not completed";

    // Channel on Hub A.
    Messenger prod_a;
    ASSERT_TRUE(prod_a.connect(hub_a.endpoint, hub_a.pubkey));
    auto handle_a = prod_a.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle_a.has_value());

    // Same channel on Hub B — P_B listens.
    Messenger prod_b;
    ASSERT_TRUE(prod_b.connect(hub_b.endpoint, hub_b.pubkey));
    auto handle_b = prod_b.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle_b.has_value());

    EventCollector prod_b_events;
    prod_b.on_channel_error(channel, [&](std::string ev, json details)
    { prod_b_events.push(ev, details); });

    // Send CHANNEL_BROADCAST_REQ to Hub A.
    Messenger sender;
    ASSERT_TRUE(sender.connect(hub_a.endpoint, hub_a.pubkey));
    sender.enqueue_channel_broadcast(channel, "SENDER-UID", "start", "go");

    // Hub B's producer receives the relayed broadcast.
    ASSERT_TRUE(prod_b_events.wait_for(1, 3000))
        << "Hub B producer did not receive relayed broadcast";

    // Relay delivers as CHANNEL_EVENT_NOTIFY; event encodes "broadcast:<msg>" form.
    auto ev_str = prod_b_events.get_event(0);
    EXPECT_FALSE(ev_str.empty());
    auto details = prod_b_events.get_details(0);
    EXPECT_EQ(details.value("originator_uid", ""), "HUB-BCAST-A");

    hub_b.stop_and_join();
    hub_a.stop_and_join();
}

// ============================================================================
// Test 4: HUB_TARGETED_MSG delivery via send_hub_targeted_msg
// ============================================================================

TEST_F(BrokerFederationTest, TargetedMessage_FiresOnHubMessage)
{
    // Hub B connects outbound to Hub A → Hub B is an inbound peer of Hub A.
    // Hub A calls send_hub_targeted_msg("HUB-TGT-B", ...) → routed via Hub A's
    // ROUTER socket using Hub B's ZMQ routing identity.
    // Hub B's on_hub_message callback fires with channel + payload + source_uid.

    HubEventCollector hub_a_events;
    HubEventCollector hub_b_events;

    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_a.self_hub_uid           = "HUB-TGT-A";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };

    // Hub B registered in cfg — relay_channels empty; just allows lookup.
    FederationPeer peer_b_cfg;
    peer_b_cfg.hub_uid = "HUB-TGT-B";
    cfg_a.peers.push_back(std::move(peer_b_cfg));

    auto hub_a = start_local_broker(std::move(cfg_a));

    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_b.self_hub_uid           = "HUB-TGT-B";

    FederationPeer peer_a_on_b;
    peer_a_on_b.hub_uid         = "HUB-TGT-A";
    peer_a_on_b.broker_endpoint = hub_a.endpoint;
    peer_a_on_b.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a_on_b));

    cfg_b.on_hub_message = [&](const std::string &ch, const std::string &payload,
                                const std::string &src_uid)
    { hub_b_events.push_message(ch, payload, src_uid); };

    auto hub_b = start_local_broker(std::move(cfg_b));

    // Wait for Hub A to register Hub B as an inbound peer.
    ASSERT_TRUE(hub_a_events.wait_for(1, 3000)) << "Handshake not completed";

    // Hub A sends a targeted message to Hub B (Hub B is inbound — Hub A has its identity).
    hub_a.service->send_hub_targeted_msg("HUB-TGT-B", "ctrl.channel", R"({"cmd":"start"})");

    // Hub B's on_hub_message fires.
    ASSERT_TRUE(hub_b_events.wait_for(1, 3000))
        << "Hub B on_hub_message did not fire within timeout";

    auto ev = hub_b_events.get(0);
    EXPECT_EQ(ev.type, "message");
    EXPECT_EQ(ev.hub_uid, "HUB-TGT-A");
    EXPECT_EQ(ev.channel, "ctrl.channel");
    EXPECT_EQ(ev.payload, R"({"cmd":"start"})");

    hub_b.stop_and_join();
    hub_a.stop_and_join();
}

// ============================================================================
// Test 5: Channel-filtered relay — unlisted channels are not forwarded
// ============================================================================

TEST_F(BrokerFederationTest, RelayChannelFilter_UnsubscribedChannelNotRelayed)
{
    // Hub A is configured to relay only "allowed_channel" to Hub B.
    // "other_channel" is NOT in the relay list.
    // A CHANNEL_NOTIFY_REQ on "other_channel" must NOT reach Hub B.

    const std::string allowed_channel = pid_chan("fed.filter.allowed");
    const std::string other_channel   = pid_chan("fed.filter.other");

    HubEventCollector hub_a_events;

    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_a.self_hub_uid           = "HUB-FILT-A";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };

    FederationPeer peer_b_on_a;
    peer_b_on_a.hub_uid  = "HUB-FILT-B";
    peer_b_on_a.channels = {allowed_channel}; // other_channel NOT included
    cfg_a.peers.push_back(std::move(peer_b_on_a));

    auto hub_a = start_local_broker(std::move(cfg_a));

    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_b.self_hub_uid           = "HUB-FILT-B";

    FederationPeer peer_a_on_b;
    peer_a_on_b.hub_uid         = "HUB-FILT-A";
    peer_a_on_b.broker_endpoint = hub_a.endpoint;
    peer_a_on_b.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a_on_b));

    auto hub_b = start_local_broker(std::move(cfg_b));

    ASSERT_TRUE(hub_a_events.wait_for(1, 3000)) << "Handshake not completed";

    // Register other_channel on Hub A.
    Messenger prod_a;
    ASSERT_TRUE(prod_a.connect(hub_a.endpoint, hub_a.pubkey));
    auto handle_other_a = prod_a.create_channel(other_channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle_other_a.has_value());

    // Register other_channel on Hub B to capture any mistaken relays.
    Messenger prod_b;
    ASSERT_TRUE(prod_b.connect(hub_b.endpoint, hub_b.pubkey));
    auto handle_other_b = prod_b.create_channel(other_channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle_other_b.has_value());

    EventCollector prod_b_events;
    prod_b.on_channel_error(other_channel, [&](std::string ev, json details)
    { prod_b_events.push(ev, details); });

    // Consumer on Hub A notifies other_channel — should NOT relay to Hub B.
    Messenger consumer_a;
    ASSERT_TRUE(consumer_a.connect(hub_a.endpoint, hub_a.pubkey));
    auto cons_handle = consumer_a.connect_channel(other_channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());
    consumer_a.enqueue_channel_notify(other_channel, "CONS-A-UID", "unlisted_event", "data");

    // Wait for broker to process; relay must NOT have occurred.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(prod_b_events.size(), 0) << "other_channel was incorrectly relayed to Hub B";

    hub_b.stop_and_join();
    hub_a.stop_and_join();
}

// ============================================================================
// Test 6: HUB_PEER_BYE triggers on_hub_disconnected
// ============================================================================

TEST_F(BrokerFederationTest, PeerBye_TriggersOnHubDisconnected)
{
    // Hub B connects outbound to Hub A.
    // When Hub B stops, it sends HUB_PEER_BYE via its outbound DEALER.
    // Hub A's ROUTER receives the BYE → fires on_hub_disconnected("HUB-BYE-B").

    HubEventCollector hub_a_events;

    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_a.self_hub_uid           = "HUB-BYE-A";
    cfg_a.on_hub_connected    = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };
    cfg_a.on_hub_disconnected = [&](const std::string &uid)
    { hub_a_events.push_disconnected(uid); };

    auto hub_a = start_local_broker(std::move(cfg_a));

    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.channel_shutdown_grace = std::chrono::seconds(0);
    cfg_b.self_hub_uid           = "HUB-BYE-B";

    FederationPeer peer_a_on_b;
    peer_a_on_b.hub_uid         = "HUB-BYE-A";
    peer_a_on_b.broker_endpoint = hub_a.endpoint;
    peer_a_on_b.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a_on_b));

    auto hub_b = start_local_broker(std::move(cfg_b));

    // Wait for handshake.
    ASSERT_TRUE(hub_a_events.wait_for(1, 3000)) << "Handshake not completed";
    EXPECT_TRUE(hub_a_events.has_event("connected", "HUB-BYE-B"));

    // Stop Hub B — sends HUB_PEER_BYE from its outbound DEALER to Hub A's ROUTER.
    hub_b.stop_and_join();

    // Hub A fires on_hub_disconnected for Hub B.
    ASSERT_TRUE(hub_a_events.wait_for(2, 3000))
        << "Hub A on_hub_disconnected did not fire after Hub B stopped";
    EXPECT_TRUE(hub_a_events.has_event("disconnected", "HUB-BYE-B"));

    hub_a.stop_and_join();
}
