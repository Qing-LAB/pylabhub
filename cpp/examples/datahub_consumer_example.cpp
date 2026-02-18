/**
 * @file datahub_consumer_example.cpp
 * @brief Example: DataBlock consumer using the RAII transaction API.
 *
 * Demonstrates find_datablock_consumer<FlexZoneT, DataBlockT> and
 * consumer->with_transaction for typed, schema-validated reads.
 *
 * Schema types (SensorFlexZone, SensorData) must match the producer.
 */
#include "plh_datahub.hpp"

#include <chrono>
#include <iostream>

using namespace pylabhub::hub;
using namespace pylabhub::utils;
using namespace std::chrono_literals;

// ─── Application data types (must match producer exactly) ────────────────────

struct SensorFlexZone
{
    uint64_t frame_count{0};
    bool shutdown_flag{false};
};
static_assert(std::is_trivially_copyable_v<SensorFlexZone>);

PYLABHUB_SCHEMA_BEGIN(SensorFlexZone)
    PYLABHUB_SCHEMA_MEMBER(frame_count)
    PYLABHUB_SCHEMA_MEMBER(shutdown_flag)
PYLABHUB_SCHEMA_END(SensorFlexZone)

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
    LifecycleGuard lifecycle(MakeModDefList(
        Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        GetLifecycleModule()));

    // ─── Attach consumer ───────────────────────────────────────────────────
    DataBlockConfig expected_config{};
    expected_config.policy               = DataBlockPolicy::RingBuffer;
    expected_config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
    expected_config.shared_secret        = 0xBAD5ECRET;
    expected_config.ring_buffer_capacity = 4;
    expected_config.physical_page_size   = DataBlockPageSize::Size4K;
    expected_config.flex_zone_size       = 4096; // must be a multiple of 4096 (page size)
    expected_config.checksum_policy      = ChecksumPolicy::Enforced;

    // Schema types are validated at attach time: mismatched types → nullptr.
    auto consumer = find_datablock_consumer<SensorFlexZone, SensorData>(
        "sensor_data_channel", expected_config.shared_secret, expected_config);
    if (!consumer)
    {
        std::cerr << "Failed to attach DataBlockConsumer (producer not running?)\n";
        return 1;
    }
    std::cout << "DataBlockConsumer attached.\n";

    // ─── Read until the producer signals shutdown ──────────────────────────
    consumer->with_transaction<SensorFlexZone, SensorData>(
        10000ms,  // outer timeout (total budget for this transaction session)
        [](ReadTransactionContext<SensorFlexZone, SensorData> &ctx)
        {
            for (auto &result : ctx.slots(200ms))
            {
                // Check control zone first — no slot acquisition needed.
                {
                    auto zone = ctx.flexzone();
                    if (zone.get().shutdown_flag)
                    {
                        std::cout << "  Shutdown flag set — stopping.\n";
                        break;
                    }
                }

                if (!result.is_ok())
                {
                    // Timeout waiting for a new slot; keep heartbeat alive and retry.
                    ctx.update_heartbeat();
                    continue;
                }

                const SensorData &data = result.content().get();
                std::cout << "  Slot " << data.sequence_num
                          << "  temp=" << data.temperature
                          << "  hum=" << data.humidity << "\n";
                // Slot is released automatically when result goes out of scope.
            }
        });

    std::cout << "DataBlockConsumer finished.\n";
    return 0;
}
