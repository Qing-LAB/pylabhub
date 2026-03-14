// tests/test_layer3_datahub/workers/datahub_e2e_workers.cpp
// End-to-end multi-process integration test.
//
// Pipeline: broker (in-thread) ← Messenger → producer subprocess ← shm → consumer subprocess
#include "datahub_e2e_workers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"

#include "utils/broker_service.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

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

namespace pylabhub::tests::worker::e2e
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Shared constants
// ============================================================================
namespace
{

// Number of slots the producer writes; consumer reads the latest (Latest_only policy)
constexpr uint32_t kNumSlots = 5;
// Pre-shared DataBlock secret for the E2E test
constexpr uint64_t kE2ESecret = 0xE2EC0FFEEC0FFEULL;

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

} // anonymous namespace

// ============================================================================
// orchestrator — starts broker, spawns producer + consumer sub-workers
// ============================================================================

int orchestrator(int /*argc*/, char** /*argv*/)
{
    return run_gtest_worker(
        []()
        {
            // Start broker with CurveZMQ
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            // Unique channel name so concurrent test runs don't conflict
            const std::string channel = make_test_channel_name("E2E");

            // Spawn producer with ready-signal; it writes 5 slots then signals ready
            WorkerProcess producer(g_self_exe_path, "e2e.e2e_producer",
                                   {broker.endpoint, broker.pubkey, channel},
                                   false, /*with_ready_signal=*/true);

            // Block until producer has written all slots and signalled ready
            producer.wait_for_ready();

            // Spawn consumer; it discovers channel, reads data, verifies, exits
            WorkerProcess consumer(g_self_exe_path, "e2e.e2e_consumer",
                                   {broker.endpoint, broker.pubkey, channel},
                                   false, /*with_ready_signal=*/false);

            // Consumer reads and exits first
            consumer.wait_for_exit();
            expect_worker_ok(consumer);

            // Producer finishes its sleep and exits
            producer.wait_for_exit();
            expect_worker_ok(producer);

            broker.stop_and_join();
        },
        "e2e.orchestrator",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// e2e_producer — creates DataBlock, writes kNumSlots slots, signals ready
// ============================================================================

int e2e_producer(int argc, char** argv)
{
    if (argc < 5)
    {
        fmt::print(stderr, "ERROR: e2e_producer requires argv[2..4]: endpoint pubkey channel\n");
        return 1;
    }
    const std::string endpoint = argv[2];
    const std::string pubkey = argv[3];
    const std::string channel = argv[4];

    return run_gtest_worker(
        [&endpoint, &pubkey, &channel]()
        {
            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(endpoint, pubkey))
                << "e2e_producer: Messenger::connect failed";

            // Create the DataBlock (producer side)
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = kE2ESecret;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer =
                create_datablock_producer_impl(channel, DataBlockPolicy::RingBuffer, config,
                                              nullptr, nullptr);
            ASSERT_NE(producer, nullptr) << "e2e_producer: create_datablock_producer_impl failed";

            // Register with broker (fire-and-forget)
            ProducerInfo pinfo{};
            pinfo.shm_name = channel;
            pinfo.producer_pid = pylabhub::platform::get_pid();
            pinfo.schema_hash.assign(32, '\0');
            pinfo.schema_version = 1;
            messenger.register_producer(channel, pinfo);

            // Write kNumSlots slots with incrementing uint32_t values
            for (uint32_t i = 0; i < kNumSlots; ++i)
            {
                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr) << "e2e_producer: acquire_write_slot failed at i=" << i;
                EXPECT_TRUE(wh->write(&i, sizeof(i)));
                EXPECT_TRUE(wh->commit(sizeof(i)));
                EXPECT_TRUE(producer->release_write_slot(*wh));
            }

            // Signal the orchestrator that data is ready
            signal_test_ready();

            // Keep shm alive while the consumer reads (5s is generous)
            std::this_thread::sleep_for(std::chrono::seconds(5));

            messenger.disconnect();
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "e2e.e2e_producer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// e2e_consumer — discovers channel, attaches DataBlock, reads and verifies data
// ============================================================================

int e2e_consumer(int argc, char** argv)
{
    if (argc < 5)
    {
        fmt::print(stderr, "ERROR: e2e_consumer requires argv[2..4]: endpoint pubkey channel\n");
        return 1;
    }
    const std::string endpoint = argv[2];
    const std::string pubkey = argv[3];
    const std::string channel = argv[4];

    return run_gtest_worker(
        [&endpoint, &pubkey, &channel]()
        {
            Messenger& messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(endpoint, pubkey))
                << "e2e_consumer: Messenger::connect failed";

            // Discover the producer channel via broker
            auto cinfo = messenger.discover_producer(channel, 5000);
            ASSERT_TRUE(cinfo.has_value())
                << "e2e_consumer: discover_producer returned nullopt — channel not registered?";

            // Register this process as a consumer with the broker
            messenger.register_consumer(channel, *cinfo);

            // Attach to the DataBlock
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = kE2ESecret;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto consumer = find_datablock_consumer_impl(cinfo->shm_name, kE2ESecret, &config,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr)
                << "e2e_consumer: find_datablock_consumer_impl failed for shm '" << cinfo->shm_name
                << "'";

            // With Latest_only, acquire_consume_slot returns the most recently committed slot.
            // The producer wrote slots 0..4; the latest is slot 4 (value == 4).
            auto ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr) << "e2e_consumer: acquire_consume_slot timed out";

            uint32_t val = 0;
            EXPECT_TRUE(ch->read(&val, sizeof(val)));
            EXPECT_EQ(val, kNumSlots - 1)
                << "e2e_consumer: expected latest slot value=" << (kNumSlots - 1) << " got=" << val;
            ch.reset();

            // Deregister consumer with broker
            messenger.deregister_consumer(channel);

            messenger.disconnect();
            consumer.reset();
        },
        "e2e.e2e_consumer",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::e2e

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct E2EWorkerRegistrar
{
    E2EWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "e2e")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::e2e;
                if (scenario == "orchestrator")
                    return orchestrator(argc, argv);
                if (scenario == "e2e_producer")
                    return e2e_producer(argc, argv);
                if (scenario == "e2e_consumer")
                    return e2e_consumer(argc, argv);
                fmt::print(stderr, "ERROR: Unknown e2e scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static E2EWorkerRegistrar g_e2e_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
