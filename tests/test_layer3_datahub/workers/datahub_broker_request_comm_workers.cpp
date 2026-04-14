/**
 * @file datahub_broker_request_comm_workers.cpp
 * @brief L3 test workers for BrokerRequestComm.
 *
 * Each worker runs in a subprocess (IsolatedProcessTest pattern).
 * Tests exercise the full BrokerRequestComm against a real BrokerService.
 */

#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_sync_utils.h"

#include "utils/broker_request_comm.hpp"
#include "utils/logger.hpp"
#include "utils/broker_service.hpp"
#include "utils/lifecycle.hpp"
#include "utils/zmq_context.hpp"
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

// ============================================================================
// Broker helper
// ============================================================================

namespace
{

struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread thread;
    std::string endpoint;
    std::string pubkey;

    void stop_and_join()
    {
        service->stop();
        if (thread.joinable())
            thread.join();
    }
};

BrokerHandle start_broker()
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto ready_promise = std::make_shared<std::promise<ReadyInfo>>();
    auto ready_future  = ready_promise->get_future();

    BrokerService::Config bcfg;
    bcfg.endpoint = "tcp://127.0.0.1:0";
    bcfg.use_curve = true;
    bcfg.on_ready = [ready_promise](const std::string &ep, const std::string &pk)
    { ready_promise->set_value({ep, pk}); };

    auto svc = std::make_unique<BrokerService>(std::move(bcfg));
    auto *raw = svc.get();
    std::thread t([raw] { raw->run(); });

    auto info = ready_future.get();
    return {std::move(svc), std::move(t), info.first, info.second};
}

// ============================================================================
// Worker functions
// ============================================================================

int connect_and_heartbeat()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    EXPECT_TRUE(ch.connect(cfg));
    EXPECT_TRUE(ch.is_connected());

    std::atomic<bool> running{true};
    std::thread poll_thread([&] {
        ch.run_poll_loop([&] { return running.load(); });
    });

    ch.send_heartbeat("test_channel", {{"test_key", 42}});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    running.store(false);
    ch.stop();
    poll_thread.join();
    ch.disconnect();

    EXPECT_FALSE(ch.is_connected());
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

int register_and_discover()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    EXPECT_TRUE(ch.connect(cfg));

    std::atomic<bool> running{true};
    std::thread poll_thread([&] {
        ch.run_poll_loop([&] { return running.load(); });
    });

    // Register a channel.
    nlohmann::json reg_opts;
    reg_opts["channel_name"]      = "test_ch";
    reg_opts["pattern"]           = "PubSub";
    reg_opts["has_shared_memory"] = false;
    reg_opts["producer_pid"]      = 12345;
    reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_pubkey"]        = "";
    reg_opts["role_uid"]          = "test_prod_001";
    reg_opts["role_name"]         = "test_producer";

    auto reg_result = ch.register_channel(reg_opts, 5000);
    EXPECT_TRUE(reg_result.has_value()) << "REG_REQ timed out";
    if (reg_result)
        EXPECT_EQ(reg_result->value("status", ""), "success");

    // Send heartbeat so broker marks channel as Ready (required before DISC_REQ).
    ch.send_heartbeat("test_ch", {});
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Discover the channel.
    nlohmann::json disc_opts;
    disc_opts["consumer_uid"]  = "test_cons_001";
    disc_opts["consumer_name"] = "test_consumer";

    auto disc_result = ch.discover_channel("test_ch", disc_opts, 5000);
    EXPECT_TRUE(disc_result.has_value()) << "DISC_REQ timed out";
    if (disc_result)
        EXPECT_EQ(disc_result->value("status", ""), "success");

    // List channels.
    auto channels = ch.list_channels(5000);
    EXPECT_GE(channels.size(), 1u);

    running.store(false);
    ch.stop();
    poll_thread.join();
    ch.disconnect();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

int role_presence()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    EXPECT_TRUE(ch.connect(cfg));

    std::atomic<bool> running{true};
    std::thread poll_thread([&] {
        ch.run_poll_loop([&] { return running.load(); });
    });

    // Not present yet.
    bool present = ch.query_role_presence("nonexistent_uid", 3000);
    EXPECT_FALSE(present);

    // Register a channel so a role exists.
    nlohmann::json reg_opts;
    reg_opts["channel_name"]      = "presence_ch";
    reg_opts["pattern"]           = "PubSub";
    reg_opts["has_shared_memory"] = false;
    reg_opts["producer_pid"]      = 12345;
    reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_pubkey"]        = "";
    reg_opts["role_uid"]          = "prod_uid_42";
    reg_opts["role_name"]         = "my_producer";

    auto reg = ch.register_channel(reg_opts, 5000);
    EXPECT_TRUE(reg.has_value());

    // Now query — should be present.
    present = ch.query_role_presence("prod_uid_42", 3000);
    EXPECT_TRUE(present);

    running.store(false);
    ch.stop();
    poll_thread.join();
    ch.disconnect();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

int notification_dispatch()
{
    auto mods = utils::MakeModDefList(logger_module(), crypto_module(), hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    auto broker = start_broker();

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    EXPECT_TRUE(ch.connect(cfg));

    std::atomic<int> notify_count{0};
    std::string last_notify_type;
    ch.on_notification([&](const std::string &type, const nlohmann::json &) {
        last_notify_type = type;
        notify_count.fetch_add(1);
    });

    std::atomic<bool> running{true};
    std::thread poll_thread([&] {
        ch.run_poll_loop([&] { return running.load(); });
    });

    // Register a channel.
    nlohmann::json reg_opts;
    reg_opts["channel_name"]      = "notify_ch";
    reg_opts["pattern"]           = "PubSub";
    reg_opts["has_shared_memory"] = false;
    reg_opts["producer_pid"]      = 12345;
    reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_pubkey"]        = "";
    reg_opts["role_uid"]          = "notify_prod";
    reg_opts["role_name"]         = "notify_producer";

    auto reg = ch.register_channel(reg_opts, 5000);
    EXPECT_TRUE(reg.has_value());

    // Request broker to close the channel → should trigger CHANNEL_CLOSING_NOTIFY.
    broker.service->request_close_channel("notify_ch");

    bool got = pylabhub::tests::helper::poll_until(
        [&] { return notify_count.load() > 0; },
        std::chrono::seconds{3});
    EXPECT_TRUE(got) << "CHANNEL_CLOSING_NOTIFY never received";
    if (got)
        EXPECT_EQ(last_notify_type, "CHANNEL_CLOSING_NOTIFY");

    running.store(false);
    ch.stop();
    poll_thread.join();
    ch.disconnect();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

// ============================================================================
// Worker dispatcher
// ============================================================================

struct BrokerReqChannelWorkerRegistrar
{
    BrokerReqChannelWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_req")
                    return -1;
                std::string scenario(mode.substr(dot + 1));

                if (scenario == "connect_and_heartbeat")
                    return connect_and_heartbeat();
                if (scenario == "register_and_discover")
                    return register_and_discover();
                if (scenario == "role_presence")
                    return role_presence();
                if (scenario == "notification_dispatch")
                    return notification_dispatch();
                return -1;
            });
    }
};

static BrokerReqChannelWorkerRegistrar s_registrar;

} // anonymous namespace
