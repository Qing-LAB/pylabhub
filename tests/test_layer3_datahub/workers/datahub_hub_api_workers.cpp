// tests/test_layer3_datahub/workers/datahub_hub_api_workers.cpp
//
// Hub Producer/Consumer unified API tests.
// Uses a real BrokerService in a background thread + Messenger singleton.
#include "datahub_hub_api_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_service.hpp"
#include "utils/hub_inbox_queue.hpp"
#include "utils/role_api_base.hpp"
#include "utils/schema_utils.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;
using pylabhub::LoopTimingPolicy;

namespace pylabhub::tests::worker::hub_api
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Shared helpers
// ============================================================================

namespace
{

/// Create a single-field schema for ShmQueue (required by establish_channel).
std::vector<hub::SchemaFieldDesc> make_schema(const std::string &type_str,
                                               uint32_t count = 1, uint32_t length = 0)
{
    hub::SchemaFieldDesc f;
    f.type_str = type_str;
    f.count    = count;
    f.length   = length;
    return {f};
}

struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    // RAII: ensure the broker thread is joined even if an exception skips stop_and_join().
    ~BrokerHandle()
    {
        if (thread.joinable())
        {
            if (service)
            {
                service->stop();
            }
            thread.join();
        }
    }

    // Non-copyable, movable.
    BrokerHandle()                               = default;
    BrokerHandle(const BrokerHandle &)            = delete;
    BrokerHandle &operator=(const BrokerHandle &) = delete;
    BrokerHandle(BrokerHandle &&)                 = default;
    BrokerHandle &operator=(BrokerHandle &&)      = default;

    void stop_and_join()
    {
        if (service)
        {
            service->stop();
        }
        if (thread.joinable())
        {
            thread.join();
        }
    }
};

BrokerHandle start_broker()
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<ReadyInfo>>();
    auto future  = promise->get_future();

    BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0";
    cfg.use_curve = true;
    cfg.on_ready  = [promise](const std::string &ep, const std::string &pk) {
        promise->set_value({ep, pk});
    };

    auto service     = std::make_unique<BrokerService>(std::move(cfg));
    auto *raw        = service.get();
    std::thread t([raw]() { raw->run(); });

    auto info = future.get();

    BrokerHandle h;
    h.service  = std::move(service);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

// Minimal SHM config for tests.
DataBlockConfig make_shm_config()
{
    DataBlockConfig cfg;
    cfg.physical_page_size    = DataBlockPageSize::Size4K;
    cfg.logical_unit_size     = 4096;
    cfg.ring_buffer_capacity  = 4;
    cfg.policy                = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy  = ConsumerSyncPolicy::Latest_only;
    cfg.flex_zone_size        = 0;
    cfg.shared_secret         = 0xDEADBEEFCAFEBABEULL;
    return cfg;
}

constexpr uint64_t kTestShmSecret = 0xDEADBEEFCAFEBABEULL;

} // anonymous namespace

// ============================================================================
// producer_create_pubsub
// ============================================================================

int producer_create_pubsub(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            ProducerOptions opts;
            opts.channel_name = make_test_channel_name("hub.pubsub");
            opts.pattern      = ChannelPattern::PubSub;
            opts.has_shm      = false;
            opts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, opts);
            ASSERT_TRUE(producer.has_value()) << "Producer::create(PubSub) failed";

            EXPECT_TRUE(producer->is_valid());
            EXPECT_EQ(producer->channel_name(), opts.channel_name);
            EXPECT_EQ(producer->pattern(), ChannelPattern::PubSub);
            EXPECT_FALSE(producer->has_shm());

            producer->close();
            EXPECT_FALSE(producer->is_valid());

            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.producer_create_pubsub",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// producer_create_with_shm
// ============================================================================

int producer_create_with_shm(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            ProducerOptions opts;
            opts.channel_name = make_test_channel_name("hub.shm_producer");
            opts.pattern      = ChannelPattern::Pipeline;
            opts.has_shm      = true;
            opts.shm_config   = make_shm_config();
            opts.zmq_schema   = make_schema("uint32");
            opts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, opts);
            ASSERT_TRUE(producer.has_value()) << "Producer::create(has_shm) failed";
            EXPECT_TRUE(producer->is_valid());
            EXPECT_TRUE(producer->has_shm());

            // synced_write: sync slot acquire + job executed in calling thread
            struct Payload { uint32_t value; };
            uint32_t written_value = 0xCAFEU;
            bool job_ran = false;
            bool ok = producer->synced_write<void, Payload>(
                [&](WriteProcessorContext<void, Payload> &ctx) {
                    for (auto &result : ctx.txn.slots(std::chrono::milliseconds(5000)))
                    {
                        if (!result.is_ok()) break;
                        result.content().get() = Payload{written_value};
                        job_ran = true;
                        break; // write one slot; auto-publish on break
                    }
                });
            EXPECT_TRUE(ok);
            EXPECT_TRUE(job_ran);

            // push: requires start() to have write_thread running
            ASSERT_TRUE(producer->start());

            std::atomic<bool> async_job_ran{false};
            bool posted = producer->push<void, Payload>(
                [&](WriteProcessorContext<void, Payload> &ctx) {
                    for (auto &result : ctx.txn.slots(std::chrono::milliseconds(5000)))
                    {
                        if (!result.is_ok()) break;
                        result.content().get() = Payload{0xBEEFU};
                        async_job_ran.store(true);
                        break;
                    }
                });
            EXPECT_TRUE(posted);

            // Wait for async job to complete (up to 1s)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!async_job_ran.load() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            EXPECT_TRUE(async_job_ran.load()) << "push async job did not run";

            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.producer_create_with_shm",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_connect_e2e
// ============================================================================

int consumer_connect_e2e(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.e2e");

            // Create producer
            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::Pipeline;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // Connect consumer
            ConsumerOptions copts;
            copts.channel_name = channel;
            copts.timeout_ms   = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";
            EXPECT_TRUE(consumer->is_valid());
            EXPECT_EQ(consumer->channel_name(), channel);
            EXPECT_EQ(consumer->pattern(), ChannelPattern::Pipeline);

            // Producer send → Consumer recv
            const uint32_t kVal = 0xABCD1234U;
            EXPECT_TRUE(producer->send(&kVal, sizeof(kVal)));

            std::vector<std::byte> buf;
            ASSERT_TRUE(consumer->channel_handle().recv(buf, 1000));
            ASSERT_EQ(buf.size(), sizeof(uint32_t));
            uint32_t recv_val = 0;
            std::memcpy(&recv_val, buf.data(), sizeof(recv_val));
            EXPECT_EQ(recv_val, kVal);

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_connect_e2e",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_hello_tracked
// ============================================================================

int consumer_hello_tracked(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.hello");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->start()); // start peer_thread to receive HELLO

            ConsumerOptions copts;
            copts.channel_name = channel;
            copts.timeout_ms   = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());

            // Wait for peer_thread to process HELLO (up to 500ms)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (producer->connected_consumers().empty() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            auto consumers = producer->connected_consumers();
            EXPECT_EQ(consumers.size(), 1U)
                << "Expected 1 consumer in connected list after HELLO";

            consumer->close(); // sends BYE
            producer->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_hello_tracked",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// active_producer_consumer_callbacks
// ============================================================================

int active_producer_consumer_callbacks(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.active");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            ConsumerOptions copts;
            copts.channel_name = channel;
            copts.timeout_ms   = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());

            // Set callback before start()
            std::atomic<int> data_received{0};
            std::mutex       data_mu;
            std::vector<std::byte> last_data;
            consumer->on_zmq_data([&](std::span<const std::byte> data) {
                std::lock_guard<std::mutex> lock(data_mu);
                last_data.assign(data.begin(), data.end());
                data_received.fetch_add(1, std::memory_order_relaxed);
            });

            ASSERT_TRUE(consumer->start());
            ASSERT_TRUE(producer->start());

            // Send data repeatedly until received (XPUB/SUB subscription latency)
            const uint32_t kVal = 0xFEEDFACEU;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (data_received.load() == 0 && std::chrono::steady_clock::now() < deadline)
            {
                producer->send(&kVal, sizeof(kVal));
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }

            EXPECT_GT(data_received.load(), 0) << "on_zmq_data callback never fired";

            {
                std::lock_guard<std::mutex> lock(data_mu);
                if (!last_data.empty())
                {
                    ASSERT_EQ(last_data.size(), sizeof(uint32_t));
                    uint32_t recv_val = 0;
                    std::memcpy(&recv_val, last_data.data(), sizeof(recv_val));
                    EXPECT_EQ(recv_val, kVal);
                }
            }

            consumer->stop();
            consumer->close();
            producer->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.active_producer_consumer_callbacks",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// peer_callback_on_consumer_join
// ============================================================================

int peer_callback_on_consumer_join(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.peer_cb");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // Register callback BEFORE start()
            std::atomic<int> join_count{0};
            std::string      joined_identity;
            std::mutex       identity_mu;
            producer->on_consumer_joined([&](const std::string &id) {
                std::lock_guard<std::mutex> lock(identity_mu);
                joined_identity = id;
                join_count.fetch_add(1, std::memory_order_relaxed);
            });

            ASSERT_TRUE(producer->start());

            ConsumerOptions copts;
            copts.channel_name = channel;
            copts.timeout_ms   = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());

            // Wait for on_consumer_joined callback to fire (up to 500ms)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (join_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            EXPECT_EQ(join_count.load(), 1) << "on_consumer_joined should fire exactly once";
            {
                std::lock_guard<std::mutex> lock(identity_mu);
                EXPECT_FALSE(joined_identity.empty()) << "Identity should be non-empty";
            }

            consumer->close();
            producer->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.peer_callback_on_consumer_join",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// non_template_factory
// ============================================================================

int non_template_factory(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.notemplate");

            // Non-template producer
            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::Pipeline;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = make_shm_config();
            popts.zmq_schema   = make_schema("uint64");
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->has_shm());

            // Non-template consumer
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            // SHM may or may not attach depending on timing; just check ZMQ works
            EXPECT_TRUE(consumer->is_valid());

            // ZMQ send/recv works regardless of SHM
            const uint32_t kVal = 0x12345678U;
            EXPECT_TRUE(producer->send(&kVal, sizeof(kVal)));
            std::vector<std::byte> buf;
            ASSERT_TRUE(consumer->channel_handle().recv(buf, 1000));
            ASSERT_EQ(buf.size(), sizeof(uint32_t));
            uint32_t recv_val = 0;
            std::memcpy(&recv_val, buf.data(), sizeof(recv_val));
            EXPECT_EQ(recv_val, kVal);

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.non_template_factory",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// managed_producer_lifecycle
// ============================================================================

int managed_producer_lifecycle(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.managed");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::Pipeline;
            popts.timeout_ms   = 3000;

            ManagedProducer mp(messenger, popts);
            EXPECT_FALSE(mp.is_initialized());

            // Manually call startup (simulating what LifecycleGuard would do)
            // ManagedProducer::s_startup is static/private; instead, test via get_module_def()
            // by manually invoking the startup via the stored callback key.
            // For this test, directly call Producer::create to simulate lifecycle.
            auto p = Producer::create(messenger, popts);
            ASSERT_TRUE(p.has_value());
            EXPECT_TRUE(p->is_valid());
            EXPECT_EQ(p->channel_name(), channel);

            p->start();
            EXPECT_TRUE(p->is_running());

            p->stop();
            EXPECT_FALSE(p->is_running());

            p->close();
            EXPECT_FALSE(p->is_valid());

            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.managed_producer_lifecycle",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_shm_secret_mismatch
// ============================================================================

int consumer_shm_secret_mismatch(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.secret_mismatch");

            // Producer with SHM
            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::Pipeline;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // Consumer with WRONG shm_shared_secret
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = 0xBADBADBADBADBADULL; // wrong secret
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());

            // ZMQ should still work; SHM should be nullptr due to secret mismatch
            EXPECT_TRUE(consumer->is_valid());
            EXPECT_FALSE(consumer->has_shm())
                << "SHM must not be connected when shared_secret doesn't match";

            // ZMQ send/recv still works
            const uint32_t kVal = 0xABCDEF01U;
            EXPECT_TRUE(producer->send(&kVal, sizeof(kVal)));
            std::vector<std::byte> buf;
            ASSERT_TRUE(consumer->channel_handle().recv(buf, 1000));
            ASSERT_EQ(buf.size(), sizeof(uint32_t));
            uint32_t recv_val = 0;
            std::memcpy(&recv_val, buf.data(), sizeof(recv_val));
            EXPECT_EQ(recv_val, kVal);

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_shm_secret_mismatch",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_bye_tracked
// ============================================================================

int consumer_bye_tracked(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker      = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.bye");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            std::atomic<int> left_count{0};
            std::string      left_identity;
            std::mutex       identity_mu;
            producer->on_consumer_left([&](const std::string &id) {
                std::lock_guard<std::mutex> lock(identity_mu);
                left_identity = id;
                left_count.fetch_add(1, std::memory_order_relaxed);
            });
            ASSERT_TRUE(producer->start());

            // Connect consumer (sends HELLO automatically)
            ConsumerOptions copts;
            copts.channel_name = channel;
            copts.timeout_ms   = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());

            // Wait for HELLO to be tracked (up to 500ms)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (producer->connected_consumers().empty() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ASSERT_EQ(producer->connected_consumers().size(), 1U)
                << "HELLO must be tracked before testing BYE";

            // Disconnect: consumer::close() sends BYE before closing sockets
            consumer->close();

            // Wait for BYE to be processed (up to 500ms)
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (left_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            EXPECT_EQ(left_count.load(), 1) << "on_consumer_left should fire when consumer closes";
            EXPECT_TRUE(producer->connected_consumers().empty())
                << "connected_consumers should be empty after BYE";
            {
                std::lock_guard<std::mutex> lock(identity_mu);
                EXPECT_FALSE(left_identity.empty()) << "Left identity should be non-empty";
            }

            producer->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_bye_tracked",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_shm_read_e2e
// ============================================================================

int consumer_shm_read_e2e(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker      = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.shm_read_e2e");

            // Producer with SHM
            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::Pipeline;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Consumer with SHM (matching secret)
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            ASSERT_TRUE(consumer->has_shm())
                << "Consumer should attach to SHM with matching secret";

            // Install set_read_handler BEFORE start()
            struct Payload
            {
                uint32_t value;
            };
            constexpr uint32_t    kWrittenVal = 0xDEADBEEFU;
            std::atomic<bool>     shm_cb_fired{false};
            std::atomic<uint32_t> read_val{0};
            consumer->set_read_handler<void, Payload>(
                [&](ReadProcessorContext<void, Payload> &ctx) {
                    for (auto &result : ctx.txn.slots(std::chrono::milliseconds(50)))
                    {
                        if (!result.is_ok()) break;
                        read_val.store(result.content().get().value,
                                       std::memory_order_relaxed);
                        shm_cb_fired.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (shm_cb_fired.load())
                        return; // Got one slot — done
                });

            ASSERT_TRUE(consumer->start()); // launches shm_thread
            ASSERT_TRUE(producer->start()); // launches write_thread

            // Post an async write
            bool posted = producer->push<void, Payload>(
                [&](WriteProcessorContext<void, Payload> &ctx) {
                    for (auto &result : ctx.txn.slots(std::chrono::milliseconds(5000)))
                    {
                        if (!result.is_ok()) break;
                        result.content().get() = Payload{kWrittenVal};
                        break;
                    }
                });
            EXPECT_TRUE(posted);

            // Wait for set_read_handler callback (up to 2s)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!shm_cb_fired.load() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            EXPECT_TRUE(shm_cb_fired.load()) << "set_read_handler callback never fired";
            EXPECT_EQ(read_val.load(), kWrittenVal)
                << "Data read from SHM does not match written value";

            consumer->stop();
            consumer->close();
            producer->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_shm_read_e2e",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_read_shm_sync
// ============================================================================

int consumer_read_shm_sync(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker      = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.shm_sync");

            // Producer with SHM (no start() required for sync write_shm)
            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::Pipeline;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Consumer with SHM (no start() required for sync read_shm)
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            ASSERT_TRUE(consumer->has_shm())
                << "Consumer should attach to SHM with matching secret";

            // Sync write: producer writes known data in calling thread
            struct Payload
            {
                uint32_t a;
                uint32_t b;
            };
            constexpr Payload kWrite{0xCAFEBABEU, 0x12345678U};
            bool write_ok = producer->synced_write<void, Payload>(
                [&](WriteProcessorContext<void, Payload> &ctx) {
                    for (auto &result : ctx.txn.slots(std::chrono::milliseconds(5000)))
                    {
                        if (!result.is_ok()) break;
                        result.content().get() = kWrite;
                        break; // auto-publish on break
                    }
                });
            EXPECT_TRUE(write_ok) << "synced_write should succeed";

            // Sync read: consumer reads and verifies in calling thread
            Payload read_val{};
            bool read_ok = consumer->pull<void, Payload>(
                [&](ReadProcessorContext<void, Payload> &ctx) {
                    for (auto &result : ctx.txn.slots(std::chrono::milliseconds(5000)))
                    {
                        if (!result.is_ok()) break;
                        read_val = result.content().get();
                        break;
                    }
                });
            EXPECT_TRUE(read_ok) << "pull should succeed";
            EXPECT_EQ(read_val.a, kWrite.a);
            EXPECT_EQ(read_val.b, kWrite.b);

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_read_shm_sync",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// producer_consumer_idempotency
// ============================================================================

int producer_consumer_idempotency(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker      = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.idempotent");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // start() is idempotent: second call returns false (already running)
            EXPECT_TRUE(producer->start());
            EXPECT_FALSE(producer->start()) << "second start() must return false";
            EXPECT_TRUE(producer->is_running());

            // stop() is idempotent: second call is a no-op
            producer->stop();
            EXPECT_FALSE(producer->is_running());
            producer->stop(); // must not crash

            // Consumer connects while producer is valid (not yet closed)
            ConsumerOptions copts;
            copts.channel_name = channel;
            copts.timeout_ms   = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());

            EXPECT_TRUE(consumer->start());
            EXPECT_FALSE(consumer->start()) << "second start() must return false";
            EXPECT_TRUE(consumer->is_running());

            consumer->stop();
            EXPECT_FALSE(consumer->is_running());
            consumer->stop(); // must not crash

            // close() is idempotent
            consumer->close();
            EXPECT_FALSE(consumer->is_valid());
            consumer->close(); // must not crash

            producer->close();
            EXPECT_FALSE(producer->is_valid());
            producer->close(); // must not crash

            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.producer_consumer_idempotency",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// producer_consumer_ctrl_messaging
// ============================================================================

int producer_consumer_ctrl_messaging(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker      = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.ctrl_msg");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // ── Phase 1: Consumer → Producer ctrl message ────────────────────
            // peer_thread must be running to dispatch on_consumer_message.
            // on_consumer_message callback registered BEFORE start().
            std::atomic<int>       consumer_msg_count{0};
            std::string            recv_identity;
            std::vector<std::byte> recv_body;
            std::mutex             recv_mu;
            producer->on_consumer_message(
                [&](const std::string &id, std::span<const std::byte> data) {
                    std::lock_guard<std::mutex> lock(recv_mu);
                    recv_identity = id;
                    recv_body.assign(data.begin(), data.end());
                    consumer_msg_count.fetch_add(1, std::memory_order_relaxed);
                });
            ASSERT_TRUE(producer->start());

            // Connect consumer — ctrl_thread NOT started yet (safe to send_ctrl from main thread)
            ConsumerOptions copts;
            copts.channel_name = channel;
            copts.timeout_ms   = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());

            // Wait for HELLO to be tracked (up to 500ms)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (producer->connected_consumers().empty() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            auto consumers = producer->connected_consumers();
            ASSERT_EQ(consumers.size(), 1U) << "Consumer must be tracked before ctrl test";
            const std::string identity = consumers[0];

            // Consumer sends ctrl from main thread (ctrl_thread not running — no race)
            const uint32_t kPingVal = 0xC0FFEE42U;
            EXPECT_TRUE(consumer->send_ctrl("CUSTOM_PING", &kPingVal, sizeof(kPingVal)));

            // Wait for on_consumer_message callback (up to 500ms)
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (consumer_msg_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            EXPECT_EQ(consumer_msg_count.load(), 1)
                << "on_consumer_message should fire once for CUSTOM_PING";
            {
                std::lock_guard<std::mutex> lock(recv_mu);
                EXPECT_EQ(recv_identity, identity);
                ASSERT_EQ(recv_body.size(), sizeof(uint32_t));
                uint32_t body_val = 0;
                std::memcpy(&body_val, recv_body.data(), sizeof(body_val));
                EXPECT_EQ(body_val, kPingVal);
            }

            // ── Phase 2: Producer → Consumer ctrl message ────────────────────
            // Register on_producer_message callback BEFORE starting consumer.
            std::atomic<int>       producer_msg_count{0};
            std::string            recv_type;
            std::vector<std::byte> recv_ctrl_body;
            std::mutex             recv_ctrl_mu;
            consumer->on_producer_message([&](std::string_view type,
                                               std::span<const std::byte> data) {
                std::lock_guard<std::mutex> lock(recv_ctrl_mu);
                recv_type = std::string(type);
                recv_ctrl_body.assign(data.begin(), data.end());
                producer_msg_count.fetch_add(1, std::memory_order_relaxed);
            });
            ASSERT_TRUE(consumer->start()); // ctrl_thread starts; takes ownership of DEALER socket

            // Producer sends ctrl to consumer — queued to peer_thread → thread-safe
            const uint32_t kPongVal = 0xDEADF00DU;
            EXPECT_TRUE(
                producer->send_ctrl(identity, "CUSTOM_PONG", &kPongVal, sizeof(kPongVal)));

            // Wait for on_producer_message callback (up to 500ms)
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (producer_msg_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            EXPECT_EQ(producer_msg_count.load(), 1)
                << "on_producer_message should fire once for CUSTOM_PONG";
            {
                std::lock_guard<std::mutex> lock(recv_ctrl_mu);
                EXPECT_EQ(recv_type, "CUSTOM_PONG");
                ASSERT_EQ(recv_ctrl_body.size(), sizeof(uint32_t));
                uint32_t ctrl_val = 0;
                std::memcpy(&ctrl_val, recv_ctrl_body.data(), sizeof(ctrl_val));
                EXPECT_EQ(ctrl_val, kPongVal);
            }

            consumer->stop();
            consumer->close();
            producer->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.producer_consumer_ctrl_messaging",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_destructor_bye
// ============================================================================

int consumer_destructor_bye(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker      = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.dtor_bye");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            std::atomic<int> left_count{0};
            producer->on_consumer_left([&](const std::string & /*id*/) {
                left_count.fetch_add(1, std::memory_order_relaxed);
            });
            ASSERT_TRUE(producer->start());

            {
                // Consumer created and started inside an inner scope.
                // No explicit stop() or close() — destructor must send BYE.
                ConsumerOptions copts;
                copts.channel_name = channel;
                copts.timeout_ms   = 3000;
                auto consumer = Consumer::connect(messenger, copts);
                ASSERT_TRUE(consumer.has_value());

                ASSERT_TRUE(consumer->start()); // ctrl_thread running

                // Wait for HELLO to be tracked (up to 500ms)
                auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (producer->connected_consumers().empty() &&
                       std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                ASSERT_EQ(producer->connected_consumers().size(), 1U);
                // consumer goes out of scope here → destructor calls close() → stop() then BYE
            }

            // Wait for BYE to arrive at producer's peer_thread (up to 500ms)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (left_count.load() == 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            EXPECT_EQ(left_count.load(), 1)
                << "Destructor must send BYE even without explicit stop()/close()";
            EXPECT_TRUE(producer->connected_consumers().empty());

            producer->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_destructor_bye",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// producer_channel_identity
// ============================================================================

int producer_channel_identity(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker          = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.identity");

            // Set identity fields in the SHM config.
            DataBlockConfig cfg = make_shm_config();
            cfg.hub_uid         = "hub_uid_test_42";
            cfg.hub_name        = "TestHub";
            cfg.producer_uid    = "prod_uid_abc";
            cfg.producer_name   = "TestProducer";

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = cfg;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Verify channel identity accessors read back from SHM header.
            EXPECT_EQ(producer->hub_uid(),      "hub_uid_test_42");
            EXPECT_EQ(producer->hub_name(),     "TestHub");
            EXPECT_EQ(producer->producer_uid(), "prod_uid_abc");
            EXPECT_EQ(producer->producer_name(), "TestProducer");

            // Consumer also reads the same header — channel identity is shared.
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            ASSERT_TRUE(consumer->has_shm());

            EXPECT_EQ(consumer->hub_uid(),      "hub_uid_test_42");
            EXPECT_EQ(consumer->hub_name(),     "TestHub");
            EXPECT_EQ(consumer->producer_uid(), "prod_uid_abc");
            EXPECT_EQ(consumer->producer_name(), "TestProducer");

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.producer_channel_identity",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_identity_in_shm
// ============================================================================

int consumer_identity_in_shm(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker          = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string channel = make_test_channel_name("hub.consumer_id");

            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::PubSub;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Consumer sets its own identity via ConsumerOptions.
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.consumer_uid      = "cuid_abcdef1234";
            copts.consumer_name     = "MyCoolConsumer";
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            ASSERT_TRUE(consumer->has_shm());

            // Verify the consumer's own identity is stored and readable.
            EXPECT_EQ(consumer->consumer_uid(),  "cuid_abcdef1234");
            EXPECT_EQ(consumer->consumer_name(), "MyCoolConsumer");

            // Graceful close: uid/name are cleared from the heartbeat slot.
            consumer->close();
            EXPECT_FALSE(consumer->is_valid());

            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.consumer_identity_in_shm",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// producer_consumer_forwarding_api — write/read through Producer/Consumer
// ============================================================================

int producer_consumer_forwarding_api(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            ProducerOptions popts;
            popts.channel_name = make_test_channel_name("hub.fwd_api");
            popts.pattern      = ChannelPattern::PubSub;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = make_shm_config();
            popts.zmq_schema = make_schema("float64");
            popts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // Verify forwarding API metadata.
            EXPECT_EQ(producer->queue_item_size(), sizeof(double));
            EXPECT_GT(producer->queue_capacity(), 0u);

            // Start queue through forwarding API.
            EXPECT_TRUE(producer->start_queue());

            // Write through forwarding API.
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            *static_cast<double *>(buf) = 42.5;
            producer->write_commit();

            // Consumer side.
            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.zmq_schema = make_schema("float64");
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_TRUE(consumer->start_queue());
            EXPECT_EQ(consumer->queue_item_size(), sizeof(double));

            // Read through forwarding API.
            const void *data = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data, nullptr);
            EXPECT_DOUBLE_EQ(*static_cast<const double *>(data), 42.5);
            consumer->read_release();

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.producer_consumer_forwarding_api",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// construction_time_checksum — checksum configured via Options, not setters
// ============================================================================

int construction_time_checksum(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            ProducerOptions popts;
            popts.channel_name    = make_test_channel_name("hub.cksum");
            popts.pattern         = ChannelPattern::PubSub;
            popts.has_shm         = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config      = make_shm_config();
            popts.shm_config.checksum_policy = ChecksumPolicy::Manual;
            popts.zmq_schema = make_schema("uint64");
            popts.checksum_policy = ChecksumPolicy::Enforced;  // construction-time flag
            popts.timeout_ms      = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->start_queue());

            // Write a value — checksum should be stamped automatically by ShmQueue.
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            *static_cast<uint64_t *>(buf) = 0xCAFEBABE;
            producer->write_commit();

            // Consumer with verify_checksum enabled at construction.
            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.zmq_schema = make_schema("uint64");
            copts.checksum_policy   = ChecksumPolicy::Enforced;  // construction-time flag
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_TRUE(consumer->start_queue());

            // Read — should succeed (checksum valid because producer stamped it).
            const void *data = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data, nullptr) << "read_acquire failed — checksum mismatch?";
            EXPECT_EQ(*static_cast<const uint64_t *>(data), 0xCAFEBABEu);
            consumer->read_release();

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.construction_time_checksum",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// flexzone_through_service_layer — access via Producer/Consumer DataBlock path
// ============================================================================

int flexzone_through_service_layer(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            constexpr size_t kFzSize = 4096; // page-aligned

            auto shm_cfg = make_shm_config();
            shm_cfg.flex_zone_size = kFzSize;

            ProducerOptions popts;
            popts.channel_name = make_test_channel_name("hub.fz_svc");
            popts.pattern      = ChannelPattern::PubSub;
            popts.has_shm      = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config   = shm_cfg;
            popts.zmq_schema = make_schema("float64");
            popts.fz_schema = make_schema("float64");
            popts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // Flexzone through Producer — DataBlock-direct path.
            EXPECT_EQ(producer->flexzone_size(), kFzSize);
            void *wfz = producer->write_flexzone();
            ASSERT_NE(wfz, nullptr);

            // Write marker to flexzone.
            std::memset(wfz, 0, kFzSize);
            static_cast<uint32_t *>(wfz)[0] = 0xDEAD;

            // Consumer reads flexzone.
            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.zmq_schema = make_schema("float64");
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_EQ(consumer->flexzone_size(), kFzSize);

            const void *rfz = consumer->read_flexzone();
            ASSERT_NE(rfz, nullptr);
            EXPECT_EQ(static_cast<const uint32_t *>(rfz)[0], 0xDEADu);

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.flexzone_through_service_layer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// queue_metrics_forwarding — metrics() through Producer/Consumer
// ============================================================================

int queue_metrics_forwarding(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            ProducerOptions popts;
            popts.channel_name  = make_test_channel_name("hub.metrics_fwd");
            popts.pattern       = ChannelPattern::PubSub;
            popts.has_shm       = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config    = make_shm_config();
            popts.zmq_schema = make_schema("uint64");
            // Timing is role-level — not set on queue options.
            popts.timeout_ms    = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->start_queue());
            producer->reset_queue_metrics();

            // Write 3 slots.
            for (int i = 0; i < 3; ++i)
            {
                void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
                ASSERT_NE(buf, nullptr);
                *static_cast<uint64_t *>(buf) = static_cast<uint64_t>(i);
                producer->write_commit();
            }

            // Check metrics through forwarding.
            auto m = producer->queue_metrics();
            // configured_period_us now reported at loop level, not in QueueMetrics.
            // policy_info through forwarding.
            auto pi = producer->queue_policy_info();
            EXPECT_FALSE(pi.empty());

            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.queue_metrics_forwarding",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// zmq_forwarding_api — ZMQ transport through Producer/Consumer forwarding
// ============================================================================

int zmq_forwarding_api(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            // Separate Messengers for producer and consumer — avoids single-worker
            // deadlock where heartbeat delivery blocks behind connect_channel.
            Messenger prod_m;
            ASSERT_TRUE(prod_m.connect(broker.endpoint, broker.pubkey));

            constexpr size_t kItemSz = 8;
            const std::string endpoint = "tcp://127.0.0.1:0"; // ephemeral — tests HEP-0021 §16

            ProducerOptions popts;
            popts.channel_name      = make_test_channel_name("hub.zmq_fwd");
            popts.has_shm           = false;
            popts.data_transport    = "zmq";
            popts.zmq_node_endpoint = endpoint;
            popts.zmq_schema        = {{"bytes", 1, static_cast<uint32_t>(kItemSz)}};
            // Timing is role-level — not set on queue options.
            popts.timeout_ms        = 3000;

            fmt::print(stderr, "[T] creating producer...\n");
            auto producer = Producer::create(prod_m, popts);
            ASSERT_TRUE(producer.has_value());
            fmt::print(stderr, "[T] producer created OK\n");

            // Forwarding API metadata works for ZMQ.
            EXPECT_EQ(producer->queue_item_size(), kItemSz);
            EXPECT_GT(producer->queue_capacity(), 0u);
            EXPECT_TRUE(producer->start_queue());
            fmt::print(stderr, "[T] producer queue started\n");

            // Flexzone returns nullptr for ZMQ (no DataBlock).
            EXPECT_EQ(producer->write_flexzone(), nullptr);
            EXPECT_EQ(producer->read_flexzone(), nullptr);
            EXPECT_EQ(producer->flexzone_size(), 0u);

            // Checksum no-op for ZMQ (no crash).
            producer->set_checksum_options(true, true);
            producer->sync_flexzone_checksum();

            // configured_period_us now reported at loop level, not in QueueMetrics.
            EXPECT_FALSE(producer->queue_policy_info().empty());
            fmt::print(stderr, "[T] producer verified, connecting consumer messenger...\n");

            // Consumer via broker discovery (separate Messenger).
            Messenger cons_m;
            ASSERT_TRUE(cons_m.connect(broker.endpoint, broker.pubkey));
            fmt::print(stderr, "[T] consumer messenger connected, calling Consumer::connect...\n");

            ConsumerOptions copts;
            copts.channel_name  = popts.channel_name;
            copts.consumer_uid  = "CONS-ZMQ-FWD-0001";
            copts.zmq_schema    = {{"bytes", 1, static_cast<uint32_t>(kItemSz)}};
            // Timing is role-level — not set on queue options.
            copts.timeout_ms    = 3000;

            auto consumer = Consumer::connect(cons_m, copts);
            fmt::print(stderr, "[T] Consumer::connect returned has_value={}\n",
                       consumer.has_value());
            ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";
            fmt::print(stderr, "[T] consumer transport='{}' ep='{}' queue={}\n",
                       consumer->data_transport(), consumer->zmq_node_endpoint(),
                       static_cast<void*>(consumer->queue()));
            EXPECT_EQ(consumer->queue_item_size(), kItemSz);

            fmt::print(stderr, "[T] sleeping 100ms for ZMQ TCP...\n");
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            fmt::print(stderr, "[T] sleep done, writing...\n");

            // Write through forwarding.
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            fmt::print(stderr, "[T] write_acquire returned {}\n", static_cast<void*>(buf));
            ASSERT_NE(buf, nullptr);
            const std::array<uint8_t, kItemSz> payload{10, 20, 30, 40, 50, 60, 70, 80};
            std::memcpy(buf, payload.data(), kItemSz);
            producer->write_commit();
            fmt::print(stderr, "[T] write_commit done, reading...\n");

            // Read through forwarding.
            const void *data = consumer->read_acquire(std::chrono::milliseconds{3000});
            fmt::print(stderr, "[T] read_acquire returned {}\n", static_cast<const void*>(data));
            ASSERT_NE(data, nullptr) << "ZMQ read_acquire timed out";
            std::array<uint8_t, kItemSz> received{};
            std::memcpy(received.data(), data, kItemSz);
            consumer->read_release();
            EXPECT_EQ(received, payload);
            fmt::print(stderr, "[T] data verified OK\n");

            // Consumer metrics.
            // configured_period_us now reported at loop level, not in QueueMetrics.

            // Cleanup: close channels while broker is alive, then stop broker.
            consumer->close();
            producer->close();
            broker.stop_and_join();
            cons_m.disconnect();
            prod_m.disconnect();
        },
        "hub_api.zmq_forwarding_api",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// forwarding_error_paths — null queue / no-SHM paths return safe defaults
// ============================================================================

int forwarding_error_paths(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Producer with no schema → no queue created internally.
            ProducerOptions popts;
            popts.channel_name = make_test_channel_name("hub.err_path");
            popts.has_shm      = true;
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            // All forwarding methods must return safe defaults, not crash.
            EXPECT_EQ(producer->queue_item_size(), 0u);
            EXPECT_EQ(producer->queue_capacity(), 0u);
            EXPECT_FALSE(producer->start_queue());
            EXPECT_EQ(producer->write_acquire(std::chrono::milliseconds{10}), nullptr);

            // Commit/discard on null queue — must not crash.
            producer->write_commit();
            producer->write_discard();

            // Metrics returns empty.
            auto m = producer->queue_metrics();
            EXPECT_EQ(m.last_iteration_us, 0u);

            // Flexzone still works (SHM exists, flexzone just happens to be 0).
            // Note: shm_config.flex_zone_size == 0, so flexzone is nullptr.
            EXPECT_EQ(producer->write_flexzone(), nullptr);
            EXPECT_EQ(producer->flexzone_size(), 0u);

            // Checksum/sync on null queue — no crash.
            producer->set_checksum_options(true, true);
            producer->sync_flexzone_checksum();
            producer->reset_queue_metrics();

            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.forwarding_error_paths",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// runtime_verify_checksum_toggle — toggle at runtime through Consumer
// ============================================================================

int runtime_verify_checksum_toggle(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Use a struct with sequence index for tracking.
            struct TestSlot { uint64_t seq; uint64_t value; };

            ProducerOptions popts;
            popts.channel_name    = make_test_channel_name("hub.rt_verify");
            popts.has_shm         = true;
            popts.zmq_schema   = make_schema("uint64");
            popts.shm_config      = make_shm_config();
            popts.shm_config.checksum_policy = ChecksumPolicy::Manual;
            popts.shm_config.logical_unit_size = sizeof(TestSlot);
            { hub::SchemaFieldDesc f1, f2; f1.type_str = "uint64"; f2.type_str = "uint64";
              popts.zmq_schema = {f1, f2}; }
            popts.checksum_policy = ChecksumPolicy::None; // NO ShmQueue-level checksum
            popts.timeout_ms      = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->start_queue());

            // Write slot seq=1 (no ShmQueue checksum, but DataBlock may auto-checksum).
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            auto *slot = static_cast<TestSlot *>(buf);
            slot->seq = 1;
            slot->value = 0xBEEF;
            fmt::print(stderr, "[T] wrote seq={} value=0x{:X} tid={}\n",
                       slot->seq, slot->value, pylabhub::platform::get_native_thread_id());
            producer->write_commit();

            // Consumer with verify_checksum=false initially.
            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            { hub::SchemaFieldDesc f1, f2; f1.type_str = "uint64"; f2.type_str = "uint64";
              copts.zmq_schema = {f1, f2}; }
            copts.checksum_policy   = ChecksumPolicy::None;
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_TRUE(consumer->start_queue());

            // Read slot — should get seq=1.
            const void *data = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data, nullptr);
            auto *rslot = static_cast<const TestSlot *>(data);
            fmt::print(stderr, "[T] read  seq={} value=0x{:X} last_seq={} tid={}\n",
                       rslot->seq, rslot->value, consumer->last_seq(),
                       pylabhub::platform::get_native_thread_id());
            EXPECT_EQ(rslot->seq, 1u);
            EXPECT_EQ(rslot->value, 0xBEEFu);
            consumer->read_release();

            // Enable checksum verification at runtime.
            consumer->set_verify_checksum(true, false);
            producer->set_checksum_options(true, false);
            fmt::print(stderr, "[T] toggled: verify=true, checksum=true\n");

            // Write slot seq=2 (now WITH ShmQueue checksum).
            buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            slot = static_cast<TestSlot *>(buf);
            slot->seq = 2;
            slot->value = 0xDEAD;
            fmt::print(stderr, "[T] wrote seq={} value=0x{:X}\n", slot->seq, slot->value);
            producer->write_commit();

            // Read — should get seq=2 with valid checksum.
            data = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data, nullptr) << "Runtime toggle: should read checksummed slot";
            rslot = static_cast<const TestSlot *>(data);
            fmt::print(stderr, "[T] read  seq={} value=0x{:X} last_seq={}\n",
                       rslot->seq, rslot->value, consumer->last_seq());
            EXPECT_EQ(rslot->seq, 2u);
            EXPECT_EQ(rslot->value, 0xDEADu);
            consumer->read_release();

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.runtime_verify_checksum_toggle",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// checksum_enforced_roundtrip — Enforced: stamp + verify, round-trip succeeds
// ============================================================================

int checksum_enforced_roundtrip(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            ProducerOptions popts;
            popts.channel_name    = make_test_channel_name("hub.cksum_enf");
            popts.has_shm         = true;
            popts.zmq_schema      = make_schema("uint64");
            popts.shm_config      = make_shm_config();
            popts.shm_config.checksum_policy = ChecksumPolicy::Manual;
            popts.checksum_policy = ChecksumPolicy::Enforced;
            popts.timeout_ms      = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->start_queue());

            // Write a checksummed slot.
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            *static_cast<uint64_t *>(buf) = 0xCAFE;
            producer->write_commit(); // checksum stamped by Enforced policy

            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.checksum_policy   = ChecksumPolicy::Enforced;
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_TRUE(consumer->start_queue());

            // Read succeeds — checksum stamped and verified.
            const void *data = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data, nullptr) << "Enforced: valid checksum should pass";
            EXPECT_EQ(*static_cast<const uint64_t *>(data), 0xCAFEu);
            consumer->read_release();

            // Second write/read round-trip.
            buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            *static_cast<uint64_t *>(buf) = 0xBEEF;
            producer->write_commit();

            data = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data, nullptr) << "Enforced: second read should pass";
            EXPECT_EQ(*static_cast<const uint64_t *>(data), 0xBEEFu);
            consumer->read_release();

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.checksum_enforced_roundtrip",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// checksum_manual_no_stamp_mismatch — Manual: no stamp, verify rejects
// ============================================================================

int checksum_manual_no_stamp_mismatch(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Producer: Manual policy — write_commit does NOT stamp checksum.
            ProducerOptions popts;
            popts.channel_name    = make_test_channel_name("hub.cksum_man");
            popts.has_shm         = true;
            popts.zmq_schema      = make_schema("uint64");
            popts.shm_config      = make_shm_config();
            popts.shm_config.checksum_policy = ChecksumPolicy::Manual;
            popts.checksum_policy = ChecksumPolicy::Manual; // no auto-stamp
            popts.timeout_ms      = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->start_queue());

            // Write a slot WITHOUT checksum.
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            *static_cast<uint64_t *>(buf) = 0xDEAD;
            producer->write_commit(); // no checksum stamp (Manual)

            // Consumer: verify enabled — should reject the unchecksumed slot.
            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.checksum_policy   = ChecksumPolicy::Enforced; // verify enabled
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_TRUE(consumer->start_queue());

            // Read should fail — checksum not stamped by producer, consumer verifies.
            const void *data = consumer->read_acquire(std::chrono::milliseconds{50});
            EXPECT_EQ(data, nullptr)
                << "Manual no-stamp: consumer verify should reject unchecksumed slot";

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.checksum_manual_no_stamp_mismatch",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// checksum_none_roundtrip — None: no checksum, round-trip succeeds
// ============================================================================

int checksum_none_roundtrip(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Both producer and consumer: None — no checksum at all.
            ProducerOptions popts;
            popts.channel_name    = make_test_channel_name("hub.cksum_none");
            popts.has_shm         = true;
            popts.zmq_schema      = make_schema("uint64");
            popts.shm_config      = make_shm_config();
            popts.shm_config.checksum_policy = ChecksumPolicy::None;
            popts.checksum_policy = ChecksumPolicy::None;
            popts.timeout_ms      = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->start_queue());

            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            *static_cast<uint64_t *>(buf) = 0xF00D;
            producer->write_commit(); // no checksum (None policy)

            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = make_schema("uint64");
            copts.checksum_policy   = ChecksumPolicy::None;
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_TRUE(consumer->start_queue());

            // Read succeeds — no checksum involved.
            const void *data = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data, nullptr) << "None: read should succeed without checksum";
            EXPECT_EQ(*static_cast<const uint64_t *>(data), 0xF00Du);
            consumer->read_release();

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.checksum_none_roundtrip",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// open_inbox_client — full broker path with numeric-only inbox schema
// ============================================================================

int open_inbox_client_numeric_schema(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Complex inbox schema: same field types as SHM/ZMQ tests.
            // float64 + float32[3] + uint16 + bytes[5] + string[16] + int64
            // Tests alignment, padding, array, bytes through the full
            // broker→discover→parse→InboxClient→send→recv path.
            nlohmann::json inbox_schema;
            inbox_schema["fields"] = nlohmann::json::array({
                {{"name", "ts"},     {"type", "float64"}},
                {{"name", "data"},   {"type", "float32"}, {"count", 3}},
                {{"name", "status"}, {"type", "uint16"}},
                {{"name", "raw"},    {"type", "bytes"},   {"length", 5}},
                {{"name", "label"},  {"type", "string"},  {"length", 16}},
                {{"name", "seq"},    {"type", "int64"}}
            });
            inbox_schema["packing"] = "aligned";

            std::vector<hub::ZmqSchemaField> inbox_fields = {
                {"float64", 1, 0}, {"float32", 3, 0}, {"uint16", 1, 0},
                {"bytes", 1, 5}, {"string", 1, 16}, {"int64", 1, 0}};

            auto inbox_queue = hub::InboxQueue::bind_at(
                "tcp://127.0.0.1:0", inbox_fields, "aligned");
            ASSERT_NE(inbox_queue, nullptr);
            ASSERT_TRUE(inbox_queue->start());
            const std::string inbox_ep = inbox_queue->actual_endpoint();

            ProducerOptions popts;
            popts.channel_name      = make_test_channel_name("hub.inbox_complex");
            popts.has_shm           = true;
            popts.zmq_schema        = make_schema("uint64");
            popts.shm_config        = make_shm_config();
            popts.timeout_ms        = 3000;
            popts.role_uid          = "PROD-INBOX-TEST-001";
            popts.inbox_endpoint    = inbox_ep;
            popts.inbox_schema_json = inbox_schema.dump();
            popts.inbox_packing     = "aligned";

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());

            ASSERT_TRUE(messenger.query_role_presence("PROD-INBOX-TEST-001", 3000))
                << "Broker should have the producer registered within 3s";

            // Open inbox client via RoleAPIBase (full broker discovery path).
            scripting::RoleHostCore core;
            scripting::RoleAPIBase api(core);
            api.set_messenger(&messenger);
            api.set_uid("CONS-INBOX-TEST-001");

            auto result = api.open_inbox_client("PROD-INBOX-TEST-001");
            ASSERT_TRUE(result.has_value())
                << "open_inbox_client should succeed for registered producer with inbox";

            // Verify item_size matches compute_schema_size for the complex schema.
            auto [layout, expected_size] = hub::compute_field_layout(
                hub::to_field_descs(result->spec.fields), "aligned");
            EXPECT_GT(expected_size, 0u);
            EXPECT_EQ(result->item_size, expected_size)
                << "item_size must match compute_schema_size";

            // Verify all 6 fields were parsed.
            ASSERT_EQ(result->spec.fields.size(), 6u);
            EXPECT_EQ(result->spec.fields[0].type_str, "float64");
            EXPECT_EQ(result->spec.fields[1].type_str, "float32");
            EXPECT_EQ(result->spec.fields[1].count, 3u);
            EXPECT_EQ(result->spec.fields[2].type_str, "uint16");
            EXPECT_EQ(result->spec.fields[3].type_str, "bytes");
            EXPECT_EQ(result->spec.fields[3].length, 5u);
            EXPECT_EQ(result->spec.fields[4].type_str, "string");
            EXPECT_EQ(result->spec.fields[4].length, 16u);
            EXPECT_EQ(result->spec.fields[5].type_str, "int64");

            // Send a message through the InboxClient and receive it on InboxQueue.
            auto &client = *result->client;
            void *buf = client.acquire();
            ASSERT_NE(buf, nullptr);

            auto *p = static_cast<uint8_t *>(buf);
            double ts = 2.71828;
            float data_arr[3] = {-1.0f, 0.0f, 1.0f};
            uint16_t status = 0x1234;
            uint8_t raw[5] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
            char label[16] = "inbox_test!";
            int64_t seq = -42LL;

            std::memcpy(p + layout[0].offset, &ts, sizeof(ts));
            std::memcpy(p + layout[1].offset, data_arr, sizeof(data_arr));
            std::memcpy(p + layout[2].offset, &status, sizeof(status));
            std::memcpy(p + layout[3].offset, raw, 5);
            std::memcpy(p + layout[4].offset, label, 16);
            std::memcpy(p + layout[5].offset, &seq, sizeof(seq));

            // recv in background, send from main thread.
            const hub::InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async, [&] {
                item = inbox_queue->recv_one(std::chrono::milliseconds{2000});
                if (item) inbox_queue->send_ack(0);
                return item != nullptr;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
            uint8_t ack = client.send(std::chrono::milliseconds{1500});

            ASSERT_TRUE(fut.get()) << "InboxQueue::recv_one should receive the message";
            ASSERT_NE(item, nullptr);
            ASSERT_NE(item->data, nullptr);
            EXPECT_EQ(ack, 0u);

            // Verify all fields from received message.
            const auto *r = static_cast<const uint8_t *>(item->data);
            double read_ts = 0;
            float read_data[3] = {};
            uint16_t read_status = 0;
            uint8_t read_raw[5] = {};
            char read_label[16] = {};
            int64_t read_seq = 0;

            std::memcpy(&read_ts, r + layout[0].offset, sizeof(read_ts));
            std::memcpy(read_data, r + layout[1].offset, sizeof(read_data));
            std::memcpy(&read_status, r + layout[2].offset, sizeof(read_status));
            std::memcpy(read_raw, r + layout[3].offset, 5);
            std::memcpy(read_label, r + layout[4].offset, 16);
            std::memcpy(&read_seq, r + layout[5].offset, sizeof(read_seq));

            EXPECT_DOUBLE_EQ(read_ts, 2.71828);
            EXPECT_FLOAT_EQ(read_data[0], -1.0f);
            EXPECT_FLOAT_EQ(read_data[1], 0.0f);
            EXPECT_FLOAT_EQ(read_data[2], 1.0f);
            EXPECT_EQ(read_status, 0x1234u);
            EXPECT_EQ(read_raw[0], 0xAAu);
            EXPECT_EQ(read_raw[4], 0xEEu);
            EXPECT_STREQ(read_label, "inbox_test!");
            EXPECT_EQ(read_seq, -42LL);

            inbox_queue->stop();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.open_inbox_client_numeric_schema",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_multifield_schema_roundtrip — multi-type schema via SHM forwarding API
// ============================================================================

int shm_multifield_schema_roundtrip(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();
            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            // Complex schema: scalar + array + string + bytes + mixed alignment.
            // Tests padding/alignment: bytes[5] is byte-aligned (no padding before it),
            // followed by int64 which needs 8-byte alignment → 3 bytes padding after bytes.
            //
            // Aligned layout:
            //   [0]  float64 ts          offset=0  size=8
            //   [1]  float32[3] data     offset=8  size=12
            //   [2]  uint16 status       offset=20 size=2
            //   [3]  bytes[5] raw        offset=22 size=5  (byte-aligned, no pad before)
            //   [4]  string[16] label    offset=27 size=16 (byte-aligned, no pad before)
            //        (pad 5 bytes to align int64 to 8)
            //   [5]  int64 seq           offset=48 size=8
            //   Total: 56 bytes (padded to 8-align)
            auto schema = std::vector<hub::SchemaFieldDesc>{
                {"float64", 1, 0},   // ts
                {"float32", 3, 0},   // data[3]
                {"uint16",  1, 0},   // status
                {"bytes",   1, 5},   // raw (5 bytes — odd, forces alignment pad before next int64)
                {"string",  1, 16},  // label (16 bytes)
                {"int64",   1, 0}};  // seq

            ProducerOptions popts;
            popts.channel_name = make_test_channel_name("hub.multi_shm");
            popts.has_shm      = true;
            popts.zmq_schema   = schema;
            popts.zmq_packing  = "aligned";
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->start_queue());

            auto [layout, expected_size] = hub::compute_field_layout(schema, "aligned");
            EXPECT_GT(expected_size, 0u);
            EXPECT_EQ(producer->queue_item_size(), expected_size)
                << "Producer queue item_size must match compute_field_layout";

            // Write all fields at correct offsets.
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            auto *p = static_cast<uint8_t *>(buf);

            double ts = 1.23456789;
            float data_arr[3] = {1.0f, 2.5f, -3.75f};
            uint16_t status = 0xBEEF;
            uint8_t raw[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA};
            char label[16] = "hello_schema!";
            int64_t seq = -999999LL;

            std::memcpy(p + layout[0].offset, &ts, sizeof(ts));
            std::memcpy(p + layout[1].offset, data_arr, sizeof(data_arr));
            std::memcpy(p + layout[2].offset, &status, sizeof(status));
            std::memcpy(p + layout[3].offset, raw, 5);
            std::memcpy(p + layout[4].offset, label, 16);
            std::memcpy(p + layout[5].offset, &seq, sizeof(seq));
            producer->write_commit();

            // Consumer connects with same schema.
            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.shm_shared_secret = kTestShmSecret;
            copts.zmq_schema        = schema;
            copts.zmq_packing       = "aligned";
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            EXPECT_TRUE(consumer->start_queue());
            EXPECT_EQ(consumer->queue_item_size(), expected_size);

            // Read and verify every field.
            const void *data_ptr = consumer->read_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(data_ptr, nullptr);
            const auto *r = static_cast<const uint8_t *>(data_ptr);

            double read_ts = 0;
            float read_data[3] = {};
            uint16_t read_status = 0;
            uint8_t read_raw[5] = {};
            char read_label[16] = {};
            int64_t read_seq = 0;

            std::memcpy(&read_ts, r + layout[0].offset, sizeof(read_ts));
            std::memcpy(read_data, r + layout[1].offset, sizeof(read_data));
            std::memcpy(&read_status, r + layout[2].offset, sizeof(read_status));
            std::memcpy(read_raw, r + layout[3].offset, 5);
            std::memcpy(read_label, r + layout[4].offset, 16);
            std::memcpy(&read_seq, r + layout[5].offset, sizeof(read_seq));

            EXPECT_DOUBLE_EQ(read_ts, 1.23456789);
            EXPECT_FLOAT_EQ(read_data[0], 1.0f);
            EXPECT_FLOAT_EQ(read_data[1], 2.5f);
            EXPECT_FLOAT_EQ(read_data[2], -3.75f);
            EXPECT_EQ(read_status, 0xBEEFu);
            EXPECT_EQ(read_raw[0], 0xDEu);
            EXPECT_EQ(read_raw[4], 0xCAu);
            EXPECT_STREQ(read_label, "hello_schema!");
            EXPECT_EQ(read_seq, -999999LL);
            consumer->read_release();

            consumer->close();
            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "hub_api.shm_multifield_schema_roundtrip",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// zmq_multifield_schema_roundtrip — multi-type schema via ZMQ forwarding API
// ============================================================================

int zmq_multifield_schema_roundtrip(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            // ZMQ needs separate Messenger instances for producer and consumer.
            Messenger prod_m, cons_m;
            ASSERT_TRUE(prod_m.connect(broker.endpoint, broker.pubkey));
            ASSERT_TRUE(cons_m.connect(broker.endpoint, broker.pubkey));

            // Complex schema: same as SHM test — validates ZMQ wire format preserves
            // scalars, arrays, strings, bytes through msgpack pack/unpack.
            auto schema = std::vector<hub::SchemaFieldDesc>{
                {"float64", 1, 0},
                {"float32", 3, 0},
                {"uint16",  1, 0},
                {"bytes",   1, 5},
                {"string",  1, 16},
                {"int64",   1, 0}};

            ProducerOptions popts;
            popts.channel_name      = make_test_channel_name("hub.multi_zmq");
            popts.has_shm           = false;
            popts.data_transport    = "zmq";
            popts.zmq_node_endpoint = "tcp://127.0.0.1:0";
            popts.zmq_bind          = true;
            popts.zmq_schema        = schema;
            popts.zmq_packing       = "aligned";
            popts.zmq_buffer_depth  = 16;
            popts.timeout_ms        = 3000;

            auto producer = Producer::create(prod_m, popts);
            ASSERT_TRUE(producer.has_value());

            auto [layout, expected_size] = hub::compute_field_layout(schema, "aligned");
            EXPECT_EQ(producer->queue_item_size(), expected_size);

            // Write all fields.
            void *buf = producer->write_acquire(std::chrono::milliseconds{1000});
            ASSERT_NE(buf, nullptr);
            auto *p = static_cast<uint8_t *>(buf);

            double ts = 3.14159;
            float data_arr[3] = {10.0f, 20.0f, 30.0f};
            uint16_t status = 0xCAFE;
            uint8_t raw[5] = {0x01, 0x02, 0x03, 0x04, 0x05};
            char label[16] = "zmq_schema_ok";
            int64_t seq = 1234567890LL;

            std::memcpy(p + layout[0].offset, &ts, sizeof(ts));
            std::memcpy(p + layout[1].offset, data_arr, sizeof(data_arr));
            std::memcpy(p + layout[2].offset, &status, sizeof(status));
            std::memcpy(p + layout[3].offset, raw, 5);
            std::memcpy(p + layout[4].offset, label, 16);
            std::memcpy(p + layout[5].offset, &seq, sizeof(seq));
            producer->write_commit();

            // Consumer discovers ZMQ from broker.
            ConsumerOptions copts;
            copts.channel_name      = popts.channel_name;
            copts.zmq_schema        = schema;
            copts.zmq_packing       = "aligned";
            copts.zmq_buffer_depth  = 16;
            copts.timeout_ms        = 3000;

            auto consumer = Consumer::connect(cons_m, copts);
            ASSERT_TRUE(consumer.has_value());

            std::this_thread::sleep_for(std::chrono::milliseconds{100});

            // Read and verify every field.
            const void *data_ptr = consumer->read_acquire(std::chrono::milliseconds{2000});
            ASSERT_NE(data_ptr, nullptr) << "Consumer should receive the ZMQ frame";
            const auto *r = static_cast<const uint8_t *>(data_ptr);

            double read_ts = 0;
            float read_data[3] = {};
            uint16_t read_status = 0;
            uint8_t read_raw[5] = {};
            char read_label[16] = {};
            int64_t read_seq = 0;

            std::memcpy(&read_ts, r + layout[0].offset, sizeof(read_ts));
            std::memcpy(read_data, r + layout[1].offset, sizeof(read_data));
            std::memcpy(&read_status, r + layout[2].offset, sizeof(read_status));
            std::memcpy(read_raw, r + layout[3].offset, 5);
            std::memcpy(read_label, r + layout[4].offset, 16);
            std::memcpy(&read_seq, r + layout[5].offset, sizeof(read_seq));

            EXPECT_DOUBLE_EQ(read_ts, 3.14159);
            EXPECT_FLOAT_EQ(read_data[0], 10.0f);
            EXPECT_FLOAT_EQ(read_data[1], 20.0f);
            EXPECT_FLOAT_EQ(read_data[2], 30.0f);
            EXPECT_EQ(read_status, 0xCAFEu);
            EXPECT_EQ(read_raw[0], 0x01u);
            EXPECT_EQ(read_raw[4], 0x05u);
            EXPECT_STREQ(read_label, "zmq_schema_ok");
            EXPECT_EQ(read_seq, 1234567890LL);
            consumer->read_release();

            consumer->close();
            producer->close();
            broker.stop_and_join();
            cons_m.disconnect();
            prod_m.disconnect();
        },
        "hub_api.zmq_multifield_schema_roundtrip",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::hub_api

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct HubApiWorkerRegistrar
{
    HubApiWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int {
                if (argc < 2)
                {
                    return -1;
                }
                std::string_view mode = argv[1];
                const auto       dot  = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "hub_api")
                {
                    return -1;
                }
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_api;
                if (scenario == "producer_create_pubsub")
                    return producer_create_pubsub(argc, argv);
                if (scenario == "producer_create_with_shm")
                    return producer_create_with_shm(argc, argv);
                if (scenario == "consumer_connect_e2e")
                    return consumer_connect_e2e(argc, argv);
                if (scenario == "consumer_hello_tracked")
                    return consumer_hello_tracked(argc, argv);
                if (scenario == "active_producer_consumer_callbacks")
                    return active_producer_consumer_callbacks(argc, argv);
                if (scenario == "peer_callback_on_consumer_join")
                    return peer_callback_on_consumer_join(argc, argv);
                if (scenario == "non_template_factory")
                    return non_template_factory(argc, argv);
                if (scenario == "managed_producer_lifecycle")
                    return managed_producer_lifecycle(argc, argv);
                if (scenario == "consumer_shm_secret_mismatch")
                    return consumer_shm_secret_mismatch(argc, argv);
                if (scenario == "consumer_bye_tracked")
                    return consumer_bye_tracked(argc, argv);
                if (scenario == "consumer_shm_read_e2e")
                    return consumer_shm_read_e2e(argc, argv);
                if (scenario == "consumer_read_shm_sync")
                    return consumer_read_shm_sync(argc, argv);
                if (scenario == "producer_consumer_idempotency")
                    return producer_consumer_idempotency(argc, argv);
                if (scenario == "producer_consumer_ctrl_messaging")
                    return producer_consumer_ctrl_messaging(argc, argv);
                if (scenario == "consumer_destructor_bye")
                    return consumer_destructor_bye(argc, argv);
                if (scenario == "producer_channel_identity")
                    return producer_channel_identity(argc, argv);
                if (scenario == "consumer_identity_in_shm")
                    return consumer_identity_in_shm(argc, argv);
                if (scenario == "producer_consumer_forwarding_api")
                    return producer_consumer_forwarding_api(argc, argv);
                if (scenario == "construction_time_checksum")
                    return construction_time_checksum(argc, argv);
                if (scenario == "flexzone_through_service_layer")
                    return flexzone_through_service_layer(argc, argv);
                if (scenario == "queue_metrics_forwarding")
                    return queue_metrics_forwarding(argc, argv);
                if (scenario == "zmq_forwarding_api")
                    return zmq_forwarding_api(argc, argv);
                if (scenario == "forwarding_error_paths")
                    return forwarding_error_paths(argc, argv);
                if (scenario == "runtime_verify_checksum_toggle")
                    return runtime_verify_checksum_toggle(argc, argv);
                if (scenario == "checksum_enforced_roundtrip")
                    return checksum_enforced_roundtrip(argc, argv);
                if (scenario == "checksum_manual_no_stamp_mismatch")
                    return checksum_manual_no_stamp_mismatch(argc, argv);
                if (scenario == "checksum_none_roundtrip")
                    return checksum_none_roundtrip(argc, argv);
                if (scenario == "open_inbox_client_numeric_schema")
                    return open_inbox_client_numeric_schema(argc, argv);
                if (scenario == "shm_multifield_schema_roundtrip")
                    return shm_multifield_schema_roundtrip(argc, argv);
                if (scenario == "zmq_multifield_schema_roundtrip")
                    return zmq_multifield_schema_roundtrip(argc, argv);
                fmt::print(stderr, "ERROR: Unknown hub_api scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static HubApiWorkerRegistrar g_hub_api_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
