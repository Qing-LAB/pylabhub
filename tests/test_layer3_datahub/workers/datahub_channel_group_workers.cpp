/**
 * @file datahub_channel_group_workers.cpp
 * @brief L3 test workers for band pub/sub messaging (HEP-CORE-0030).
 */

#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_sync_utils.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"
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
static auto hub_module()    { return ::pylabhub::hub::GetDataBlockModule(); }
static auto zmq_module()    { return ::pylabhub::hub::GetZMQContextModule(); }

namespace
{

struct BrokerHandle
{
    std::unique_ptr<pylabhub::hub::HubState> hub_state;
    std::unique_ptr<BrokerService>           service;
    std::thread                              thread;
    std::string                              endpoint;
    std::string                              pubkey;
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
    auto state = std::make_unique<pylabhub::hub::HubState>();
    auto svc   = std::make_unique<BrokerService>(std::move(bcfg), *state);
    auto *raw  = svc.get();
    std::thread t([raw] { raw->run(); });
    auto info = rf.get();
    return {std::move(state), std::move(svc), std::move(t),
            info.first, info.second};
}

struct ChannelClient
{
    BrokerRequestComm ch;
    std::atomic<bool> running{true};
    std::thread poll_thread;

    void connect(const std::string &ep, const std::string &pk,
                 const std::string &uid, const std::string &name)
    {
        BrokerRequestComm::Config cfg;
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
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    ChannelClient c1, c2;
    c1.connect(broker.endpoint, broker.pubkey, "prod.role.a.uid00000001", "role_a");
    c2.connect(broker.endpoint, broker.pubkey, "prod.role.b.uid00000002", "role_b");

    // Role A joins channel.
    auto join1 = c1.ch.band_join("!test_ch", 5000);
    EXPECT_TRUE(join1.has_value()) << "BAND_JOIN_REQ timed out";
    if (join1)
    {
        EXPECT_EQ(join1->value("status", ""), "success");
        auto members = (*join1)["members"];
        EXPECT_EQ(members.size(), 1u); // just A
    }

    // Role B joins — should see both members.
    auto join2 = c2.ch.band_join("!test_ch", 5000);
    EXPECT_TRUE(join2.has_value());
    if (join2)
    {
        auto members = (*join2)["members"];
        EXPECT_EQ(members.size(), 2u);
    }

    // Query members.
    auto mq = c1.ch.band_members("!test_ch", 5000);
    EXPECT_TRUE(mq.has_value());
    if (mq)
    {
        auto members = (*mq)["members"];
        EXPECT_EQ(members.size(), 2u);
    }

    // Role A leaves.
    bool left = c1.ch.band_leave("!test_ch", 5000);
    EXPECT_TRUE(left);

    // Query again — should have 1 member.
    auto mq2 = c2.ch.band_members("!test_ch", 5000);
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
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    // Track notifications on client B.
    std::atomic<int> msg_count{0};
    nlohmann::json last_body;
    std::mutex body_mu;

    ChannelClient c1, c2;
    c2.ch.on_notification([&](const std::string &type, const nlohmann::json &payload) {
        if (type == "BAND_BROADCAST_NOTIFY")
        {
            std::lock_guard<std::mutex> lk(body_mu);
            last_body = payload.value("body", nlohmann::json::object());
            msg_count.fetch_add(1);
        }
    });

    c1.connect(broker.endpoint, broker.pubkey, "prod.sender.uid00000001", "sender");
    c2.connect(broker.endpoint, broker.pubkey, "cons.recvr.uid00000002", "receiver");

    // Both join the channel.
    c1.ch.band_join("!msg_ch", 5000);
    c2.ch.band_join("!msg_ch", 5000);

    // Give broker time to process joins.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Sender sends a message.
    c1.ch.band_broadcast("!msg_ch", {{"event", "hello"}, {"value", 42}});

    // Wait for receiver to get it.
    bool got = pylabhub::tests::helper::poll_until(
        [&] { return msg_count.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got) << "BAND_BROADCAST_NOTIFY never received";

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
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    std::atomic<int> join_notify_count{0};
    std::string notified_uid;
    std::mutex mu;

    ChannelClient c1, c2;
    c1.ch.on_notification([&](const std::string &type, const nlohmann::json &payload) {
        if (type == "BAND_JOIN_NOTIFY")
        {
            std::lock_guard<std::mutex> lk(mu);
            notified_uid = payload.value("role_uid", "");
            join_notify_count.fetch_add(1);
        }
    });

    c1.connect(broker.endpoint, broker.pubkey, "prod.first.uid00000001", "first");

    // First joins channel.
    c1.ch.band_join("!notify_ch", 5000);

    // Second joins — first should get BAND_JOIN_NOTIFY.
    c2.connect(broker.endpoint, broker.pubkey, "prod.second.uid00000002", "second");
    c2.ch.band_join("!notify_ch", 5000);

    bool got = pylabhub::tests::helper::poll_until(
        [&] { return join_notify_count.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got) << "BAND_JOIN_NOTIFY never received";

    if (got)
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_EQ(notified_uid, "prod.second.uid00000002");
    }

    c1.stop();
    c2.stop();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker: RoleAPIBase channel integration — full path test
// ============================================================================

int roleapi_channel()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    // Set up two RoleAPIBases, each with its own BrokerRequestComm.
    scripting::RoleHostCore core1, core2;
    core1.set_running(true);
    core2.set_running(true);

    auto api1 = std::make_unique<scripting::RoleAPIBase>(core1, "role_a", "prod.role.a.uid00000100");
    auto api2 = std::make_unique<scripting::RoleAPIBase>(core2, "role_b", "prod.role.b.uid00000200");

    auto bc1 = std::make_unique<BrokerRequestComm>();
    auto bc2 = std::make_unique<BrokerRequestComm>();

    BrokerRequestComm::Config bc_cfg;
    bc_cfg.broker_endpoint = broker.endpoint;
    bc_cfg.broker_pubkey = broker.pubkey;

    bc_cfg.role_uid = "prod.role.a.uid00000100";
    bc_cfg.role_name = "role_a";
    EXPECT_TRUE(bc1->connect(bc_cfg));

    bc_cfg.role_uid = "prod.role.b.uid00000200";
    bc_cfg.role_name = "role_b";
    EXPECT_TRUE(bc2->connect(bc_cfg));

    api1->set_broker_comm(bc1.get());
    api2->set_broker_comm(bc2.get());

    // Start broker threads (uses start_broker_thread which wires notifications).
    // We need a minimal engine for ThreadEngineGuard — use NativeEngine stub.
    // Actually, start_broker_thread needs engine for on_heartbeat.
    // For this test, run poll loops directly in separate threads.

    std::atomic<bool> running1{true}, running2{true};

    // Wire notifications manually (start_broker_thread does this, but we
    // can't call it without an engine — do it directly).
    bc1->on_notification([&core1](const std::string &type, const nlohmann::json &body) {
        scripting::IncomingMessage msg;
        msg.event = type;
        msg.details = body;
        core1.enqueue_message(std::move(msg));
    });
    bc2->on_notification([&core2](const std::string &type, const nlohmann::json &body) {
        scripting::IncomingMessage msg;
        msg.event = type;
        msg.details = body;
        core2.enqueue_message(std::move(msg));
    });

    std::thread t1([&] { bc1->run_poll_loop([&] { return running1.load(); }); });
    std::thread t2([&] { bc2->run_poll_loop([&] { return running2.load(); }); });

    // Role A joins channel via RoleAPIBase.
    auto join1 = api1->band_join("!api_test_ch");
    EXPECT_TRUE(join1.has_value()) << "band_join failed";

    // Role B joins — Role A should get BAND_JOIN_NOTIFY.
    auto join2 = api2->band_join("!api_test_ch");
    EXPECT_TRUE(join2.has_value());

    // Wait for notification to arrive in core1's message queue.
    bool got_notify = pylabhub::tests::helper::poll_until(
        [&] {
            auto msgs = core1.drain_messages();
            for (const auto &m : msgs)
            {
                if (m.event == "BAND_JOIN_NOTIFY")
                    return true;
            }
            return false;
        },
        std::chrono::seconds{3});
    EXPECT_TRUE(got_notify) << "BAND_JOIN_NOTIFY not received in core1";

    // Role A sends a channel message via RoleAPIBase.
    api1->band_broadcast("!api_test_ch", {{"action", "start"}, {"seq", 1}});

    // Wait for Role B to receive BAND_BROADCAST_NOTIFY.
    bool got_msg = pylabhub::tests::helper::poll_until(
        [&] {
            auto msgs = core2.drain_messages();
            for (const auto &m : msgs)
            {
                if (m.event == "BAND_BROADCAST_NOTIFY")
                    return true;
            }
            return false;
        },
        std::chrono::seconds{3});
    EXPECT_TRUE(got_msg) << "BAND_BROADCAST_NOTIFY not received in core2";

    // Query members via RoleAPIBase.
    auto members = api1->band_members("!api_test_ch");
    EXPECT_TRUE(members.has_value());
    if (members)
    {
        auto mlist = (*members)["members"];
        EXPECT_EQ(mlist.size(), 2u);
    }

    // Leave via RoleAPIBase.
    bool left = api1->band_leave("!api_test_ch");
    EXPECT_TRUE(left);

    running1.store(false);
    running2.store(false);
    bc1->stop();
    bc2->stop();
    t1.join();
    t2.join();
    bc1->disconnect();
    bc2->disconnect();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker: leave notification delivery
// ============================================================================

int channel_leave_notify()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    std::atomic<int> leave_count{0};
    std::string left_uid;
    std::string left_reason;
    std::mutex mu;

    ChannelClient c1, c2;
    c1.ch.on_notification([&](const std::string &type, const nlohmann::json &payload) {
        if (type == "BAND_LEAVE_NOTIFY")
        {
            std::lock_guard<std::mutex> lk(mu);
            left_uid = payload.value("role_uid", "");
            left_reason = payload.value("reason", "");
            leave_count.fetch_add(1);
        }
    });

    c1.connect(broker.endpoint, broker.pubkey, "prod.stayer.uid00000001", "stayer");
    c2.connect(broker.endpoint, broker.pubkey, "prod.leaver.uid00000002", "leaver");

    c1.ch.band_join("!leave_ch", 5000);
    c2.ch.band_join("!leave_ch", 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Leaver leaves.
    c2.ch.band_leave("!leave_ch", 5000);

    bool got = pylabhub::tests::helper::poll_until(
        [&] { return leave_count.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got) << "BAND_LEAVE_NOTIFY never received";
    if (got)
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT_EQ(left_uid, "prod.leaver.uid00000002");
        EXPECT_EQ(left_reason, "voluntary");
    }

    c1.stop();
    c2.stop();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker: sender does not receive own message
// ============================================================================

int channel_self_excluded()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    std::atomic<int> sender_msg_count{0};
    std::atomic<int> receiver_msg_count{0};

    ChannelClient c1, c2;
    c1.ch.on_notification([&](const std::string &type, const nlohmann::json &) {
        if (type == "BAND_BROADCAST_NOTIFY")
            sender_msg_count.fetch_add(1);
    });
    c2.ch.on_notification([&](const std::string &type, const nlohmann::json &) {
        if (type == "BAND_BROADCAST_NOTIFY")
            receiver_msg_count.fetch_add(1);
    });

    c1.connect(broker.endpoint, broker.pubkey, "prod.sender.uid00000001", "sender");
    c2.connect(broker.endpoint, broker.pubkey, "cons.recvr.uid00000002", "receiver");

    c1.ch.band_join("!self_ch", 5000);
    c2.ch.band_join("!self_ch", 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Sender sends a message.
    c1.ch.band_broadcast("!self_ch", {{"ping", true}});

    // Wait for receiver to get it.
    bool got = pylabhub::tests::helper::poll_until(
        [&] { return receiver_msg_count.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got) << "Receiver didn't get message";

    // Give sender some time to potentially receive its own message.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // Sender should NOT have received its own message.
    EXPECT_EQ(sender_msg_count.load(), 0) << "Sender received its own message";

    c1.stop();
    c2.stop();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker: role joins multiple channels independently
// ============================================================================

int channel_multi_channel()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    std::atomic<int> msg_on_ch1{0};
    std::atomic<int> msg_on_ch2{0};

    ChannelClient c1;
    c1.ch.on_notification([&](const std::string &type, const nlohmann::json &payload) {
        if (type == "BAND_BROADCAST_NOTIFY")
        {
            std::string ch = payload.value("channel", "");
            if (ch == "!ch_alpha")
                msg_on_ch1.fetch_add(1);
            else if (ch == "!ch_beta")
                msg_on_ch2.fetch_add(1);
        }
    });

    ChannelClient c2;

    c1.connect(broker.endpoint, broker.pubkey, "prod.multi.uid00000001", "multi_role");
    c2.connect(broker.endpoint, broker.pubkey, "prod.other.uid00000002", "other_role");

    // Role 1 joins both channels.
    c1.ch.band_join("!ch_alpha", 5000);
    c1.ch.band_join("!ch_beta", 5000);

    // Role 2 joins both channels.
    c2.ch.band_join("!ch_alpha", 5000);
    c2.ch.band_join("!ch_beta", 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Role 2 sends to ch_alpha only.
    c2.ch.band_broadcast("!ch_alpha", {{"target", "alpha"}});

    bool got1 = pylabhub::tests::helper::poll_until(
        [&] { return msg_on_ch1.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got1) << "Message on #ch_alpha not received";

    // Verify ch_beta did NOT receive it.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    EXPECT_EQ(msg_on_ch2.load(), 0) << "Message leaked to #ch_beta";

    // Now send to ch_beta.
    c2.ch.band_broadcast("!ch_beta", {{"target", "beta"}});

    bool got2 = pylabhub::tests::helper::poll_until(
        [&] { return msg_on_ch2.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got2) << "Message on #ch_beta not received";

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
                if (scenario == "roleapi_channel")
                    return roleapi_channel();
                if (scenario == "leave_notify")
                    return channel_leave_notify();
                if (scenario == "self_excluded")
                    return channel_self_excluded();
                if (scenario == "multi_channel")
                    return channel_multi_channel();
                return -1;
            });
    }
};

static ChannelGroupWorkerRegistrar s_registrar;

} // anonymous namespace
