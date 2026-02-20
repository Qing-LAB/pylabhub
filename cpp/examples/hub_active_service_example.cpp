/**
 * @file hub_active_service_example.cpp
 * @brief Example: Full Hub API — BrokerService + Producer + Consumer.
 *
 * Self-contained demo that runs Broker, Producer, and Consumer in a single
 * executable using std::thread. In a real deployment these would typically
 * live in separate processes or applications.
 *
 * Key concepts shown:
 *  - BrokerService in a background thread; on_ready callback provides the
 *    bound endpoint and Z85 server public key for clients to connect.
 *  - Messenger — ZMQ connection to the broker (one per role in this example).
 *  - Producer::create<F,D>() — creates ZMQ channel + SHM DataBlock.
 *  - Consumer::connect<F,D>() — discovers channel via broker, attaches SHM.
 *  - push() / synced_write() — async and synchronous SHM slot writes.
 *  - pull() — synchronous SHM slot read from the calling thread.
 *  - ZMQ broadcast from producer to consumer alongside SHM transfer.
 *  - Producer callbacks: on_consumer_joined, on_consumer_left.
 *  - Consumer callbacks: on_zmq_data.
 *  - Clean shutdown in correct order: consumer → producer → broker.
 */
#include "plh_datahub.hpp"
#include "utils/broker_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::utils;
using namespace std::chrono_literals;

// ─── Shared data types ────────────────────────────────────────────────────────

struct ControlFlexZone
{
    uint64_t sequence{0};    ///< Last published sequence number.
    bool     shutdown{false};
    uint32_t _pad{0};
};
static_assert(std::is_trivially_copyable_v<ControlFlexZone>);

PYLABHUB_SCHEMA_BEGIN(ControlFlexZone)
    PYLABHUB_SCHEMA_MEMBER(sequence)
    PYLABHUB_SCHEMA_MEMBER(shutdown)
PYLABHUB_SCHEMA_END(ControlFlexZone)

struct MeasurementData
{
    uint64_t timestamp_ns{0};
    double   value{0.0};
    uint32_t sequence{0};
    uint32_t _pad{0};
};
static_assert(std::is_trivially_copyable_v<MeasurementData>);

PYLABHUB_SCHEMA_BEGIN(MeasurementData)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(value)
    PYLABHUB_SCHEMA_MEMBER(sequence)
PYLABHUB_SCHEMA_END(MeasurementData)

// ─── Configuration ────────────────────────────────────────────────────────────

constexpr uint64_t kShmSecret      = 0xFEED'CAFE'DEAD'BEEF;
constexpr int      kPublishCount    = 10;
constexpr int      kBrokerTimeoutMs = 5000;

// ─── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    // Initialise Logger and CryptoUtils (for ZMQ CurveZMQ keypair generation).
    // Do NOT call hub::GetLifecycleModule() here — Messengers are created manually.
    LifecycleGuard lifecycle(MakeModDefList(
        Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule()));

    // ─── 1. Start broker in a background thread ────────────────────────────

    std::string            broker_endpoint;
    std::string            broker_pubkey;
    std::mutex             ready_mu;
    std::condition_variable ready_cv;
    bool                   broker_ready = false;

    pylabhub::broker::BrokerService::Config broker_cfg;
    broker_cfg.endpoint = "tcp://127.0.0.1:0"; // OS assigns free port
    broker_cfg.on_ready = [&](const std::string& ep, const std::string& pk)
    {
        std::lock_guard lock(ready_mu);
        broker_endpoint = ep;
        broker_pubkey   = pk;
        broker_ready    = true;
        ready_cv.notify_all();
    };

    pylabhub::broker::BrokerService broker(broker_cfg);
    std::thread broker_thread([&broker] { broker.run(); });

    // Wait until broker has bound.
    {
        std::unique_lock lock(ready_mu);
        ready_cv.wait_for(lock, 5s, [&] { return broker_ready; });
        if (!broker_ready)
        {
            std::cerr << "Broker failed to start\n";
            broker.stop();
            broker_thread.join();
            return 1;
        }
    }
    std::cout << "Broker ready at " << broker_endpoint << "\n";
    std::cout << "Server key: " << broker_pubkey << "\n";

    // ─── 2. Producer Messenger + channel creation ──────────────────────────

    Messenger producer_msg;
    if (!producer_msg.connect(broker_endpoint, broker_pubkey))
    {
        std::cerr << "Producer Messenger failed to connect to broker\n";
        broker.stop();
        broker_thread.join();
        return 1;
    }

    DataBlockConfig shm_cfg{};
    shm_cfg.policy               = DataBlockPolicy::RingBuffer;
    shm_cfg.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
    shm_cfg.shared_secret        = kShmSecret;
    shm_cfg.ring_buffer_capacity = 4;
    shm_cfg.physical_page_size   = DataBlockPageSize::Size4K;
    shm_cfg.flex_zone_size       = 4096;
    shm_cfg.checksum_policy      = ChecksumPolicy::Enforced;

    ProducerOptions prod_opts;
    prod_opts.channel_name = "measurements";
    prod_opts.pattern      = ChannelPattern::PubSub;
    prod_opts.has_shm      = true;
    prod_opts.shm_config   = shm_cfg;

    std::atomic<int> consumer_count{0};

    auto producer_opt = Producer::create<ControlFlexZone, MeasurementData>(
        producer_msg, prod_opts);
    if (!producer_opt)
    {
        std::cerr << "Failed to create Producer\n";
        broker.stop();
        broker_thread.join();
        return 1;
    }
    Producer& producer = *producer_opt;

    // Register peer callbacks before start().
    producer.on_consumer_joined([&consumer_count](const std::string& /*identity*/)
    {
        ++consumer_count;
        std::cout << "[producer] Consumer joined  (total=" << consumer_count.load() << ")\n";
    });
    producer.on_consumer_left([&consumer_count](const std::string& /*identity*/)
    {
        --consumer_count;
        std::cout << "[producer] Consumer left (total=" << consumer_count.load() << ")\n";
    });

    producer.start();
    std::cout << "[producer] Active — channel 'measurements' published\n";

    // ─── 3. Consumer Messenger + channel connect ───────────────────────────

    Messenger consumer_msg;
    if (!consumer_msg.connect(broker_endpoint, broker_pubkey))
    {
        std::cerr << "Consumer Messenger failed to connect to broker\n";
        producer.close();
        broker.stop();
        broker_thread.join();
        return 1;
    }

    ConsumerOptions cons_opts;
    cons_opts.channel_name       = "measurements";
    cons_opts.shm_shared_secret  = kShmSecret;
    cons_opts.expected_shm_config = shm_cfg;

    auto consumer_opt = Consumer::connect<ControlFlexZone, MeasurementData>(
        consumer_msg, cons_opts);
    if (!consumer_opt)
    {
        std::cerr << "Failed to connect Consumer\n";
        producer.close();
        broker.stop();
        broker_thread.join();
        return 1;
    }
    Consumer& consumer = *consumer_opt;

    // Register ZMQ data callback before start().
    consumer.on_zmq_data([](std::span<const std::byte> data)
    {
        std::cout << "[consumer] ZMQ data received (" << data.size() << " bytes)\n";
    });

    consumer.start();
    std::cout << "[consumer] Active — subscribed to 'measurements'\n";

    // Give peer_thread a moment to see the consumer's HELLO.
    std::this_thread::sleep_for(100ms);

    // ─── 4. Publish data (ZMQ broadcast + SHM push) ───────────────────────

    for (int i = 0; i < kPublishCount; ++i)
    {
        // Async SHM write via the write_thread (non-blocking for caller).
        producer.push<ControlFlexZone, MeasurementData>(
            [i](WriteProcessorContext<ControlFlexZone, MeasurementData>& ctx)
            {
                // Update FlexZone.
                ctx.flexzone().sequence = static_cast<uint64_t>(i + 1);
                ctx.flexzone().shutdown = (i + 1 == kPublishCount);

                // Acquire a write slot.
                for (auto& slot : ctx.txn.slots(200ms))
                {
                    if (!slot.is_ok())
                        break;
                    MeasurementData& d = slot.content().get();
                    d.timestamp_ns     = pylabhub::platform::monotonic_time_ns();
                    d.value            = static_cast<double>(i) * 1.5;
                    d.sequence         = static_cast<uint32_t>(i);
                    break;
                }

                // Also broadcast a lightweight ZMQ notification to all consumers.
                const uint32_t seq = static_cast<uint32_t>(i);
                ctx.broadcast(&seq, sizeof(seq));
            });

        std::cout << "[producer] Pushed frame " << i << "\n";
        std::this_thread::sleep_for(30ms);
    }

    // Wait for write_thread to flush the last push().
    std::this_thread::sleep_for(200ms);

    // ─── 5. Consumer pull from SHM (synchronous, calling thread) ──────────

    std::cout << "[consumer] Pulling SHM frames...\n";
    for (int i = 0; i < kPublishCount; ++i)
    {
        bool got = consumer.pull<ControlFlexZone, MeasurementData>(
            [&i](ReadProcessorContext<ControlFlexZone, MeasurementData>& ctx)
            {
                for (auto& slot : ctx.txn.slots(200ms))
                {
                    if (!slot.is_ok())
                        break;
                    const MeasurementData& d = slot.content().get();
                    std::cout << "[consumer] SHM seq=" << d.sequence
                              << "  value=" << d.value << "\n";
                    i = static_cast<int>(d.sequence); // advance outer loop
                    break;
                }
            });
        if (!got)
            break;
        if (i + 1 >= kPublishCount)
            break;
    }

    // ─── 6. Shutdown: consumer first, then producer, then broker ──────────

    std::cout << "Shutting down...\n";
    consumer.close();   // sends BYE to producer + CONSUMER_DEREG_REQ to broker
    producer.close();   // sends DEREG_REQ to broker; stops threads
    broker.stop();      // signals run() to exit
    broker_thread.join();

    std::cout << "Example complete\n";
    return 0;
}
