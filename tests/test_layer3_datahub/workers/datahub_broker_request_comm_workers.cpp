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
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/lifecycle.hpp"
#include "utils/zmq_context.hpp"
#include "plh_datahub.hpp"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

using namespace pylabhub;
using namespace pylabhub::hub;
using namespace pylabhub::broker;

static auto logger_module()    { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto file_lock_module() { return ::pylabhub::utils::FileLock::GetLifecycleModule(); }
static auto json_module()      { return ::pylabhub::utils::JsonConfig::GetLifecycleModule(); }
static auto crypto_module()    { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()       { return ::pylabhub::hub::GetDataBlockModule(); }
static auto zmq_module()       { return ::pylabhub::hub::GetZMQContextModule(); }

// ============================================================================
// Broker helper — real HubHost wrapper (replaces legacy mock per
// docs/todo/TESTING_TODO.md §"Test Design Principles")
// ============================================================================

namespace fs = std::filesystem;

namespace
{

struct BrokerHandle
{
    fs::path                                       hub_dir;
    std::unique_ptr<::pylabhub::hub_host::HubHost> host;
    std::string                                    endpoint;
    std::string                                    pubkey;

    ~BrokerHandle() { stop_and_join(); }

    BrokerHandle()                                = default;
    BrokerHandle(const BrokerHandle &)            = delete;
    BrokerHandle &operator=(const BrokerHandle &) = delete;
    BrokerHandle(BrokerHandle &&)                 = default;
    BrokerHandle &operator=(BrokerHandle &&)      = default;

    void stop_and_join()
    {
        if (host)
            host->shutdown();
        host.reset();
        if (!hub_dir.empty())
        {
            std::error_code ec;
            fs::remove_all(hub_dir, ec);
            hub_dir.clear();
        }
    }

    BrokerService &service_ref() { return host->broker(); }
};

BrokerHandle start_broker()
{
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_brc_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    ::pylabhub::utils::HubDirectory::init_directory(dir, "BrcTestHub");

    const fs::path hub_json = dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json);
        if (f.is_open())
            j = nlohmann::json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";
    // HEP-CORE-0023 §2.1 atomic teardown: voluntary close emits
    // CHANNEL_CLOSING_NOTIFY (best-effort) and removes the channel
    // entry in the same handler — no grace window.
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
    fs::create_directories(dir / "schemas");

    BrokerHandle h;
    h.hub_dir = std::move(dir);
    h.host    = std::make_unique<::pylabhub::hub_host::HubHost>(
        ::pylabhub::config::HubConfig::load_from_directory(h.hub_dir.string()));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = h.host->broker_pubkey();
    return h;
}

// ============================================================================
// Worker functions
// ============================================================================

int connect_and_heartbeat()
{
    auto mods = utils::MakeModDefList(logger_module(), file_lock_module(),
                                          json_module(), crypto_module(),
                                          hub_module(), zmq_module());
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

    // Wire-format probe: per HEP-CORE-0019 §4.1 (Phase 6),
    // HEARTBEAT_REQ requires (channel, uid, role_type).
    ch.send_heartbeat("test_channel", "prod.test.uid00000001", "producer",
                      {{"test_key", 42}});
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
    auto mods = utils::MakeModDefList(logger_module(), file_lock_module(),
                                          json_module(), crypto_module(),
                                          hub_module(), zmq_module());
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
    reg_opts["role_uid"]          = "prod.test.uid00000001";
    reg_opts["role_name"]         = "test_producer";

    auto reg_result = ch.register_channel(reg_opts, 5000);
    EXPECT_TRUE(reg_result.has_value()) << "REG_REQ timed out";
    if (reg_result)
        EXPECT_EQ(reg_result->value("status", ""), "success");

    // Send heartbeat so broker marks channel as Ready (required before DISC_REQ).
    // Per HEP-CORE-0019 §4.1 (Phase 6) — wire format requires (channel, uid, role_type).
    ch.send_heartbeat("test_ch", "prod.test.uid00000001", "producer", {});
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Discover the channel.
    nlohmann::json disc_opts;
    disc_opts["consumer_uid"]  = "cons.test.uid00000001";
    disc_opts["consumer_name"] = "test_consumer";

    auto disc_result = ch.discover_channel("test_ch", disc_opts, 5000);
    EXPECT_TRUE(disc_result.has_value()) << "DISC_REQ timed out";
    if (disc_result)
        EXPECT_EQ(disc_result->value("status", ""), "success");

    // List channels — post-Bucket-C contract: returns optional<json>
    // carrying the broker's response body or nullopt on transport failure.
    auto list_resp = ch.list_channels(5000);
    EXPECT_TRUE(list_resp.has_value());
    if (list_resp.has_value())
    {
        auto channels = list_resp->value("channels", nlohmann::json::array());
        EXPECT_GE(channels.size(), 1u);
    }

    running.store(false);
    ch.stop();
    poll_thread.join();
    ch.disconnect();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

int role_presence()
{
    auto mods = utils::MakeModDefList(logger_module(), file_lock_module(),
                                          json_module(), crypto_module(),
                                          hub_module(), zmq_module());
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

    // Not present yet.  Post-Bucket-C: query_role_presence returns
    // optional<json>; "present" is the response body field that signals
    // the role was found.
    auto presence_resp = ch.query_role_presence("nonexistent_uid", 3000);
    EXPECT_TRUE(presence_resp.has_value());
    if (presence_resp.has_value())
        EXPECT_FALSE(presence_resp->value("present", false));

    // Register a channel so a role exists.
    nlohmann::json reg_opts;
    reg_opts["channel_name"]      = "presence_ch";
    reg_opts["pattern"]           = "PubSub";
    reg_opts["has_shared_memory"] = false;
    reg_opts["producer_pid"]      = 12345;
    reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    reg_opts["zmq_pubkey"]        = "";
    reg_opts["role_uid"]          = "prod.my.uid00000042";
    reg_opts["role_name"]         = "my_producer";

    auto reg = ch.register_channel(reg_opts, 5000);
    EXPECT_TRUE(reg.has_value());

    // Now query — should be present.
    presence_resp = ch.query_role_presence("prod.my.uid00000042", 3000);
    EXPECT_TRUE(presence_resp.has_value());
    if (presence_resp.has_value())
        EXPECT_TRUE(presence_resp->value("present", false));

    running.store(false);
    ch.stop();
    poll_thread.join();
    ch.disconnect();
    broker.stop_and_join();
    return ::testing::Test::HasFailure() ? 1 : 0;
}

int notification_dispatch()
{
    auto mods = utils::MakeModDefList(logger_module(), file_lock_module(),
                                          json_module(), crypto_module(),
                                          hub_module(), zmq_module());
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
    reg_opts["role_uid"]          = "prod.notify.uid00000001";
    reg_opts["role_name"]         = "notify_producer";

    auto reg = ch.register_channel(reg_opts, 5000);
    EXPECT_TRUE(reg.has_value());

    // Request broker to close the channel → should trigger CHANNEL_CLOSING_NOTIFY.
    broker.service_ref().request_close_channel("notify_ch");

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
