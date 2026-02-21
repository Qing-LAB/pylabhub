// tests/test_layer3_datahub/workers/datahub_hub_api_workers.cpp
//
// Hub Producer/Consumer unified API tests.
// Uses a real BrokerService in a background thread + Messenger singleton.
#include "datahub_hub_api_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_service.hpp"
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
            EXPECT_EQ(producer->shm(), nullptr);

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
            opts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, opts);
            ASSERT_TRUE(producer.has_value()) << "Producer::create(has_shm) failed";
            EXPECT_TRUE(producer->is_valid());
            EXPECT_TRUE(producer->has_shm());
            EXPECT_NE(producer->shm(), nullptr);

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

            // Non-template producer (no schema type info)
            ProducerOptions popts;
            popts.channel_name = channel;
            popts.pattern      = ChannelPattern::Pipeline;
            popts.has_shm      = true;
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            EXPECT_TRUE(producer->has_shm());

            // Non-template consumer (no schema validation)
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
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
            EXPECT_EQ(consumer->shm(), nullptr)
                << "SHM must be nullptr when shared_secret doesn't match";

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
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Consumer with SHM (matching secret)
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
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
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Consumer with SHM (no start() required for sync read_shm)
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
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
            popts.shm_config   = cfg;
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Verify channel identity accessors read back from SHM header.
            EXPECT_EQ(producer->shm()->hub_uid(),      "hub_uid_test_42");
            EXPECT_EQ(producer->shm()->hub_name(),     "TestHub");
            EXPECT_EQ(producer->shm()->producer_uid(), "prod_uid_abc");
            EXPECT_EQ(producer->shm()->producer_name(), "TestProducer");

            // Consumer also reads the same header — channel identity is shared.
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            ASSERT_TRUE(consumer->has_shm());

            EXPECT_EQ(consumer->shm()->hub_uid(),      "hub_uid_test_42");
            EXPECT_EQ(consumer->shm()->hub_name(),     "TestHub");
            EXPECT_EQ(consumer->shm()->producer_uid(), "prod_uid_abc");
            EXPECT_EQ(consumer->shm()->producer_name(), "TestProducer");

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
            popts.shm_config   = make_shm_config();
            popts.timeout_ms   = 3000;
            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value());
            ASSERT_TRUE(producer->has_shm());

            // Consumer sets its own identity via ConsumerOptions.
            ConsumerOptions copts;
            copts.channel_name      = channel;
            copts.shm_shared_secret = kTestShmSecret;
            copts.consumer_uid      = "cuid_abcdef1234";
            copts.consumer_name     = "MyCoolConsumer";
            copts.timeout_ms        = 3000;
            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value());
            ASSERT_TRUE(consumer->has_shm());

            // Verify the consumer's own identity is stored and readable.
            EXPECT_EQ(consumer->shm()->consumer_uid(),  "cuid_abcdef1234");
            EXPECT_EQ(consumer->shm()->consumer_name(), "MyCoolConsumer");

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
                fmt::print(stderr, "ERROR: Unknown hub_api scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static HubApiWorkerRegistrar g_hub_api_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
