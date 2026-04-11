/**
 * @file datahub_channel_group_workers.cpp
 * @brief L3 test workers for channel pub/sub messaging (HEP-CORE-0030).
 */

#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_sync_utils.h"

#include "utils/broker_request_channel.hpp"
#include "utils/broker_service.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "plh_datahub.hpp"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace pylabhub;
using namespace pylabhub::hub;
using namespace pylabhub::broker;

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetLifecycleModule(); }

namespace
{

struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread thread;
    std::string endpoint;
    std::string pubkey;
    void stop_and_join() { service->stop(); if (thread.joinable()) thread.join(); }
};

BrokerHandle start_broker()
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto rp = std::make_shared<std::promise<ReadyInfo>>();
    auto rf = rp->get_future();
    BrokerService::Config bcfg;
    bcfg.endpoint = "tcp://127.0.0.1:0";
    bcfg.use_curve = true;
    bcfg.on_ready = [rp](const std::string &ep, const std::string &pk)
    { rp->set_value({ep, pk}); };
    auto svc = std::make_unique<BrokerService>(std::move(bcfg));
    auto *raw = svc.get();
    std::thread t([raw] { raw->run(); });
    auto info = rf.get();
    return {std::move(svc), std::move(t), info.first, info.second};
}

struct ChannelClient
{
    BrokerRequestChannel ch;
    std::atomic<bool> running{true};
    std::thread poll_thread;

    void connect(const std::string &ep, const std::string &pk,
                 const std::string &uid, const std::string &name)
    {
        BrokerRequestChannel::Config cfg;
        cfg.broker_endpoint = ep;
        cfg.broker_pubkey = pk;
        cfg.role_uid = uid;
        cfg.role_name = name;
        ASSERT_TRUE(ch.connect(cfg));
        poll_thread = std::thread([this] {
            ch.run_poll_loop([this] { return running.load(); });
        });
    }

    void stop()
    {
        running.store(false);
        ch.stop();
        if (poll_thread.joinable())
            poll_thread.join();
        ch.disconnect();
    }
};

// ============================================================================
// Worker: join + leave + member query
// ============================================================================

int channel_join_leave()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    ChannelClient c1, c2;
    c1.connect(broker.endpoint, broker.pubkey, "ROLE-A-001", "role_a");
    c2.connect(broker.endpoint, broker.pubkey, "ROLE-B-002", "role_b");

    // Role A joins channel.
    auto join1 = c1.ch.join_channel("#test_ch", 5000);
    EXPECT_TRUE(join1.has_value()) << "CHANNEL_JOIN_REQ timed out";
    if (join1)
    {
        EXPECT_EQ(join1->value("status", ""), "success");
        auto members = (*join1)["members"];
        EXPECT_EQ(members.size(), 1u); // just A
    }

    // Role B joins — should see both members.
    auto join2 = c2.ch.join_channel("#test_ch", 5000);
    EXPECT_TRUE(join2.has_value());
    if (join2)
    {
        auto members = (*join2)["members"];
        EXPECT_EQ(members.size(), 2u);
    }

    // Query members.
    auto mq = c1.ch.query_channel_members("#test_ch", 5000);
    EXPECT_TRUE(mq.has_value());
    if (mq)
    {
        auto members = (*mq)["members"];
        EXPECT_EQ(members.size(), 2u);
    }

    // Role A leaves.
    bool left = c1.ch.leave_channel("#test_ch", 5000);
    EXPECT_TRUE(left);

    // Query again — should have 1 member.
    auto mq2 = c2.ch.query_channel_members("#test_ch", 5000);
    EXPECT_TRUE(mq2.has_value());
    if (mq2)
    {
        auto members = (*mq2)["members"];
        EXPECT_EQ(members.size(), 1u);
    }

    c1.stop();
    c2.stop();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker: message fan-out
// ============================================================================

int channel_msg_fanout()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    // Track notifications on client B.
    std::atomic<int> msg_count{0};
    nlohmann::json last_body;
    std::mutex body_mu;

    ChannelClient c1, c2;
    c2.ch.on_notification([&](const std::string &type, const nlohmann::json &payload) {
        if (type == "CHANNEL_MSG_NOTIFY")
        {
            std::lock_guard<std::mutex> lk(body_mu);
            last_body = payload.value("body", nlohmann::json::object());
            msg_count.fetch_add(1);
        }
    });

    c1.connect(broker.endpoint, broker.pubkey, "SENDER-001", "sender");
    c2.connect(broker.endpoint, broker.pubkey, "RECVR-002", "receiver");

    // Both join the channel.
    c1.ch.join_channel("#msg_ch", 5000);
    c2.ch.join_channel("#msg_ch", 5000);

    // Give broker time to process joins.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Sender sends a message.
    c1.ch.send_channel_msg("#msg_ch", {{"event", "hello"}, {"value", 42}});

    // Wait for receiver to get it.
    bool got = pylabhub::tests::helper::poll_until(
        [&] { return msg_count.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got) << "CHANNEL_MSG_NOTIFY never received";

    if (got)
    {
        std::lock_guard<std::mutex> lk(body_mu);
        EXPECT_EQ(last_body.value("event", ""), "hello");
        EXPECT_EQ(last_body.value("value", 0), 42);
    }

    c1.stop();
    c2.stop();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker: join notification
// ============================================================================

int channel_join_notify()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    std::atomic<int> join_notify_count{0};
    std::string notified_uid;
    std::mutex mu;

    ChannelClient c1, c2;
    c1.ch.on_notification([&](const std::string &type, const nlohmann::json &payload) {
        if (type == "CHANNEL_JOIN_NOTIFY")
        {
            std::lock_guard<std::mutex> lk(mu);
            notified_uid = payload.value("role_uid", "");
            join_notify_count.fetch_add(1);
        }
    });

    c1.connect(broker.endpoint, broker.pubkey, "FIRST-001", "first");

    // First joins channel.
    c1.ch.join_channel("#notify_ch", 5000);

    // Second joins — first should get CHANNEL_JOIN_NOTIFY.
    c2.connect(broker.endpoint, broker.pubkey, "SECOND-002", "second");
    c2.ch.join_channel("#notify_ch", 5000);

    bool got = pylabhub::tests::helper::poll_until(
        [&] { return join_notify_count.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got) << "CHANNEL_JOIN_NOTIFY never received";

    if (got)
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_EQ(notified_uid, "SECOND-002");
    }

    c1.stop();
    c2.stop();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker dispatcher
// ============================================================================

struct ChannelGroupWorkerRegistrar
{
    ChannelGroupWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "channel_group")
                    return -1;
                std::string scenario(mode.substr(dot + 1));

                if (scenario == "join_leave")
                    return channel_join_leave();
                if (scenario == "msg_fanout")
                    return channel_msg_fanout();
                if (scenario == "join_notify")
                    return channel_join_notify();
                return -1;
            });
    }
};

static ChannelGroupWorkerRegistrar s_registrar;

} // anonymous namespace
