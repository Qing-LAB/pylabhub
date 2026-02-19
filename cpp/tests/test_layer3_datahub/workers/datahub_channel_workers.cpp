// tests/test_layer3_datahub/workers/datahub_channel_workers.cpp
//
// Phase 6 — ChannelHandle integration tests.
// Uses a real BrokerService in a background thread + Messenger singleton.
// All ZMQ send/recv operations happen in the test (worker process) thread.
#include "datahub_channel_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_service.hpp"
#include "plh_datahub.hpp"
#include "utils/channel_handle.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
// Use only the BrokerService from the broker namespace to avoid ChannelPattern ambiguity.
using pylabhub::broker::BrokerService;

namespace pylabhub::tests::worker::channel
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Local broker helper (same pattern as other worker files)
// ============================================================================
namespace
{

struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

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
    auto ready_future  = ready_promise->get_future();

    cfg.on_ready = [ready_promise](const std::string& ep, const std::string& pk)
    { ready_promise->set_value({ep, pk}); };

    auto     service = std::make_unique<BrokerService>(std::move(cfg));
    BrokerService* raw_ptr = service.get();
    std::thread t([raw_ptr]() { raw_ptr->run(); });

    auto info = ready_future.get();

    BrokerHandle handle;
    handle.service  = std::move(service);
    handle.thread   = std::move(t);
    handle.endpoint = info.first;
    handle.pubkey   = info.second;
    return handle;
}

} // anonymous namespace

// ============================================================================
// create_not_connected — create_channel returns nullopt when not connected
// ============================================================================

int create_not_connected(int /*argc*/, char** /*argv*/)
{
    return run_gtest_worker(
        []()
        {
            Messenger& messenger = Messenger::get_instance();

            auto handle = messenger.create_channel("channel.no_broker", ChannelPattern::Pipeline);
            EXPECT_FALSE(handle.has_value())
                << "create_channel must return nullopt when Messenger is not connected";
        },
        "channel.create_not_connected",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// connect_not_found — connect_channel returns nullopt for unknown channel
// ============================================================================

int connect_not_found(int /*argc*/, char** /*argv*/)
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Channel "does_not_exist" has never been registered — should time out.
            auto handle = messenger.connect_channel("channel.does_not_exist", 500);
            EXPECT_FALSE(handle.has_value())
                << "connect_channel must return nullopt for a non-existent channel";

            messenger.disconnect();
            broker.stop_and_join();
        },
        "channel.connect_not_found",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// pipeline_exchange — Pipeline create + connect + send + recv
// ============================================================================

int pipeline_exchange(int /*argc*/, char** /*argv*/)
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("Pipeline");

            // Producer: create Pipeline channel (binds ROUTER ctrl + PUSH data).
            auto producer = messenger.create_channel(channel, ChannelPattern::Pipeline);
            ASSERT_TRUE(producer.has_value()) << "create_channel(Pipeline) failed";
            EXPECT_EQ(producer->channel_name(), channel);
            EXPECT_EQ(producer->pattern(), ChannelPattern::Pipeline);
            EXPECT_TRUE(producer->is_valid());

            // Consumer: connect to the Pipeline channel (connects DEALER ctrl + PULL data).
            auto consumer = messenger.connect_channel(channel, 3000);
            ASSERT_TRUE(consumer.has_value()) << "connect_channel failed for Pipeline channel";
            EXPECT_EQ(consumer->channel_name(), channel);
            EXPECT_EQ(consumer->pattern(), ChannelPattern::Pipeline);
            EXPECT_TRUE(consumer->is_valid());

            // Send a known value from producer to consumer.
            const uint32_t kSentValue = 0xDEAD1234U;
            EXPECT_TRUE(producer->send(&kSentValue, sizeof(kSentValue)));

            // Receive on consumer side.
            std::vector<std::byte> buf;
            ASSERT_TRUE(consumer->recv(buf, 1000)) << "recv timed out on Pipeline channel";
            ASSERT_EQ(buf.size(), sizeof(uint32_t));

            uint32_t recv_val = 0;
            std::memcpy(&recv_val, buf.data(), sizeof(recv_val));
            EXPECT_EQ(recv_val, kSentValue);

            messenger.disconnect();
            broker.stop_and_join();
        },
        "channel.pipeline_exchange",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// pubsub_exchange — PubSub create + connect + send (retry) + recv
// ============================================================================

int pubsub_exchange(int /*argc*/, char** /*argv*/)
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("PubSub");

            // Producer: create PubSub channel (binds ROUTER ctrl + XPUB data).
            auto producer = messenger.create_channel(channel, ChannelPattern::PubSub);
            ASSERT_TRUE(producer.has_value()) << "create_channel(PubSub) failed";
            EXPECT_EQ(producer->pattern(), ChannelPattern::PubSub);
            EXPECT_TRUE(producer->is_valid());

            // Consumer: connect to the PubSub channel (connects DEALER ctrl + SUB data).
            auto consumer = messenger.connect_channel(channel, 3000);
            ASSERT_TRUE(consumer.has_value()) << "connect_channel failed for PubSub channel";
            EXPECT_EQ(consumer->pattern(), ChannelPattern::PubSub);

            // XPUB/SUB subscription propagation takes a moment.
            // Keep sending until the consumer gets a message (or give up after ~2s).
            const uint32_t kSentValue = 0xC0FFEE42U;
            std::vector<std::byte> buf;
            bool received = false;
            for (int attempt = 0; attempt < 40 && !received; ++attempt)
            {
                producer->send(&kSentValue, sizeof(kSentValue));
                received = consumer->recv(buf, 50);
            }
            ASSERT_TRUE(received) << "PubSub consumer never received message after 40 attempts";

            ASSERT_EQ(buf.size(), sizeof(uint32_t));
            uint32_t recv_val = 0;
            std::memcpy(&recv_val, buf.data(), sizeof(recv_val));
            EXPECT_EQ(recv_val, kSentValue);

            messenger.disconnect();
            broker.stop_and_join();
        },
        "channel.pubsub_exchange",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// channel_introspection — channel_name, pattern, is_valid
// ============================================================================

int channel_introspection(int /*argc*/, char** /*argv*/)
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("Introspect");

            // Default-constructed handle is invalid.
            ChannelHandle empty_handle;
            EXPECT_FALSE(empty_handle.is_valid());

            // Producer handle introspection.
            auto producer = messenger.create_channel(channel, ChannelPattern::Pipeline);
            ASSERT_TRUE(producer.has_value());
            EXPECT_EQ(producer->channel_name(), channel);
            EXPECT_EQ(producer->pattern(), ChannelPattern::Pipeline);
            EXPECT_FALSE(producer->has_shm());
            EXPECT_TRUE(producer->is_valid());

            // Consumer handle introspection.
            auto consumer = messenger.connect_channel(channel, 3000);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_EQ(consumer->channel_name(), channel);
            EXPECT_EQ(consumer->pattern(), ChannelPattern::Pipeline);
            EXPECT_FALSE(consumer->has_shm());
            EXPECT_TRUE(consumer->is_valid());

            // Invalidate the producer handle explicitly.
            producer->invalidate();
            EXPECT_FALSE(producer->is_valid());

            // Move semantics: moved-from handle is invalid.
            auto moved_consumer = std::move(*consumer);
            EXPECT_TRUE(moved_consumer.is_valid());
            EXPECT_FALSE(consumer->is_valid()); // NOLINT: moved-from checked intentionally

            messenger.disconnect();
            broker.stop_and_join();
        },
        "channel.channel_introspection",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::channel

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct ChannelWorkerRegistrar
{
    ChannelWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto             dot  = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "channel")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::channel;
                if (scenario == "create_not_connected")
                    return create_not_connected(argc, argv);
                if (scenario == "connect_not_found")
                    return connect_not_found(argc, argv);
                if (scenario == "pipeline_exchange")
                    return pipeline_exchange(argc, argv);
                if (scenario == "pubsub_exchange")
                    return pubsub_exchange(argc, argv);
                if (scenario == "channel_introspection")
                    return channel_introspection(argc, argv);
                fmt::print(stderr, "ERROR: Unknown channel scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static ChannelWorkerRegistrar g_channel_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
