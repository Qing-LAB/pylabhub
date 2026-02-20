/**
 * @file raii_layer_example.cpp
 * @brief Example: RAII DataBlock — in-process producer and consumer threads.
 *
 * Demonstrates the low-level RAII DataBlock API without any ZMQ messaging.
 * Both the producer and consumer share the same DataBlock in the same process,
 * running in separate threads. This is the simplest way to try the SHM ring
 * buffer without needing a running broker.
 *
 * Key concepts shown:
 *  - Trivially-copyable FlexZone and slot data types (no atomics; sync is
 *    managed by the DataBlock framework).
 *  - WriteTransactionContext / ReadTransactionContext — RAII slot acquire/release.
 *  - FlexZone for out-of-band control metadata (shutdown flag, frame count).
 *  - Slot iterator pattern: `for (auto& result : ctx.slots(timeout)) { ... break; }`.
 *  - Consumer heartbeat update to keep the DataBlock alive.
 */
#include "plh_datahub.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::utils;
using namespace std::chrono_literals;

// ─── Shared types ─────────────────────────────────────────────────────────────

/**
 * @brief FlexZone: control metadata written by the producer, read by consumers.
 *
 * Must be trivially copyable — no std::atomic, std::mutex, or virtual members.
 * DataBlock's two-tier synchronization (DataBlockMutex + SharedSpinLock) ensures
 * that FlexZone reads and writes are safe without additional locking here.
 */
struct SensorFlexZone
{
    uint64_t frame_count{0};   ///< Total published frames so far.
    bool     shutdown{false};  ///< Producer sets to true when finished.
    uint32_t _pad{0};          ///< Explicit padding for alignment.
};
static_assert(std::is_trivially_copyable_v<SensorFlexZone>);

PYLABHUB_SCHEMA_BEGIN(SensorFlexZone)
    PYLABHUB_SCHEMA_MEMBER(frame_count)
    PYLABHUB_SCHEMA_MEMBER(shutdown)
PYLABHUB_SCHEMA_END(SensorFlexZone)

/**
 * @brief Per-slot sensor reading.
 */
struct SensorReading
{
    uint64_t timestamp_ns{0};
    float    temperature{0.0f};
    float    humidity{0.0f};
    uint32_t sequence_num{0};
};
static_assert(std::is_trivially_copyable_v<SensorReading>);

PYLABHUB_SCHEMA_BEGIN(SensorReading)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(temperature)
    PYLABHUB_SCHEMA_MEMBER(humidity)
    PYLABHUB_SCHEMA_MEMBER(sequence_num)
PYLABHUB_SCHEMA_END(SensorReading)

// ─── Configuration ────────────────────────────────────────────────────────────

constexpr uint64_t kSharedSecret  = 0xABCD1234DEAD'BEEF;
constexpr uint32_t kSlotCount     = 8;
constexpr uint32_t kFrames        = 20;
constexpr uint32_t kWritePeriodMs = 30; // ~33 Hz

// ─── Producer thread ──────────────────────────────────────────────────────────

void run_producer(const std::string& channel_name)
{
    DataBlockConfig cfg{};
    cfg.policy               = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
    cfg.shared_secret        = kSharedSecret;
    cfg.ring_buffer_capacity = kSlotCount;
    cfg.physical_page_size   = DataBlockPageSize::Size4K;
    cfg.flex_zone_size       = 4096; // page-aligned; must be >= sizeof(SensorFlexZone)
    cfg.checksum_policy      = ChecksumPolicy::Enforced;

    auto producer = create_datablock_producer<SensorFlexZone, SensorReading>(
        channel_name, DataBlockPolicy::RingBuffer, cfg);
    if (!producer)
    {
        std::cerr << "[producer] Failed to create DataBlockProducer\n";
        return;
    }
    std::cout << "[producer] Ready — publishing " << kFrames << " frames\n";

    for (uint32_t i = 0; i < kFrames; ++i)
    {
        producer->with_transaction<SensorFlexZone, SensorReading>(
            500ms,
            [i](WriteTransactionContext<SensorFlexZone, SensorReading>& ctx)
            {
                // Update the FlexZone (visible to consumer without a slot acquire).
                auto fz = ctx.flexzone();
                fz.get().frame_count = i + 1;
                fz.get().shutdown    = (i + 1 == kFrames);

                // Acquire a write slot and fill it.
                for (auto& slot : ctx.slots(200ms))
                {
                    if (!slot.is_ok())
                    {
                        std::cerr << "[producer] Slot acquire timeout at frame " << i << "\n";
                        break;
                    }
                    SensorReading& data = slot.content().get();
                    data.timestamp_ns   = pylabhub::platform::monotonic_time_ns();
                    data.temperature    = 20.0f + static_cast<float>(i) * 0.3f;
                    data.humidity       = 50.0f + static_cast<float>(i % 10) * 0.5f;
                    data.sequence_num   = i;
                    break; // slot committed on loop exit
                }
            });

        std::cout << "[producer] Published frame " << i << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(kWritePeriodMs));
    }

    std::cout << "[producer] Done\n";
}

// ─── Consumer thread ──────────────────────────────────────────────────────────

void run_consumer(const std::string& channel_name)
{
    // Give the producer a moment to create the DataBlock.
    std::this_thread::sleep_for(50ms);

    DataBlockConfig expected{};
    expected.policy               = DataBlockPolicy::RingBuffer;
    expected.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
    expected.shared_secret        = kSharedSecret;
    expected.ring_buffer_capacity = kSlotCount;
    expected.physical_page_size   = DataBlockPageSize::Size4K;
    expected.flex_zone_size       = 4096;
    expected.checksum_policy      = ChecksumPolicy::Enforced;

    // Schema types are validated at attach time: mismatched types → nullptr.
    auto consumer = find_datablock_consumer<SensorFlexZone, SensorReading>(
        channel_name, kSharedSecret, expected);
    if (!consumer)
    {
        std::cerr << "[consumer] Failed to attach DataBlockConsumer\n";
        return;
    }
    std::cout << "[consumer] Attached\n";

    // Read until the producer sets the shutdown flag in the FlexZone.
    consumer->with_transaction<SensorFlexZone, SensorReading>(
        5000ms, // total time budget for this read session
        [](ReadTransactionContext<SensorFlexZone, SensorReading>& ctx)
        {
            for (auto& slot : ctx.slots(200ms))
            {
                // Check FlexZone between slot acquires — no slot hold required.
                {
                    auto fz = ctx.flexzone();
                    if (fz.get().shutdown)
                    {
                        std::cout << "[consumer] Shutdown flag set — stopping\n";
                        break;
                    }
                }

                if (!slot.is_ok())
                {
                    // Timeout; bump heartbeat and retry.
                    ctx.update_heartbeat();
                    continue;
                }

                const SensorReading& data = slot.content().get();
                std::cout << "[consumer] seq=" << data.sequence_num
                          << "  temp=" << data.temperature
                          << "  hum=" << data.humidity << "\n";
                // Slot released automatically when `slot` goes out of scope.
            }
        });

    std::cout << "[consumer] Done\n";
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    // LifecycleGuard initialises Logger, CryptoUtils, and the DataExchangeHub in
    // dependency order; destructor tears them down in reverse order.
    LifecycleGuard lifecycle(MakeModDefList(
        Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        GetLifecycleModule())); // pylabhub::hub::GetLifecycleModule()

    const std::string channel = "raii_example_sensors";

    std::thread producer_thread(run_producer, channel);
    std::thread consumer_thread(run_consumer, channel);

    producer_thread.join();
    consumer_thread.join();

    std::cout << "Example complete\n";
    return 0;
}
