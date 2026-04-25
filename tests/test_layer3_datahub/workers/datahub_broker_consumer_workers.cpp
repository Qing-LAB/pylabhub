// tests/test_layer3_datahub/workers/datahub_broker_consumer_workers.cpp
// Consumer registration protocol integration tests.
#include "datahub_broker_consumer_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <cppzmq/zmq.hpp>
#include <cppzmq/zmq_addon.hpp>
#include <zmq.h>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pylabhub::tests::helper;
using namespace pylabhub::broker;
using namespace pylabhub::hub;

namespace pylabhub::tests::worker::broker_consumer
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetDataBlockModule(); }
static auto zmq_module() { return ::pylabhub::hub::GetZMQContextModule(); }

// ============================================================================
// File-local helpers (mirrors datahub_broker_workers.cpp)
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
        {
            thread.join();
        }
    }
};

BrokerHandle start_broker_in_thread(BrokerService::Config cfg)
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto ready_promise = std::make_shared<std::promise<ReadyInfo>>();
    auto ready_future = ready_promise->get_future();

    cfg.on_ready = [ready_promise](const std::string& ep, const std::string& pk)
    { ready_promise->set_value({ep, pk}); };

    auto service = std::make_unique<BrokerService>(std::move(cfg));
    BrokerService* raw_ptr = service.get();
    std::thread t([raw_ptr]() { raw_ptr->run(); });

    auto info = ready_future.get();

    BrokerHandle handle;
    handle.service = std::move(service);
    handle.thread = std::move(t);
    handle.endpoint = info.first;
    handle.pubkey = info.second;
    return handle;
}

nlohmann::json raw_req(const std::string& endpoint,
                       const std::string& msg_type,
                       const nlohmann::json& payload,
                       int timeout_ms = 2000,
                       const std::string& server_pubkey = "")
{
    constexpr size_t kZ85KeyLen = 40;
    constexpr size_t kZ85BufLen = 41;

    zmq::context_t ctx(1);
    zmq::socket_t dealer(ctx, zmq::socket_type::dealer);

    if (server_pubkey.size() == kZ85KeyLen)
    {
        std::array<char, kZ85BufLen> client_pub{};
        std::array<char, kZ85BufLen> client_sec{};
        if (zmq_curve_keypair(client_pub.data(), client_sec.data()) != 0)
        {
            return {};
        }
        dealer.set(zmq::sockopt::curve_serverkey, server_pubkey);
        dealer.set(zmq::sockopt::curve_publickey, std::string(client_pub.data(), kZ85KeyLen));
        dealer.set(zmq::sockopt::curve_secretkey, std::string(client_sec.data(), kZ85KeyLen));
    }

    dealer.connect(endpoint);

    // Frame 0: 'C' (control), Frame 1: type string, Frame 2: JSON body
    static constexpr char kCtrl = 'C';
    const std::string payload_str = payload.dump();
    std::vector<zmq::const_buffer> send_frames = {zmq::buffer(&kCtrl, 1),
                                                  zmq::buffer(msg_type),
                                                  zmq::buffer(payload_str)};
    if (!zmq::send_multipart(dealer, send_frames))
    {
        return {};
    }

    std::vector<zmq::pollitem_t> items = {{dealer.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, std::chrono::milliseconds(timeout_ms));
    if ((items[0].revents & ZMQ_POLLIN) == 0)
    {
        return {};
    }

    std::vector<zmq::message_t> recv_frames;
    auto result = zmq::recv_multipart(dealer, std::back_inserter(recv_frames));
    // Reply layout: ['C', ack_type_string, body_JSON]
    if (!result || recv_frames.size() < 3)
    {
        return {};
    }

    try
    {
        return nlohmann::json::parse(recv_frames.back().to_string());
    }
    catch (const nlohmann::json::exception&)
    {
        return {};
    }
}

std::string zero_hex(size_t bytes = 32) { return std::string(bytes * 2, '0'); }

} // anonymous namespace

// ============================================================================
// consumer_reg_channel_not_found — CONSUMER_REG_REQ for unknown channel → ERROR
// ============================================================================

int consumer_reg_channel_not_found()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            nlohmann::json req;
            req["channel_name"] = "no.such.channel";
            req["consumer_pid"] = uint64_t{12345};
            req["consumer_hostname"] = "test-host";

            nlohmann::json resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", req);
            ASSERT_FALSE(resp.is_null()) << "CONSUMER_REG_REQ timed out";
            EXPECT_EQ(resp.value("status", std::string{}), "error")
                << "CONSUMER_REG_REQ for unknown channel must fail; got: " << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}), "CHANNEL_NOT_FOUND")
                << "Error code must be CHANNEL_NOT_FOUND; got: " << resp.dump();

            broker.stop_and_join();
        },
        "broker_consumer.consumer_reg_channel_not_found",
        logger_module(), zmq_module());
}

// ============================================================================
// consumer_reg_happy_path — BRC register_consumer → CONSUMER_REG_ACK
// ============================================================================

int consumer_reg_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker_consumer.reg_happy";
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            // Register producer via BRC
            BrokerRequestComm prod_brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = prod_uid;
            ASSERT_TRUE(prod_brc.connect(brc_cfg));
            std::atomic<bool> prod_running{true};
            std::thread prod_thread([&] { prod_brc.run_poll_loop([&] { return prod_running.load(); }); });

            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = ::getpid();
            reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_pubkey"]        = "";
            reg_opts["role_uid"]          = prod_uid;
            auto reg = prod_brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            // Heartbeat → Ready
            prod_brc.send_heartbeat(channel, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Register consumer via separate BRC
            BrokerRequestComm cons_brc;
            BrokerRequestComm::Config cons_cfg;
            cons_cfg.broker_endpoint = broker.endpoint;
            cons_cfg.broker_pubkey   = broker.pubkey;
            cons_cfg.role_uid        = cons_uid;
            ASSERT_TRUE(cons_brc.connect(cons_cfg));
            std::atomic<bool> cons_running{true};
            std::thread cons_thread([&] { cons_brc.run_poll_loop([&] { return cons_running.load(); }); });

            nlohmann::json cons_opts;
            cons_opts["channel_name"]  = channel;
            cons_opts["consumer_uid"]  = cons_uid;
            cons_opts["consumer_name"] = "test_consumer";
            cons_opts["consumer_pid"]  = ::getpid();
            auto cons_reg = cons_brc.register_consumer(cons_opts, 3000);
            ASSERT_TRUE(cons_reg.has_value()) << "register_consumer failed";

            // Verify consumer_count via raw DISC_REQ
            nlohmann::json disc_req;
            disc_req["channel_name"] = channel;
            nlohmann::json disc_resp =
                raw_req(broker.endpoint, "DISC_REQ", disc_req, 2000, broker.pubkey);
            ASSERT_FALSE(disc_resp.is_null()) << "DISC_REQ timed out";
            EXPECT_EQ(disc_resp.value("status", std::string{}), "success");
            EXPECT_GE(disc_resp.value("consumer_count", uint32_t{0}), 1u)
                << "DISC_ACK consumer_count must be >= 1 after register_consumer; got: "
                << disc_resp.dump();

            cons_running.store(false);
            cons_brc.stop();
            if (cons_thread.joinable()) cons_thread.join();
            cons_brc.disconnect();
            prod_running.store(false);
            prod_brc.stop();
            if (prod_thread.joinable()) prod_thread.join();
            prod_brc.disconnect();
            broker.stop_and_join();
        },
        "broker_consumer.consumer_reg_happy_path",
        logger_module(), crypto_module(), hub_module(), zmq_module());
}

// ============================================================================
// consumer_dereg_happy_path — register consumer then deregister with correct pid
// ============================================================================

int consumer_dereg_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker_consumer.dereg_happy";
            const uint64_t producer_pid = 55001;
            const uint64_t consumer_pid = 55100;

            // Register channel via raw_req
            nlohmann::json reg_req;
            reg_req["channel_name"] = channel;
            reg_req["shm_name"] = channel + ".shm";
            reg_req["schema_hash"] = zero_hex();
            reg_req["schema_version"] = 1;
            reg_req["producer_pid"] = producer_pid;
            reg_req["producer_hostname"] = "test-host";
            nlohmann::json reg_resp = raw_req(broker.endpoint, "REG_REQ", reg_req);
            ASSERT_FALSE(reg_resp.is_null()) << "REG_REQ timed out";
            ASSERT_EQ(reg_resp.value("status", std::string{}), "success");

            // Send HEARTBEAT_REQ to transition channel from PendingReady → Ready.
            // HEARTBEAT_REQ is fire-and-forget; raw_req will time out (no reply expected).
            nlohmann::json hb_req;
            hb_req["channel_name"] = channel;
            hb_req["producer_pid"] = producer_pid;
            raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100);

            // Register consumer
            nlohmann::json creg_req;
            creg_req["channel_name"] = channel;
            creg_req["consumer_pid"] = consumer_pid;
            creg_req["consumer_hostname"] = "consumer-host";
            nlohmann::json creg_resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", creg_req);
            ASSERT_FALSE(creg_resp.is_null()) << "CONSUMER_REG_REQ timed out";
            EXPECT_EQ(creg_resp.value("status", std::string{}), "success")
                << "CONSUMER_REG_REQ must succeed; got: " << creg_resp.dump();

            // Verify consumer_count == 1
            nlohmann::json disc_req1;
            disc_req1["channel_name"] = channel;
            nlohmann::json disc_resp1 = raw_req(broker.endpoint, "DISC_REQ", disc_req1);
            ASSERT_FALSE(disc_resp1.is_null());
            EXPECT_EQ(disc_resp1.value("consumer_count", uint32_t{99}), 1u)
                << "consumer_count must be 1 after register; got: " << disc_resp1.dump();

            // Deregister consumer with correct pid → success
            nlohmann::json cdereg_req;
            cdereg_req["channel_name"] = channel;
            cdereg_req["consumer_pid"] = consumer_pid;
            nlohmann::json cdereg_resp =
                raw_req(broker.endpoint, "CONSUMER_DEREG_REQ", cdereg_req);
            ASSERT_FALSE(cdereg_resp.is_null()) << "CONSUMER_DEREG_REQ timed out";
            EXPECT_EQ(cdereg_resp.value("status", std::string{}), "success")
                << "CONSUMER_DEREG_REQ must succeed; got: " << cdereg_resp.dump();

            // Verify consumer_count == 0
            nlohmann::json disc_req2;
            disc_req2["channel_name"] = channel;
            nlohmann::json disc_resp2 = raw_req(broker.endpoint, "DISC_REQ", disc_req2);
            ASSERT_FALSE(disc_resp2.is_null());
            EXPECT_EQ(disc_resp2.value("consumer_count", uint32_t{99}), 0u)
                << "consumer_count must be 0 after deregister; got: " << disc_resp2.dump();

            broker.stop_and_join();
        },
        "broker_consumer.consumer_dereg_happy_path",
        logger_module(), zmq_module());
}

// ============================================================================
// consumer_dereg_pid_mismatch — deregister wrong pid → NOT_REGISTERED
// ============================================================================

int consumer_dereg_pid_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker_consumer.dereg_pid_mismatch";
            const uint64_t correct_pid = 56001;
            const uint64_t wrong_pid = 99999;

            // Register channel
            nlohmann::json reg_req;
            reg_req["channel_name"] = channel;
            reg_req["shm_name"] = channel + ".shm";
            reg_req["schema_hash"] = zero_hex();
            reg_req["schema_version"] = 1;
            reg_req["producer_pid"] = uint64_t{56000};
            nlohmann::json reg_resp = raw_req(broker.endpoint, "REG_REQ", reg_req);
            ASSERT_FALSE(reg_resp.is_null()) << "REG_REQ timed out";
            ASSERT_EQ(reg_resp.value("status", std::string{}), "success");

            // Send HEARTBEAT_REQ to transition channel from PendingReady → Ready.
            // HEARTBEAT_REQ is fire-and-forget; raw_req will time out (no reply expected).
            nlohmann::json hb_req;
            hb_req["channel_name"] = channel;
            hb_req["producer_pid"] = uint64_t{56000};
            raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100);

            // Register consumer with correct_pid
            nlohmann::json creg_req;
            creg_req["channel_name"] = channel;
            creg_req["consumer_pid"] = correct_pid;
            nlohmann::json creg_resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", creg_req);
            ASSERT_FALSE(creg_resp.is_null());
            ASSERT_EQ(creg_resp.value("status", std::string{}), "success");

            // Deregister with wrong pid → NOT_REGISTERED
            nlohmann::json cdereg_req;
            cdereg_req["channel_name"] = channel;
            cdereg_req["consumer_pid"] = wrong_pid;
            nlohmann::json cdereg_resp =
                raw_req(broker.endpoint, "CONSUMER_DEREG_REQ", cdereg_req);
            ASSERT_FALSE(cdereg_resp.is_null()) << "CONSUMER_DEREG_REQ timed out";
            EXPECT_EQ(cdereg_resp.value("status", std::string{}), "error")
                << "CONSUMER_DEREG_REQ with wrong pid must be rejected; got: "
                << cdereg_resp.dump();
            EXPECT_EQ(cdereg_resp.value("error_code", std::string{}), "NOT_REGISTERED")
                << "Error code must be NOT_REGISTERED; got: " << cdereg_resp.dump();

            // Consumer with correct pid is still registered
            nlohmann::json disc_req;
            disc_req["channel_name"] = channel;
            nlohmann::json disc_resp = raw_req(broker.endpoint, "DISC_REQ", disc_req);
            ASSERT_FALSE(disc_resp.is_null());
            EXPECT_EQ(disc_resp.value("consumer_count", uint32_t{0}), 1u)
                << "consumer_count must still be 1 after pid-mismatch deregister; got: "
                << disc_resp.dump();

            broker.stop_and_join();
        },
        "broker_consumer.consumer_dereg_pid_mismatch",
        logger_module(), zmq_module());
}

// ============================================================================
// disc_shows_consumer_count — consumer_count in DISC_ACK tracks registrations
// ============================================================================

int disc_shows_consumer_count()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker_consumer.disc_count";
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            // Register producer via BRC
            BrokerRequestComm prod_brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = prod_uid;
            ASSERT_TRUE(prod_brc.connect(brc_cfg));
            std::atomic<bool> prod_running{true};
            std::thread prod_thread([&] { prod_brc.run_poll_loop([&] { return prod_running.load(); }); });

            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = ::getpid();
            reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_pubkey"]        = "";
            reg_opts["role_uid"]          = prod_uid;
            auto reg = prod_brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(reg.has_value());

            prod_brc.send_heartbeat(channel, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            nlohmann::json disc_req;
            disc_req["channel_name"] = channel;

            auto disc = [&]() -> nlohmann::json
            {
                return raw_req(broker.endpoint, "DISC_REQ", disc_req, 2000, broker.pubkey);
            };

            nlohmann::json d0 = disc();
            ASSERT_FALSE(d0.is_null());
            EXPECT_EQ(d0.value("consumer_count", uint32_t{99}), 0u)
                << "consumer_count must start at 0; got: " << d0.dump();

            // Register consumer via BRC
            BrokerRequestComm cons_brc;
            BrokerRequestComm::Config cons_cfg;
            cons_cfg.broker_endpoint = broker.endpoint;
            cons_cfg.broker_pubkey   = broker.pubkey;
            cons_cfg.role_uid        = cons_uid;
            ASSERT_TRUE(cons_brc.connect(cons_cfg));
            std::atomic<bool> cons_running{true};
            std::thread cons_thread([&] { cons_brc.run_poll_loop([&] { return cons_running.load(); }); });

            nlohmann::json cons_opts;
            cons_opts["channel_name"]  = channel;
            cons_opts["consumer_uid"]  = cons_uid;
            cons_opts["consumer_name"] = "test_consumer";
            cons_opts["consumer_pid"]  = ::getpid();
            auto cons_reg = cons_brc.register_consumer(cons_opts, 3000);
            ASSERT_TRUE(cons_reg.has_value());

            nlohmann::json d1 = disc();
            ASSERT_FALSE(d1.is_null());
            EXPECT_EQ(d1.value("consumer_count", uint32_t{0}), 1u)
                << "consumer_count must be 1 after register_consumer; got: " << d1.dump();

            // Deregister consumer
            cons_brc.deregister_consumer(channel);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            nlohmann::json d2 = disc();
            ASSERT_FALSE(d2.is_null());
            EXPECT_EQ(d2.value("consumer_count", uint32_t{1}), 0u)
                << "consumer_count must be 0 after deregister_consumer; got: " << d2.dump();

            cons_running.store(false);
            cons_brc.stop();
            if (cons_thread.joinable()) cons_thread.join();
            cons_brc.disconnect();
            prod_running.store(false);
            prod_brc.stop();
            if (prod_thread.joinable()) prod_thread.join();
            prod_brc.disconnect();
            broker.stop_and_join();
        },
        "broker_consumer.disc_shows_consumer_count",
        logger_module(), crypto_module(), hub_module(), zmq_module());
}

} // namespace pylabhub::tests::worker::broker_consumer

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct BrokerConsumerWorkerRegistrar
{
    BrokerConsumerWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "broker_consumer")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_consumer;
                if (scenario == "consumer_reg_channel_not_found")
                    return consumer_reg_channel_not_found();
                if (scenario == "consumer_reg_happy_path")
                    return consumer_reg_happy_path();
                if (scenario == "consumer_dereg_happy_path")
                    return consumer_dereg_happy_path();
                if (scenario == "consumer_dereg_pid_mismatch")
                    return consumer_dereg_pid_mismatch();
                if (scenario == "disc_shows_consumer_count")
                    return disc_shows_consumer_count();
                fmt::print(stderr, "ERROR: Unknown broker_consumer scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerConsumerWorkerRegistrar g_broker_consumer_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
