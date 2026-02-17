/**
 * @file datahub_producer_example.cpp
 * @brief Example: DataBlock producer using the RAII transaction API.
 *
 * Demonstrates create_datablock_producer<FlexZoneT, DataBlockT> and
 * producer->with_transaction for typed, schema-validated writes.
 */
#include "plh_datahub.hpp"

#include <chrono>
#include <cstring>
#include <iostream>

using namespace pylabhub::hub;
using namespace pylabhub::utils;
using namespace std::chrono_literals;

// ─── Application data types ──────────────────────────────────────────────────

/**
 * @brief Flexible zone: control/metadata shared between producer and consumers.
 *
 * Written by the producer on every transaction. Consumers poll this to detect
 * shutdown or configuration changes without acquiring a data slot.
 */
struct SensorFlexZone
{
    uint64_t frame_count{0};     ///< Incremented on every published slot.
    bool shutdown_flag{false};   ///< Set to true when producer is shutting down.
};
static_assert(std::is_trivially_copyable_v<SensorFlexZone>);

PYLABHUB_SCHEMA_BEGIN(SensorFlexZone)
    PYLABHUB_SCHEMA_MEMBER(frame_count)
    PYLABHUB_SCHEMA_MEMBER(shutdown_flag)
PYLABHUB_SCHEMA_END(SensorFlexZone)

/**
 * @brief Per-slot data payload.
 */
struct SensorData
{
    uint64_t timestamp_ns{0};
    float    temperature{0.0f};
    float    humidity{0.0f};
    uint32_t sequence_num{0};
};
static_assert(std::is_trivially_copyable_v<SensorData>);

PYLABHUB_SCHEMA_BEGIN(SensorData)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(temperature)
    PYLABHUB_SCHEMA_MEMBER(humidity)
    PYLABHUB_SCHEMA_MEMBER(sequence_num)
PYLABHUB_SCHEMA_END(SensorData)

// ─── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    // LifecycleGuard initializes Logger, CryptoUtils, and DataHub in topological order.
    LifecycleGuard lifecycle(MakeModDefList(
        Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        GetLifecycleModule()));

    MessageHub &hub = MessageHub::get_instance();

    // ─── Create producer ───────────────────────────────────────────────────
    DataBlockConfig config{};
    config.policy                = DataBlockPolicy::RingBuffer;
    config.consumer_sync_policy  = ConsumerSyncPolicy::Single_reader;
    config.shared_secret         = 0xBAD5ECRET;
    config.ring_buffer_capacity  = 4;
    config.physical_page_size    = DataBlockPageSize::Size4K;
    config.flex_zone_size        = 4096; // must be a multiple of 4096 (page size)
    config.checksum_policy       = ChecksumPolicy::Enforced;

    auto producer = create_datablock_producer<SensorFlexZone, SensorData>(
        hub, "sensor_data_channel", DataBlockPolicy::RingBuffer, config);
    if (!producer)
    {
        std::cerr << "Failed to create DataBlockProducer!\n";
        return 1;
    }
    std::cout << "DataBlockProducer ready.\n";

    // ─── Write 5 slots ─────────────────────────────────────────────────────
    for (uint32_t i = 0; i < 5; ++i)
    {
        producer->with_transaction<SensorFlexZone, SensorData>(
            1000ms,
            [i](WriteTransactionContext<SensorFlexZone, SensorData> &ctx)
            {
                // Update the flexible zone (control/metadata shared with consumers).
                auto zone = ctx.flexzone();
                zone.get().frame_count = i + 1;
                zone.get().shutdown_flag = (i == 4); // signal shutdown on last frame

                // Acquire a write slot and publish data.
                for (auto &result : ctx.slots(100ms))
                {
                    if (!result.is_ok())
                    {
                        std::cerr << "  Slot acquire timeout, retrying...\n";
                        break;
                    }

                    SensorData &data = result.content().get();
                    data.timestamp_ns  = pylabhub::platform::monotonic_time_ns();
                    data.temperature   = 20.0f + static_cast<float>(i) * 0.5f;
                    data.humidity      = 50.0f + static_cast<float>(i) * 1.0f;
                    data.sequence_num  = i;

                    // auto-publish fires when the loop exits normally (break).
                    break;
                }

                std::cout << "  Published slot " << i << " (frame_count=" << i + 1 << ")\n";
            });
    }

    std::cout << "DataBlockProducer finished.\n";
    return 0;
}
