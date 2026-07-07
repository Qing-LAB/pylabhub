/**
 * @file hub_federation_workers.cpp
 * @brief Worker bodies for Hub Federation tests (HEP-CORE-0022, Pattern 3).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Each scenario constructs two
 * `BrokerService` instances → transitively touches Logger / SecureSubsystem /
 * ZMQContext lifecycle modules; per README_testing.md § "Choosing a test
 * pattern", the body must run in a worker subprocess.
 */

#include "hub_federation_workers.h"

#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"

#include "log_capture_fixture.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace hub_federation
{

namespace
{

struct LocalBrokerHandle
{
    std::unique_ptr<pylabhub::hub::HubState> hub_state;
    std::unique_ptr<BrokerService>           service;
    std::thread                              thread;
    std::string                              endpoint;
    std::string                              pubkey;

    LocalBrokerHandle()                                                 = default;
    LocalBrokerHandle(LocalBrokerHandle &&) noexcept                    = default;
    LocalBrokerHandle &operator=(LocalBrokerHandle &&) noexcept         = default;
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

    auto state   = std::make_unique<pylabhub::hub::HubState>();
    auto svc     = std::make_unique<BrokerService>(std::move(cfg), *state);
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get();

    LocalBrokerHandle h;
    h.hub_state = std::move(state);
    h.service   = std::move(svc);
    h.thread    = std::move(t);
    h.endpoint  = info.first;
    h.pubkey    = info.second;
    return h;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(getpid());
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

} // namespace

int hello_handshake_fires_on_hub_connected()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            HubEventCollector hub_a_events;
            HubEventCollector hub_b_events;

            BrokerService::Config cfg_a;
            cfg_a.endpoint         = "tcp://127.0.0.1:0";
            cfg_a.use_curve              = false;
            cfg_a.enforce_ctrl_admission = false;
            // HEP-CORE-0035 §4.8: federation HELLO/BYE tests do not exercise CURVE; both knobs off to keep the broker config self-consistent (the broker WARNs on enforce=true + use_curve=false because the gate is inert without CURVE handshakes to gate).
            cfg_a.self_hub_uid     = "hub.test.a";
            cfg_a.on_hub_connected = [&](const std::string &uid)
            { hub_a_events.push_connected(uid); };

            FederationPeer peer_b_inbound;
            peer_b_inbound.hub_uid = "hub.test.b";
            cfg_a.peers.push_back(std::move(peer_b_inbound));

            auto hub_a = start_local_broker(std::move(cfg_a));

            BrokerService::Config cfg_b;
            cfg_b.endpoint     = "tcp://127.0.0.1:0";
            cfg_b.use_curve              = false;
            cfg_b.enforce_ctrl_admission = false;
            // HEP-CORE-0035 §4.8: federation HELLO/BYE tests do not exercise CURVE; both knobs off to keep the broker config self-consistent (the broker WARNs on enforce=true + use_curve=false because the gate is inert without CURVE handshakes to gate).
            cfg_b.self_hub_uid = "hub.test.b";

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

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_federation::hello_handshake_fires_on_hub_connected",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int targeted_message_fires_on_hub_message()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const std::string channel = pid_chan("fed.targeted.msg");

            HubEventCollector hub_a_events;
            HubEventCollector hub_b_events;

            BrokerService::Config cfg_a;
            cfg_a.endpoint         = "tcp://127.0.0.1:0";
            cfg_a.use_curve              = false;
            cfg_a.enforce_ctrl_admission = false;
            // HEP-CORE-0035 §4.8: federation HELLO/BYE tests do not exercise CURVE; both knobs off to keep the broker config self-consistent (the broker WARNs on enforce=true + use_curve=false because the gate is inert without CURVE handshakes to gate).
            cfg_a.self_hub_uid     = "hub.target.a";
            cfg_a.on_hub_connected = [&](const std::string &uid)
            { hub_a_events.push_connected(uid); };

            FederationPeer peer_b_inbound;
            peer_b_inbound.hub_uid  = "hub.target.b";
            peer_b_inbound.channels = {channel};
            cfg_a.peers.push_back(std::move(peer_b_inbound));

            auto hub_a = start_local_broker(std::move(cfg_a));

            BrokerService::Config cfg_b;
            cfg_b.endpoint       = "tcp://127.0.0.1:0";
            cfg_b.use_curve              = false;
            cfg_b.enforce_ctrl_admission = false;
            // HEP-CORE-0035 §4.8: federation HELLO/BYE tests do not exercise CURVE; both knobs off to keep the broker config self-consistent (the broker WARNs on enforce=true + use_curve=false because the gate is inert without CURVE handshakes to gate).
            cfg_b.self_hub_uid   = "hub.target.b";
            cfg_b.on_hub_message = [&](const std::string &ch,
                                       const std::string &payload,
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

            ASSERT_TRUE(hub_a_events.wait_for(1, 3000))
                << "Handshake not completed";

            hub_a.service->send_hub_targeted_msg("hub.target.b", channel,
                                                  "hello from A");

            ASSERT_TRUE(hub_b_events.wait_for(2, 3000))
                << "Hub B did not receive HUB_TARGETED_MSG";

            bool found_msg = hub_b_events.has_event("message", "hub.target.a");
            EXPECT_TRUE(found_msg)
                << "No 'message' event from HUB-TARGET-A found in Hub B events";

            hub_b.stop_and_join();
            hub_a.stop_and_join();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_federation::targeted_message_fires_on_hub_message",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int peer_bye_triggers_on_hub_disconnected()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            HubEventCollector hub_a_events;

            BrokerService::Config cfg_a;
            cfg_a.endpoint            = "tcp://127.0.0.1:0";
            cfg_a.use_curve              = false;
            cfg_a.enforce_ctrl_admission = false;
            // HEP-CORE-0035 §4.8: federation HELLO/BYE tests do not exercise CURVE; both knobs off to keep the broker config self-consistent (the broker WARNs on enforce=true + use_curve=false because the gate is inert without CURVE handshakes to gate).
            cfg_a.self_hub_uid        = "hub.bye.a";
            cfg_a.on_hub_connected    = [&](const std::string &uid)
            { hub_a_events.push_connected(uid); };
            cfg_a.on_hub_disconnected = [&](const std::string &uid)
            { hub_a_events.push_disconnected(uid); };

            FederationPeer peer_b_inbound;
            peer_b_inbound.hub_uid = "hub.bye.b";
            cfg_a.peers.push_back(std::move(peer_b_inbound));

            auto hub_a = start_local_broker(std::move(cfg_a));

            BrokerService::Config cfg_b;
            cfg_b.endpoint     = "tcp://127.0.0.1:0";
            cfg_b.use_curve              = false;
            cfg_b.enforce_ctrl_admission = false;
            // HEP-CORE-0035 §4.8: federation HELLO/BYE tests do not exercise CURVE; both knobs off to keep the broker config self-consistent (the broker WARNs on enforce=true + use_curve=false because the gate is inert without CURVE handshakes to gate).
            cfg_b.self_hub_uid = "hub.bye.b";

            FederationPeer peer_a;
            peer_a.hub_uid         = "hub.bye.a";
            peer_a.broker_endpoint = hub_a.endpoint;
            peer_a.pubkey_z85      = hub_a.pubkey;
            cfg_b.peers.push_back(std::move(peer_a));

            auto hub_b = start_local_broker(std::move(cfg_b));

            ASSERT_TRUE(hub_a_events.wait_for(1, 3000));

            hub_b.stop_and_join();

            ASSERT_TRUE(hub_a_events.wait_for(2, 3000))
                << "Hub A did not fire on_hub_disconnected after Hub B bye";
            EXPECT_TRUE(hub_a_events.has_event("disconnected", "hub.bye.b"));

            hub_a.stop_and_join();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_federation::peer_bye_triggers_on_hub_disconnected",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace hub_federation
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct HubFederationRegistrar
{
    HubFederationRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "hub_federation")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_federation;

                if (sc == "hello_handshake_fires_on_hub_connected")
                    return hello_handshake_fires_on_hub_connected();
                if (sc == "targeted_message_fires_on_hub_message")
                    return targeted_message_fires_on_hub_message();
                if (sc == "peer_bye_triggers_on_hub_disconnected")
                    return peer_bye_triggers_on_hub_disconnected();
                return -1;
            });
    }
} g_registrar;

} // namespace
