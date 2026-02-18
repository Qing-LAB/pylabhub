// tests/test_layer3_datahub/workers/messagehub_workers.cpp
// Phase C – Messenger unit tests (no broker and with in-process broker).
#include "datahub_messagehub_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <cppzmq/zmq.hpp>
#include <cppzmq/zmq_addon.hpp>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

// Note: create_datablock_producer_impl / find_datablock_consumer_impl are declared in
// data_block.hpp (plh_datahub.hpp) without hub parameter. No local forward declarations needed.

namespace pylabhub::tests::worker::messagehub
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

int lifecycle_initialized_follows_state()
{
    return run_gtest_worker(
        []()
        {
            EXPECT_TRUE(pylabhub::hub::lifecycle_initialized());
        },
        "messagehub.lifecycle_initialized_follows_state",
        logger_module(), crypto_module(), hub_module());
}

// send_message / receive_message are internal to Messenger; the equivalent
// observable behavior is that discover_producer returns nullopt when disconnected.
int send_message_when_not_connected_returns_nullopt()
{
    return run_gtest_worker(
        []()
        {
            Messenger &messenger = Messenger::get_instance();
            std::optional<ConsumerInfo> result = messenger.discover_producer("test_channel", 100);
            EXPECT_FALSE(result.has_value())
                << "discover_producer must return nullopt when Messenger is not connected";
        },
        "messagehub.send_message_when_not_connected_returns_nullopt",
        logger_module(), crypto_module(), hub_module());
}

int receive_message_when_not_connected_returns_nullopt()
{
    return run_gtest_worker(
        []()
        {
            Messenger &messenger = Messenger::get_instance();
            std::optional<ConsumerInfo> result = messenger.discover_producer("test_channel", 50);
            EXPECT_FALSE(result.has_value())
                << "discover_producer must return nullopt when Messenger is not connected";
        },
        "messagehub.receive_message_when_not_connected_returns_nullopt",
        logger_module(), crypto_module(), hub_module());
}

// register_producer is now void (fire-and-forget). Verify it does not throw
// or crash when the Messenger is not connected.
int register_producer_when_not_connected_returns_false()
{
    return run_gtest_worker(
        []()
        {
            Messenger &messenger = Messenger::get_instance();
            ProducerInfo info{};
            info.shm_name = "test_shm";
            info.producer_pid = 12345;
            info.schema_hash.assign(32, '\0');
            info.schema_version = 0;
            // fire-and-forget: must not throw, crash, or block
            EXPECT_NO_THROW(messenger.register_producer("test_channel", info));
        },
        "messagehub.register_producer_when_not_connected_returns_false",
        logger_module(), crypto_module(), hub_module());
}

int discover_producer_when_not_connected_returns_nullopt()
{
    return run_gtest_worker(
        []()
        {
            Messenger &messenger = Messenger::get_instance();
            std::optional<ConsumerInfo> result = messenger.discover_producer("test_channel", 100);
            EXPECT_FALSE(result.has_value())
                << "discover_producer must return nullopt when Messenger is not connected";
        },
        "messagehub.discover_producer_when_not_connected_returns_nullopt",
        logger_module(), crypto_module(), hub_module());
}

int disconnect_when_not_connected_idempotent()
{
    return run_gtest_worker(
        []()
        {
            Messenger &messenger = Messenger::get_instance();
            messenger.disconnect();
            messenger.disconnect();
        },
        "messagehub.disconnect_when_not_connected_idempotent",
        logger_module(), crypto_module(), hub_module());
}

// -----------------------------------------------------------------------------
// Phase C.1 – In-process minimal broker (REG_REQ / DISC_REQ, CurveZMQ)
// -----------------------------------------------------------------------------
struct TestBrokerState
{
    std::string endpoint;
    std::string server_public_z85;
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    std::mutex registry_mutex;
    std::map<std::string, nlohmann::json> registry; // channel_name -> {shm_name, schema_hash, schema_version}
};

static void run_test_broker(TestBrokerState &state)
{
    zmq::context_t ctx(1);
    zmq::socket_t router(ctx, zmq::socket_type::router);

    char server_public[41];
    char server_secret[41];
    if (zmq_curve_keypair(server_public, server_secret) != 0)
    {
        return;
    }
    state.server_public_z85.assign(server_public, 40);

    router.set(zmq::sockopt::curve_server, 1);
    router.set(zmq::sockopt::curve_secretkey, std::string(server_secret, 40));
    router.set(zmq::sockopt::curve_publickey, state.server_public_z85);

    router.bind("tcp://127.0.0.1:0");
    std::string bound = router.get(zmq::sockopt::last_endpoint);
    state.endpoint = bound;
    state.ready.store(true, std::memory_order_release);

    while (!state.stop.load(std::memory_order_acquire))
    {
        std::vector<zmq::message_t> msgs;
        auto result = zmq::recv_multipart(router, std::back_inserter(msgs),
                                         zmq::recv_flags::dontwait);
        if (!result || msgs.size() < 3)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        zmq::message_t &identity = msgs[0];
        std::string msg_type = msgs[1].to_string();
        std::string json_str = msgs[2].to_string();

        if (msg_type == "REG_REQ")
        {
            nlohmann::json req = nlohmann::json::parse(json_str);
            std::string channel = req.value("channel_name", "");
            if (!channel.empty())
            {
                nlohmann::json entry;
                entry["shm_name"] = req.value("shm_name", "");
                entry["schema_hash"] = req.value("schema_hash", "");
                entry["schema_version"] = req.value("schema_version", 0);
                std::lock_guard<std::mutex> lock(state.registry_mutex);
                state.registry[channel] = std::move(entry);
            }
            nlohmann::json resp;
            resp["status"] = "success";
            std::string resp_str = resp.dump();
            zmq::send_multipart(router, std::vector<zmq::const_buffer>{
                                            zmq::buffer(identity.data(), identity.size()),
                                            zmq::buffer("REG_RESP"), zmq::buffer(resp_str)});
        }
        else if (msg_type == "DISC_REQ")
        {
            nlohmann::json req = nlohmann::json::parse(json_str);
            std::string channel = req.value("channel_name", "");
            nlohmann::json resp;
            {
                std::lock_guard<std::mutex> lock(state.registry_mutex);
                auto it = state.registry.find(channel);
                if (it != state.registry.end())
                {
                    resp["status"] = "success";
                    resp["shm_name"] = it->second["shm_name"];
                    resp["schema_hash"] = it->second["schema_hash"];
                    resp["schema_version"] = it->second["schema_version"];
                }
                else
                {
                    resp["status"] = "error";
                    resp["message"] = "channel not found";
                }
            }
            std::string resp_str = resp.dump();
            zmq::send_multipart(router, std::vector<zmq::const_buffer>{
                                            zmq::buffer(identity.data(), identity.size()),
                                            zmq::buffer("DISC_RESP"), zmq::buffer(resp_str)});
        }
    }
}

int with_broker_happy_path()
{
    return run_gtest_worker(
        []()
        {
            TestBrokerState broker_state;
            std::thread broker_thread(run_test_broker, std::ref(broker_state));
            while (!broker_state.ready.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));

            std::string channel = make_test_channel_name("MessageHubBroker");
            Messenger &messenger = Messenger::get_instance();
            EXPECT_TRUE(messenger.connect(broker_state.endpoint, broker_state.server_public_z85))
                << "Messenger connect to in-process broker failed";

            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 0x123456789ABCDEF0ULL;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer =
                create_datablock_producer_impl(channel, DataBlockPolicy::RingBuffer, config,
                                              nullptr, nullptr);
            ASSERT_NE(producer, nullptr) << "create_datablock_producer failed";

            // DataBlock factory no longer calls register_producer automatically.
            // Manually register so that discover_producer can find the channel.
            ProducerInfo pinfo{};
            pinfo.shm_name = channel;
            pinfo.producer_pid = pylabhub::platform::get_pid();
            pinfo.schema_hash.assign(32, '\0');
            pinfo.schema_version = 0;
            messenger.register_producer(channel, pinfo);

            // Give the async worker thread time to deliver REG_REQ to broker
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            const char payload[] = "with_broker_happy_path payload";
            const size_t payload_len = sizeof(payload);
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->write(payload, payload_len));
            EXPECT_TRUE(write_handle->commit(payload_len));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            std::optional<ConsumerInfo> info = messenger.discover_producer(channel, 5000);
            ASSERT_TRUE(info.has_value()) << "discover_producer should return ConsumerInfo when broker has registration";
            EXPECT_EQ(info->shm_name, channel);
            EXPECT_EQ(info->schema_version, 0u);

            auto consumer = find_datablock_consumer_impl(info->shm_name, config.shared_secret,
                                                         &config, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr) << "find_datablock_consumer with discovered shm_name must succeed";

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            std::string read_buf(payload_len, '\0');
            EXPECT_TRUE(consume_handle->read(read_buf.data(), payload_len));
            EXPECT_EQ(std::memcmp(read_buf.data(), payload, payload_len), 0)
                << "read data must match written data";

            consume_handle.reset();
            producer.reset();
            consumer.reset();
            messenger.disconnect();
            broker_state.stop.store(true, std::memory_order_release);
            broker_thread.join();
            cleanup_test_datablock(channel);
        },
        "messagehub.with_broker_happy_path",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::messagehub

namespace
{
struct MessageHubWorkerRegistrar
{
    MessageHubWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "messagehub")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::messagehub;
                if (scenario == "lifecycle_initialized_follows_state")
                    return lifecycle_initialized_follows_state();
                if (scenario == "send_message_when_not_connected_returns_nullopt")
                    return send_message_when_not_connected_returns_nullopt();
                if (scenario == "receive_message_when_not_connected_returns_nullopt")
                    return receive_message_when_not_connected_returns_nullopt();
                if (scenario == "register_producer_when_not_connected_returns_false")
                    return register_producer_when_not_connected_returns_false();
                if (scenario == "discover_producer_when_not_connected_returns_nullopt")
                    return discover_producer_when_not_connected_returns_nullopt();
                if (scenario == "disconnect_when_not_connected_idempotent")
                    return disconnect_when_not_connected_idempotent();
                if (scenario == "with_broker_happy_path")
                    return with_broker_happy_path();
                fmt::print(stderr, "ERROR: Unknown messagehub scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static MessageHubWorkerRegistrar g_messagehub_registrar;
} // namespace
