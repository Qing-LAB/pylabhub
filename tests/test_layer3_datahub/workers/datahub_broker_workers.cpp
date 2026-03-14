// tests/test_layer3_datahub/workers/datahub_broker_workers.cpp
// Phase C — BrokerService integration tests.
#include "datahub_broker_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_service.hpp"
#include "channel_registry.hpp"
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

namespace pylabhub::tests::worker::broker
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// File-local helpers
// ============================================================================

namespace
{

// -----------------------------------------------------------------------------
// BrokerHandle: owns BrokerService + its background thread.
// start_broker_in_thread() blocks until on_ready fires (broker is bound).
// -----------------------------------------------------------------------------
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
    using ReadyInfo = std::pair<std::string, std::string>; // (endpoint, pubkey)
    auto ready_promise = std::make_shared<std::promise<ReadyInfo>>();
    auto ready_future = ready_promise->get_future();

    cfg.on_ready = [ready_promise](const std::string& ep, const std::string& pk)
    {
        ready_promise->set_value({ep, pk});
    };

    auto service = std::make_unique<BrokerService>(std::move(cfg));
    BrokerService* raw_ptr = service.get();
    std::thread t([raw_ptr]() { raw_ptr->run(); });

    auto info = ready_future.get(); // blocks until broker is bound and listening

    BrokerHandle handle;
    handle.service = std::move(service);
    handle.thread = std::move(t);
    handle.endpoint = info.first;
    handle.pubkey = info.second;
    return handle;
}

// -----------------------------------------------------------------------------
// raw_req: Sends a two-frame [msg_type, payload_json] to a DEALER socket and
// returns the parsed response body JSON.  Optionally enables CurveZMQ when
// server_pubkey is a 40-char Z85 string.
//
// Returns {} (null JSON) on timeout or receive error.
// -----------------------------------------------------------------------------
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
        // Generate ephemeral client keypair for this request.
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
        return {}; // timeout
    }

    std::vector<zmq::message_t> recv_frames;
    auto result = zmq::recv_multipart(dealer, std::back_inserter(recv_frames));
    // Reply layout: ['C', ack_type_string, body_JSON]
    if (!result || recv_frames.size() < 3)
    {
        return {};
    }

    // recv_frames[0] = 'C', recv_frames[1] = ack_type, recv_frames[2] = body JSON
    try
    {
        return nlohmann::json::parse(recv_frames.back().to_string());
    }
    catch (const nlohmann::json::exception&)
    {
        return {};
    }
}

// Hex string of N zero bytes (for use as a schema_hash in JSON payloads).
std::string zero_hex(size_t bytes = 32)
{
    return std::string(bytes * 2, '0');
}

// Hex string of N 'aa' bytes (for a different schema_hash).
std::string aa_hex(size_t bytes = 32)
{
    return std::string(bytes * 2, 'a');
}

} // anonymous namespace

// ============================================================================
// channel_registry_ops — pure ChannelRegistry unit test (no ZMQ, no lifecycle)
// ============================================================================

int channel_registry_ops()
{
    return run_worker_bare(
        []()
        {
            ChannelRegistry reg;

            // Initially empty
            EXPECT_EQ(reg.size(), 0u);
            EXPECT_TRUE(reg.list_channels().empty());

            // Register "ch1" → succeeds
            ChannelEntry e1;
            e1.shm_name = "shm_ch1";
            e1.schema_hash = zero_hex();
            e1.schema_version = 1;
            e1.producer_pid = 1001;
            EXPECT_TRUE(reg.register_channel("ch1", e1));
            EXPECT_EQ(reg.size(), 1u);

            // find "ch1" → present
            auto found = reg.find_channel("ch1");
            ASSERT_TRUE(found.has_value());
            EXPECT_EQ(found->shm_name, "shm_ch1");

            // find "ch2" → nullopt
            EXPECT_FALSE(reg.find_channel("ch2").has_value());

            // Re-register "ch1" same hash → allowed (producer restart)
            ChannelEntry e1b = e1;
            e1b.producer_pid = 1002;
            EXPECT_TRUE(reg.register_channel("ch1", e1b));
            EXPECT_EQ(reg.size(), 1u);

            // Re-register "ch1" different hash → SCHEMA_MISMATCH (returns false)
            ChannelEntry e1c = e1;
            e1c.schema_hash = aa_hex();
            EXPECT_FALSE(reg.register_channel("ch1", e1c));
            EXPECT_EQ(reg.size(), 1u); // still registered

            // Deregister "ch1" with wrong pid → false; channel still present
            EXPECT_FALSE(reg.deregister_channel("ch1", 9999));
            EXPECT_TRUE(reg.find_channel("ch1").has_value());

            // Deregister "ch1" with correct pid (1002, from the re-registration) → true
            EXPECT_TRUE(reg.deregister_channel("ch1", 1002));
            EXPECT_FALSE(reg.find_channel("ch1").has_value());
            EXPECT_EQ(reg.size(), 0u);

            // list_channels() and size() after multiple ops
            ChannelEntry e2;
            e2.shm_name = "shm_ch2";
            e2.schema_hash = zero_hex();
            e2.schema_version = 2;
            e2.producer_pid = 2001;
            ChannelEntry e3;
            e3.shm_name = "shm_ch3";
            e3.schema_hash = zero_hex();
            e3.schema_version = 3;
            e3.producer_pid = 3001;
            reg.register_channel("ch2", e2);
            reg.register_channel("ch3", e3);
            EXPECT_EQ(reg.size(), 2u);
            auto names = reg.list_channels();
            EXPECT_EQ(names.size(), 2u);
        },
        "broker.channel_registry_ops");
}

// ============================================================================
// broker_reg_disc_happy_path — full REG/DISC round-trip via Messenger
// ============================================================================

int broker_reg_disc_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey))
                << "Messenger::connect() to real BrokerService failed";

            ProducerInfo pinfo;
            pinfo.shm_name = "broker_reg_disc.shm";
            pinfo.producer_pid = pylabhub::platform::get_pid();
            pinfo.schema_hash.assign(32, '\0');
            pinfo.schema_version = 7;
            messenger.register_producer("broker.ch1", pinfo);

            // discover_producer is queued after register_producer on the same worker thread,
            // so DISC_REQ is sent only after REG_ACK is received — no sleep() needed.
            auto info = messenger.discover_producer("broker.ch1", 5000);
            ASSERT_TRUE(info.has_value()) << "discover_producer must find registered channel";
            EXPECT_EQ(info->shm_name, "broker_reg_disc.shm");
            EXPECT_EQ(info->schema_version, 7u);

            messenger.disconnect();
            broker.stop_and_join();
        },
        "broker.broker_reg_disc_happy_path",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// broker_schema_mismatch — re-register same channel with different schema_hash
// ============================================================================

int broker_schema_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.mismatch.ch";
            const uint64_t pid = pylabhub::platform::get_pid();

            // First registration — succeeds
            nlohmann::json req1;
            req1["channel_name"] = channel;
            req1["shm_name"] = "shm_mismatch";
            req1["schema_hash"] = zero_hex();
            req1["schema_version"] = 1;
            req1["producer_pid"] = pid;
            req1["producer_hostname"] = "localhost";

            nlohmann::json resp1 = raw_req(broker.endpoint, "REG_REQ", req1);
            ASSERT_FALSE(resp1.is_null()) << "raw_req timed out on first REG_REQ";
            EXPECT_EQ(resp1.value("status", std::string("")), "success")
                << "First registration must succeed; got: " << resp1.dump();

            // Second registration — different schema_hash → SCHEMA_MISMATCH
            nlohmann::json req2 = req1;
            req2["schema_hash"] = aa_hex(); // different hash
            nlohmann::json resp2 = raw_req(broker.endpoint, "REG_REQ", req2);
            ASSERT_FALSE(resp2.is_null()) << "raw_req timed out on second REG_REQ";
            EXPECT_EQ(resp2.value("status", std::string("")), "error")
                << "Second registration with mismatched hash must be rejected";
            EXPECT_EQ(resp2.value("error_code", std::string("")), "SCHEMA_MISMATCH")
                << "Error code must be SCHEMA_MISMATCH; got: " << resp2.dump();

            broker.stop_and_join();
        },
        "broker.broker_schema_mismatch",
        logger_module());
}

// ============================================================================
// broker_channel_not_found — discover unknown channel → Messenger returns nullopt
// ============================================================================

int broker_channel_not_found()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Broker returns ERROR/CHANNEL_NOT_FOUND; Messenger maps that to nullopt.
            auto info = messenger.discover_producer("no.such.channel", 2000);
            EXPECT_FALSE(info.has_value())
                << "discover_producer for unknown channel must return nullopt";

            messenger.disconnect();
            broker.stop_and_join();
        },
        "broker.broker_channel_not_found",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// broker_dereg_happy_path — register, deregister (correct pid), then not found
// ============================================================================

int broker_dereg_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = "broker.dereg.ch";
            const uint64_t pid = pylabhub::platform::get_pid();

            ProducerInfo pinfo;
            pinfo.shm_name = "broker_dereg.shm";
            pinfo.producer_pid = pid;
            pinfo.schema_hash.assign(32, '\0');
            pinfo.schema_version = 3;
            messenger.register_producer(channel, pinfo);

            // Verify channel is discoverable after registration.
            auto found = messenger.discover_producer(channel, 5000);
            ASSERT_TRUE(found.has_value()) << "Channel must be registered before deregister";

            // Send DEREG_REQ with the correct producer_pid via raw ZMQ with curve.
            nlohmann::json dereg_req;
            dereg_req["channel_name"] = channel;
            dereg_req["producer_pid"] = pid;
            nlohmann::json dereg_resp = raw_req(broker.endpoint, "DEREG_REQ", dereg_req,
                                                2000, broker.pubkey);
            ASSERT_FALSE(dereg_resp.is_null()) << "raw_req for DEREG_REQ timed out";
            EXPECT_EQ(dereg_resp.value("status", std::string("")), "success")
                << "DEREG_REQ with correct pid must succeed; got: " << dereg_resp.dump();

            // After deregistration, discover must return nullopt.
            auto after_dereg = messenger.discover_producer(channel, 1000);
            EXPECT_FALSE(after_dereg.has_value())
                << "discover_producer must return nullopt after deregistration";

            messenger.disconnect();
            broker.stop_and_join();
        },
        "broker.broker_dereg_happy_path",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// broker_dereg_pid_mismatch — deregister with wrong pid → NOT_REGISTERED,
//                             channel still discoverable
// ============================================================================

int broker_dereg_pid_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.pid_mismatch.ch";
            const uint64_t correct_pid = 55555;
            const uint64_t wrong_pid = 99999;

            // Register via raw ZMQ.
            nlohmann::json reg_req;
            reg_req["channel_name"] = channel;
            reg_req["shm_name"] = "shm_pid_mismatch";
            reg_req["schema_hash"] = zero_hex();
            reg_req["schema_version"] = 1;
            reg_req["producer_pid"] = correct_pid;
            reg_req["producer_hostname"] = "localhost";
            nlohmann::json reg_resp = raw_req(broker.endpoint, "REG_REQ", reg_req);
            ASSERT_FALSE(reg_resp.is_null()) << "REG_REQ timed out";
            EXPECT_EQ(reg_resp.value("status", std::string("")), "success");

            // Send HEARTBEAT_REQ to transition channel from PendingReady → Ready.
            // HEARTBEAT_REQ is fire-and-forget (broker sends no reply); raw_req times
            // out quickly and returns empty json, which we discard.
            nlohmann::json hb_req;
            hb_req["channel_name"] = channel;
            hb_req["producer_pid"] = correct_pid;
            raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100);

            // DEREG_REQ with wrong pid → NOT_REGISTERED error.
            nlohmann::json dereg_req;
            dereg_req["channel_name"] = channel;
            dereg_req["producer_pid"] = wrong_pid;
            nlohmann::json dereg_resp = raw_req(broker.endpoint, "DEREG_REQ", dereg_req);
            ASSERT_FALSE(dereg_resp.is_null()) << "DEREG_REQ timed out";
            EXPECT_EQ(dereg_resp.value("status", std::string("")), "error")
                << "DEREG_REQ with wrong pid must be rejected; got: " << dereg_resp.dump();
            EXPECT_EQ(dereg_resp.value("error_code", std::string("")), "NOT_REGISTERED")
                << "Error code must be NOT_REGISTERED; got: " << dereg_resp.dump();

            // Channel still discoverable via DISC_REQ.
            nlohmann::json disc_req;
            disc_req["channel_name"] = channel;
            nlohmann::json disc_resp = raw_req(broker.endpoint, "DISC_REQ", disc_req);
            ASSERT_FALSE(disc_resp.is_null()) << "DISC_REQ timed out";
            EXPECT_EQ(disc_resp.value("status", std::string("")), "success")
                << "Channel must still be registered after pid-mismatch deregister attempt";
            EXPECT_EQ(disc_resp.value("shm_name", std::string("")), "shm_pid_mismatch");

            broker.stop_and_join();
        },
        "broker.broker_dereg_pid_mismatch",
        logger_module());
}

} // namespace pylabhub::tests::worker::broker

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct BrokerWorkerRegistrar
{
    BrokerWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "broker")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker;
                if (scenario == "channel_registry_ops")
                    return channel_registry_ops();
                if (scenario == "broker_reg_disc_happy_path")
                    return broker_reg_disc_happy_path();
                if (scenario == "broker_schema_mismatch")
                    return broker_schema_mismatch();
                if (scenario == "broker_channel_not_found")
                    return broker_channel_not_found();
                if (scenario == "broker_dereg_happy_path")
                    return broker_dereg_happy_path();
                if (scenario == "broker_dereg_pid_mismatch")
                    return broker_dereg_pid_mismatch();
                fmt::print(stderr, "ERROR: Unknown broker scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerWorkerRegistrar g_broker_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
