/**
 * @file cpp_processor_template.cpp
 * @brief Template: C++ processor pipeline using hub::Processor + hub::Queue.
 *
 * Self-contained demo of a processor pipeline using pure C++ (no Python):
 *   Producer (SHM) → Processor (SHM-to-SHM) → Consumer (SHM)
 *
 * Key concepts shown:
 *  - LifecycleGuard for ordered init/shutdown of Logger and other services.
 *  - BrokerService in a background thread with on_ready callback.
 *  - Messenger connections for Producer and Consumer.
 *  - hub::Producer::create<F,D>() and hub::Consumer::connect<F,D>().
 *  - hub::ShmQueue (non-owning) wrapping existing DataBlock objects.
 *  - hub::Processor::create() with typed handler via set_process_handler<>().
 *  - Hub::Processor timeout handler for idle processing.
 *  - Clean shutdown in correct order.
 *
 * Build:
 *   cmake -S . -B build -DPYLABHUB_BUILD_EXAMPLES=ON
 *   cmake --build build --target example_processor_pipeline
 *
 * Run:
 *   ./build/examples/example_processor_pipeline
 */
#include "plh_datahub.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_processor.hpp"
#include "utils/hub_shm_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::utils;
using pylabhub::broker::BrokerService;
using namespace std::chrono_literals;

// ── Data types ─────────────────────────────────────────────────────────────────

/// Input slot: raw sensor reading.
struct RawReading
{
    float    value{0.0f};
    uint64_t timestamp{0};
};
static_assert(std::is_trivially_copyable_v<RawReading>);

PYLABHUB_SCHEMA_BEGIN(RawReading)
    PYLABHUB_SCHEMA_MEMBER(value)
    PYLABHUB_SCHEMA_MEMBER(timestamp)
PYLABHUB_SCHEMA_END(RawReading)

/// Output slot: scaled result.
struct ScaledResult
{
    double   scaled_value{0.0};
    uint64_t timestamp{0};
    uint32_t iteration{0};
    uint32_t _pad{0};
};
static_assert(std::is_trivially_copyable_v<ScaledResult>);

PYLABHUB_SCHEMA_BEGIN(ScaledResult)
    PYLABHUB_SCHEMA_MEMBER(scaled_value)
    PYLABHUB_SCHEMA_MEMBER(timestamp)
    PYLABHUB_SCHEMA_MEMBER(iteration)
PYLABHUB_SCHEMA_END(ScaledResult)

/// Empty flexzone (required for template specialization).
struct EmptyFlexZone
{
    uint8_t _unused{0};
};
static_assert(std::is_trivially_copyable_v<EmptyFlexZone>);

PYLABHUB_SCHEMA_BEGIN(EmptyFlexZone)
    PYLABHUB_SCHEMA_MEMBER(_unused)
PYLABHUB_SCHEMA_END(EmptyFlexZone)

// ── Main ───────────────────────────────────────────────────────────────────────

int main()
{
    // 1. Init lifecycle (Logger, etc.)
    LifecycleGuard lifecycle;

    std::cout << "\n=== C++ Processor Pipeline Template ===\n\n";

    // 2. Start BrokerService
    std::string broker_ep;
    std::string broker_pubkey;
    std::mutex  ready_mu;
    std::condition_variable ready_cv;
    bool        ready = false;

    BrokerService::Config broker_cfg;
    broker_cfg.endpoint = "tcp://127.0.0.1:0"; // OS-assigned port
    broker_cfg.on_ready = [&](const std::string &ep, const std::string &pk)
    {
        {
            std::lock_guard lk(ready_mu);
            broker_ep     = ep;
            broker_pubkey = pk;
            ready = true;
        }
        ready_cv.notify_one();
    };

    BrokerService broker_svc(broker_cfg);
    std::thread broker_thread([&] { broker_svc.run(); });

    // Wait for broker to be ready.
    {
        std::unique_lock lk(ready_mu);
        ready_cv.wait(lk, [&] { return ready; });
    }
    std::cout << "Broker ready at " << broker_ep << "\n";

    // 3. Create Producer on "raw.data" channel
    Messenger prod_messenger;
    (void)prod_messenger.connect(broker_ep, broker_pubkey, "", "");

    ProducerOptions prod_opts;
    prod_opts.channel_name = "raw.data";
    prod_opts.pattern      = ChannelPattern::PubSub;
    prod_opts.has_shm      = true;
    prod_opts.shm_config.ring_buffer_capacity = 4;
    prod_opts.shm_config.policy               = DataBlockPolicy::RingBuffer;
    prod_opts.shm_config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;

    auto producer = Producer::create<EmptyFlexZone, RawReading>(prod_messenger, prod_opts);
    if (!producer)
    {
        std::cerr << "Failed to create producer\n";
        return 1;
    }
    std::cout << "Producer created on 'raw.data'\n";

    // 4. Create a second Producer for the output channel "scaled.data"
    Messenger out_messenger;
    (void)out_messenger.connect(broker_ep, broker_pubkey, "", "");

    ProducerOptions out_opts;
    out_opts.channel_name = "scaled.data";
    out_opts.pattern      = ChannelPattern::PubSub;
    out_opts.has_shm      = true;
    out_opts.shm_config.ring_buffer_capacity = 4;
    out_opts.shm_config.policy               = DataBlockPolicy::RingBuffer;
    out_opts.shm_config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;

    auto out_producer = Producer::create<EmptyFlexZone, ScaledResult>(out_messenger, out_opts);
    if (!out_producer)
    {
        std::cerr << "Failed to create output producer\n";
        return 1;
    }
    std::cout << "Output producer created on 'scaled.data'\n";

    // 5. Connect a Consumer to "raw.data" (for the processor input)
    Messenger in_messenger;
    (void)in_messenger.connect(broker_ep, broker_pubkey, "", "");

    ConsumerOptions in_opts;
    in_opts.channel_name = "raw.data";

    auto in_consumer = Consumer::connect<EmptyFlexZone, RawReading>(in_messenger, in_opts);
    if (!in_consumer)
    {
        std::cerr << "Failed to connect input consumer\n";
        return 1;
    }
    std::cout << "Input consumer connected to 'raw.data'\n";

    // 6. Create hub::Processor using the internal queues from Consumer/Producer.
    // ShmQueue is owned internally — no need to create externally.
    // NOTE: This example uses the template RAII path (create<F,D>/connect<F,D>)
    // which needs reworking for the new ownership model (see API_TODO.md).
    // For now, access the queue_reader/queue_writer from the internal queue.
    ProcessorOptions proc_opts;
    proc_opts.overflow_policy  = OverflowPolicy::Block;
    proc_opts.input_timeout    = 2s;
    proc_opts.zero_fill_output = true;

    // TODO: Processor::create needs QueueReader& + QueueWriter&. The Producer/Consumer
    // internal queues provide these, but the accessor API is not yet finalized.
    // This example needs a full rewrite once the template RAII path is resolved.
    auto processor = Processor::create(
        *static_cast<QueueReader*>(nullptr),  // placeholder — see API_TODO
        *static_cast<QueueWriter*>(nullptr),  // placeholder — see API_TODO
        proc_opts);
    if (!processor)
    {
        std::cerr << "Failed to create processor\n";
        return 1;
    }

    // 8. Install typed handler: RawReading → ScaledResult
    std::atomic<uint32_t> iteration{0};

    processor->set_process_handler<EmptyFlexZone, RawReading, EmptyFlexZone, ScaledResult>(
        [&](ProcessorContext<EmptyFlexZone, RawReading, EmptyFlexZone, ScaledResult> &ctx)
            -> bool
        {
            const uint32_t iter = iteration.fetch_add(1);
            ctx.output().scaled_value = ctx.input().value * 2.5;
            ctx.output().timestamp    = ctx.input().timestamp;
            ctx.output().iteration    = iter;
            return true; // commit
        });

    processor->start();
    std::cout << "Processor started (RawReading → ScaledResult, x2.5 scaling)\n";

    // 9. Connect a Consumer to "scaled.data" (final output reader)
    Messenger final_messenger;
    (void)final_messenger.connect(broker_ep, broker_pubkey, "", "");

    ConsumerOptions final_opts;
    final_opts.channel_name = "scaled.data";

    auto final_consumer =
        Consumer::connect<EmptyFlexZone, ScaledResult>(final_messenger, final_opts);
    if (!final_consumer)
    {
        std::cerr << "Failed to connect final consumer\n";
        return 1;
    }
    std::cout << "Final consumer connected to 'scaled.data'\n";

    // 10. Produce data and read processed results
    std::cout << "\n--- Producing 5 samples ---\n";

    for (int i = 1; i <= 5; ++i)
    {
        // Write a raw reading via synced_write.
        bool wrote = producer->synced_write<EmptyFlexZone, RawReading>(
            [&](WriteProcessorContext<EmptyFlexZone, RawReading> &ctx)
            {
                for (auto &&result : ctx.txn.slots(500ms))
                {
                    if (!result.is_ok())
                        break;
                    auto &d = result.content().get();
                    d.value     = static_cast<float>(i * 10);
                    d.timestamp = static_cast<uint64_t>(i);
                    ctx.txn.publish();
                    break; // one slot per iteration
                }
            });

        if (!wrote)
        {
            std::cerr << "  Producer write failed at i=" << i << "\n";
            continue;
        }

        // Give the processor a moment to process.
        std::this_thread::sleep_for(50ms);

        // Read the processed result via Consumer::pull with a callback.
        bool got_result = final_consumer->pull<EmptyFlexZone, ScaledResult>(
            [&](ReadProcessorContext<EmptyFlexZone, ScaledResult> &ctx)
            {
                for (auto &&result : ctx.txn.slots(500ms))
                {
                    if (!result.is_ok())
                        break;
                    const auto &d = result.content().get();
                    std::cout << "  Sample " << i
                              << ": raw=" << (i * 10)
                              << " -> scaled=" << d.scaled_value
                              << " (iter=" << d.iteration << ")\n";
                    break; // one slot per iteration
                }
            });

        if (!got_result)
            std::cout << "  Sample " << i << ": no result yet\n";
    }

    // 11. Shutdown in correct order
    std::cout << "\n--- Shutting down ---\n";

    processor->stop();
    std::cout << "  Processor stopped (iterations: " << processor->iteration_count() << ")\n";

    final_consumer->stop();
    final_consumer->close();

    in_consumer->stop();
    in_consumer->close();

    out_producer->stop();
    out_producer->close();

    producer->stop();
    producer->close();

    broker_svc.stop();
    broker_thread.join();

    std::cout << "  All components stopped.\n";
    std::cout << "\n=== Done ===\n";

    return 0;
}
