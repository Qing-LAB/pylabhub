/**
 * @file test_datahub_hub_federation.cpp
 * @brief Hub Federation protocol tests (HEP-CORE-0022).
 *
 * Suite: BrokerFederationTest
 *
 * Tests the federation control-plane between two in-process brokers:
 *   - HUB_PEER_HELLO / HUB_PEER_HELLO_ACK handshake (on_hub_connected)
 *   - HUB_TARGETED_MSG delivery to on_hub_message callback
 *   - HUB_PEER_BYE triggers on_hub_disconnected on the peer
 *
 * Topology: Hub B connects outbound to Hub A. Hub A is the "server" (ROUTER).
 * CTest safety: all channel names are PID-suffixed.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"

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

struct HubEventCollector
{
    struct HubEvent
    {
        std::string type;
        std::string hub_uid;
        std::string channel;
        std::string payload;
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

    bool has_event(const std::string &type, const std::string &hub_uid)
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto &e : events)
            if (e.type == type && e.hub_uid == hub_uid)
                return true;
        return false;
    }

    HubEvent get(int index)
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (index < static_cast<int>(events.size()))
            return events[index];
        return {};
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
            pylabhub::hub::GetZMQContextModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<LifecycleGuard> BrokerFederationTest::s_lifecycle_;

// ============================================================================
// Test 1: HUB_PEER_HELLO / HUB_PEER_HELLO_ACK handshake
// ============================================================================

TEST_F(BrokerFederationTest, HelloHandshake_FiresOnHubConnected)
{
    HubEventCollector hub_a_events;
    HubEventCollector hub_b_events;

    // Hub A — accepts inbound HELLO from Hub B.
    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.grace_override         = std::chrono::milliseconds(0);
    cfg_a.self_hub_uid           = "hub.test.a";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };

    FederationPeer peer_b_inbound;
    peer_b_inbound.hub_uid = "hub.test.b";
    cfg_a.peers.push_back(std::move(peer_b_inbound));

    auto hub_a = start_local_broker(std::move(cfg_a));

    // Hub B — outbound connection to Hub A.
    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.grace_override         = std::chrono::milliseconds(0);
    cfg_b.self_hub_uid           = "hub.test.b";

    FederationPeer peer_a;
    peer_a.hub_uid         = "hub.test.a";
    peer_a.broker_endpoint = hub_a.endpoint;
    peer_a.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a));

    cfg_b.on_hub_connected = [&](const std::string &uid)
    { hub_b_events.push_connected(uid); };

    auto hub_b = start_local_broker(std::move(cfg_b));

    ASSERT_TRUE(hub_a_events.wait_for(1, 3000))
        << "Hub A did not fire on_hub_connected";
    EXPECT_EQ(hub_a_events.get(0).hub_uid, "hub.test.b");

    ASSERT_TRUE(hub_b_events.wait_for(1, 3000))
        << "Hub B did not fire on_hub_connected";
    EXPECT_EQ(hub_b_events.get(0).hub_uid, "hub.test.a");

    hub_b.stop_and_join();
    hub_a.stop_and_join();
}

// ============================================================================
// Test 2: HUB_TARGETED_MSG delivery
// ============================================================================

TEST_F(BrokerFederationTest, TargetedMessage_FiresOnHubMessage)
{
    const std::string channel = pid_chan("fed.targeted.msg");

    HubEventCollector hub_a_events;
    HubEventCollector hub_b_events;

    // Hub A (server) — Hub B connects inbound to Hub A.
    // Hub A can send HUB_TARGETED_MSG to Hub B (which is an inbound peer of Hub A).
    // Hub B needs on_hub_message to receive it.
    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.grace_override         = std::chrono::milliseconds(0);
    cfg_a.self_hub_uid           = "hub.target.a";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };

    FederationPeer peer_b_inbound;
    peer_b_inbound.hub_uid  = "hub.target.b";
    peer_b_inbound.channels = {channel};
    cfg_a.peers.push_back(std::move(peer_b_inbound));

    auto hub_a = start_local_broker(std::move(cfg_a));

    // Hub B (client) — connects outbound to Hub A.
    // Hub B receives HUB_TARGETED_MSG from Hub A via on_hub_message.
    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.grace_override         = std::chrono::milliseconds(0);
    cfg_b.self_hub_uid           = "hub.target.b";
    cfg_b.on_hub_message = [&](const std::string &ch, const std::string &payload,
                               const std::string &src)
    { hub_b_events.push_message(ch, payload, src); };

    FederationPeer peer_a;
    peer_a.hub_uid         = "hub.target.a";
    peer_a.broker_endpoint = hub_a.endpoint;
    peer_a.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a));

    cfg_b.on_hub_connected = [&](const std::string &uid)
    { hub_b_events.push_connected(uid); };

    auto hub_b = start_local_broker(std::move(cfg_b));

    // Wait for handshake (Hub A sees Hub B connected).
    ASSERT_TRUE(hub_a_events.wait_for(1, 3000)) << "Handshake not completed";

    // Hub A sends targeted message to Hub B (Hub B is inbound peer of Hub A).
    hub_a.service->send_hub_targeted_msg("hub.target.b", channel, "hello from A");

    // Hub B should receive the message via on_hub_message callback.
    ASSERT_TRUE(hub_b_events.wait_for(2, 3000))
        << "Hub B did not receive HUB_TARGETED_MSG";

    bool found_msg = hub_b_events.has_event("message", "hub.target.a");
    EXPECT_TRUE(found_msg) << "No 'message' event from HUB-TARGET-A found in Hub B events";

    hub_b.stop_and_join();
    hub_a.stop_and_join();
}

// ============================================================================
// Test 3: HUB_PEER_BYE triggers on_hub_disconnected
// ============================================================================

TEST_F(BrokerFederationTest, PeerBye_TriggersOnHubDisconnected)
{
    HubEventCollector hub_a_events;

    // Hub A
    BrokerService::Config cfg_a;
    cfg_a.endpoint               = "tcp://127.0.0.1:0";
    cfg_a.grace_override         = std::chrono::milliseconds(0);
    cfg_a.self_hub_uid           = "hub.bye.a";
    cfg_a.on_hub_connected       = [&](const std::string &uid)
    { hub_a_events.push_connected(uid); };
    cfg_a.on_hub_disconnected    = [&](const std::string &uid)
    { hub_a_events.push_disconnected(uid); };

    FederationPeer peer_b_inbound;
    peer_b_inbound.hub_uid = "hub.bye.b";
    cfg_a.peers.push_back(std::move(peer_b_inbound));

    auto hub_a = start_local_broker(std::move(cfg_a));

    // Hub B
    BrokerService::Config cfg_b;
    cfg_b.endpoint               = "tcp://127.0.0.1:0";
    cfg_b.grace_override         = std::chrono::milliseconds(0);
    cfg_b.self_hub_uid           = "hub.bye.b";

    FederationPeer peer_a;
    peer_a.hub_uid         = "hub.bye.a";
    peer_a.broker_endpoint = hub_a.endpoint;
    peer_a.pubkey_z85      = hub_a.pubkey;
    cfg_b.peers.push_back(std::move(peer_a));

    auto hub_b = start_local_broker(std::move(cfg_b));

    // Wait for handshake
    ASSERT_TRUE(hub_a_events.wait_for(1, 3000));

    // Stop Hub B — triggers HUB_PEER_BYE to Hub A
    hub_b.stop_and_join();

    // Hub A should receive on_hub_disconnected
    ASSERT_TRUE(hub_a_events.wait_for(2, 3000))
        << "Hub A did not fire on_hub_disconnected after Hub B bye";
    EXPECT_TRUE(hub_a_events.has_event("disconnected", "hub.bye.b"));

    hub_a.stop_and_join();
}
