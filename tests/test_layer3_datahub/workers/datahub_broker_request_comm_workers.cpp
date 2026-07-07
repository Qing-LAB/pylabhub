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
#include "broker_test_harness.h"
#include "curve_test_setup.h"   // role_keystore_name

#include "utils/broker_request_comm.hpp"
#include "utils/logger.hpp"
#include "utils/file_lock.hpp"
#include "utils/json_config.hpp"
#include "utils/lifecycle.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/security/shm_capability_channel.hpp" // #281 default_shm_capability_endpoint
#include "utils/zmq_context.hpp"
#include "plh_datahub.hpp"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pylabhub;
using namespace pylabhub::hub;
using namespace pylabhub::broker;

static auto logger_module()    { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto file_lock_module() { return ::pylabhub::utils::FileLock::GetLifecycleModule(); }
static auto json_module()      { return ::pylabhub::utils::JsonConfig::GetLifecycleModule(); }
static auto hub_module()       { return ::pylabhub::hub::GetDataBlockModule(); }
static auto zmq_module()       { return ::pylabhub::hub::GetZMQContextModule(); }

// ============================================================================
// Test-fixture broker setup
// ============================================================================
//
// Broker comes up via the shared `start_hubhost_broker` harness —
// real `HubHost` (HEP-CORE-0033 production path), real CURVE +
// admission per HEP-CORE-0035 §2 + §4.6.5.  Each fixture below
// declares its role uids up front; the harness writes a
// `vault/known_roles.json` admitting each, and the BRC client
// connects with its matching keypair.

namespace
{

// Shared hub.json overrides — every fixture in this file wants the
// same shape (ephemeral port, admin off, no script).
nlohmann::json brc_hub_overrides()
{
    return nlohmann::json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
}

} // anon namespace

// ============================================================================
// Worker functions
// ============================================================================

int connect_and_heartbeat()
{
    auto mods = utils::MakeModDefList(logger_module(), file_lock_module(),
                                          json_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                                          hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    const std::string uid = "prod.brc.uid00000001";
    auto curve = pylabhub::tests::make_curve_setup({uid});
    pylabhub::tests::seed_curve_identities(curve);
    auto broker = pylabhub::tests::start_hubhost_broker(brc_hub_overrides(), curve);

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    cfg.keystore_name   = pylabhub::tests::role_keystore_name(uid);
    cfg.role_uid        = uid;
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
                                          json_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                                          hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    const std::string uid = "prod.test.uid00000001";
    auto curve = pylabhub::tests::make_curve_setup({uid});
    pylabhub::tests::seed_curve_identities(curve);
    auto broker = pylabhub::tests::start_hubhost_broker(brc_hub_overrides(), curve);

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    cfg.keystore_name   = pylabhub::tests::role_keystore_name(uid);
    cfg.role_uid        = uid;
    EXPECT_TRUE(ch.connect(cfg));

    std::atomic<bool> running{true};
    std::thread poll_thread([&] {
        ch.run_poll_loop([&] { return running.load(); });
    });

    // Register a channel.
    auto reg_opts = pylabhub::hub::build_producer_reg_payload(
        // #281 (2026-06-23): `data_transport` REQUIRED — SHM wire shape
        // mirrors production (HEP-CORE-0041 §5.1).  Endpoint string
        // only; no L2 listener bound here.
        pylabhub::hub::ProducerRegInputs{
            .channel    = "test_ch",
            .role_uid   = "prod.test.uid00000001",
            .role_name  = "test_producer",
            .role_type   = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = curve.role(uid).public_z85,
            .shm_capability_endpoint =
                pylabhub::utils::security::default_shm_capability_endpoint("test_ch"),
        });

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
    disc_opts["role_uid"]  = "cons.test.uid00000001";
    disc_opts["role_name"] = "test_consumer";

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
                                          json_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                                          hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    const std::string uid = "prod.my.uid00000042";
    auto curve = pylabhub::tests::make_curve_setup({uid});
    pylabhub::tests::seed_curve_identities(curve);
    auto broker = pylabhub::tests::start_hubhost_broker(brc_hub_overrides(), curve);

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    cfg.keystore_name   = pylabhub::tests::role_keystore_name(uid);
    cfg.role_uid        = uid;
    EXPECT_TRUE(ch.connect(cfg));

    std::atomic<bool> running{true};
    std::thread poll_thread([&] {
        ch.run_poll_loop([&] { return running.load(); });
    });

    // Not present yet.  Post-Bucket-C: query_role_presence returns
    // optional<json>; "present" is the response body field that signals
    // the role was found.  broker_proto 5 (R3.5b) requires the query
    // uid to be well-formed (HEP-CORE-0033 §G2.2.0b); using a
    // properly-tagged uid that simply isn't registered.
    auto presence_resp = ch.query_role_presence("prod.nonexistent.uid00000001", 3000);
    EXPECT_TRUE(presence_resp.has_value());
    if (presence_resp.has_value())
        EXPECT_FALSE(presence_resp->value("present", false));

    // Register a channel so a role exists.  #281 (2026-06-23):
    // `data_transport` REQUIRED — SHM wire shape (HEP-CORE-0041 §5.1).
    auto reg_opts = pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = "presence_ch",
            .role_uid   = "prod.my.uid00000042",
            .role_name  = "my_producer",
            .role_type   = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = curve.role(uid).public_z85,
            .shm_capability_endpoint =
                pylabhub::utils::security::default_shm_capability_endpoint("presence_ch"),
        });

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
                                          json_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
                                          hub_module(), zmq_module());
    utils::LifecycleGuard guard(std::move(mods));

    const std::string uid = "prod.notify.uid00000001";
    auto curve = pylabhub::tests::make_curve_setup({uid});
    pylabhub::tests::seed_curve_identities(curve);
    auto broker = pylabhub::tests::start_hubhost_broker(brc_hub_overrides(), curve);

    BrokerRequestComm ch;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = broker.endpoint;
    cfg.broker_pubkey   = broker.pubkey;
    cfg.keystore_name   = pylabhub::tests::role_keystore_name(uid);
    cfg.role_uid        = uid;
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

    // Register a channel.  #281 (2026-06-23): `data_transport` REQUIRED
    // — SHM wire shape (HEP-CORE-0041 §5.1).
    auto reg_opts = pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = "notify_ch",
            .role_uid   = "prod.notify.uid00000001",
            .role_name  = "notify_producer",
            .role_type   = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = curve.role(uid).public_z85,
            .shm_capability_endpoint =
                pylabhub::utils::security::default_shm_capability_endpoint("notify_ch"),
        });

    auto reg = ch.register_channel(reg_opts, 5000);
    EXPECT_TRUE(reg.has_value());

    // Request broker to close the channel → should trigger CHANNEL_CLOSING_NOTIFY.
    broker.service().request_close_channel("notify_ch");

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
