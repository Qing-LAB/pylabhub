// tests/test_layer3_datahub/workers/datahub_stress_raii_workers.cpp
//
// RAII layer multi-process stress tests — implementation.
//
// Design rationale:
//  - Uses DataBlockProducer/Consumer directly (no Messenger/Broker) to stress the RAII
//    and ring-buffer layers in isolation.
//  - StressSlotData fills an entire 4096-byte logical unit (= physical page size) so that
//    every read/write exercises the full memory boundary of each slot.
//  - BLAKE2b checksums are verified automatically by release_consume_slot() (Enforced
//    policy); the app_checksum field provides an independent XOR-fold verification layer.
//  - Random inter-operation delays mimic real-world scheduling jitter and expose races
//    that only appear when producer and consumer run at different speeds.
//
// Thread safety:
//  - Each sub-worker process is single-threaded; no shared-state concerns within a process.
//  - The DataBlock shared-memory segment is the sole cross-process channel.

#include "datahub_stress_raii_workers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

// ============================================================================
// Shared types and constants (file scope — PYLABHUB_SCHEMA macros require it)
// ============================================================================

// --- Stress test parameters -------------------------------------------------

/// Number of slots written by the full-capacity producer (Latest_only scenario).
static constexpr int kNumSlots = 500;

/// Ring buffer capacity for the full-capacity scenario.
static constexpr int kRingCapacity = 32; // 32 × 4KB = 128 KB ring; ≈15 wraparounds

/// Number of slots written by the back-pressure producer (Single_reader scenario).
static constexpr int kNumSlotsBP = 100;

/// Ring buffer capacity for the back-pressure scenario (forces frequent blocking).
static constexpr int kRingCapacityBP = 8;

/// Flex zone allocation (page-aligned; must fit StressFlexZone).
static constexpr int kFlexZoneSize = 4096;

/// Shared secret for DataBlock discovery.
static constexpr uint64_t kStressSecret = 0xDEADBEEFCAFEBABEULL;

// --- Shared data structures -------------------------------------------------

/**
 * @brief Flexible zone (shared metadata).
 * @note Must be trivially copyable; no std::atomic members.
 */
struct StressFlexZone
{
    uint64_t producer_pid{0}; ///< PID of the producer process (informational).
};
static_assert(std::is_trivially_copyable_v<StressFlexZone>);

/**
 * @brief Slot payload — exactly 4096 bytes (fills one physical page).
 *
 * The full 4KB payload ensures every slot write/read exercises the complete
 * memory boundary, catching any off-by-one or alignment issues in the ring buffer.
 *
 * Layout:
 *  [0..7]   sequence   — monotonically increasing slot index
 *  [8..11]  app_checksum — XOR-fold of all payload bytes (independent of BLAKE2b)
 *  [12..4095] payload  — deterministic pattern seeded by sequence
 */
struct StressSlotData
{
    uint64_t sequence{0};       ///< Slot index [0..kNumSlots-1 or kNumSlotsBP-1].
    uint32_t app_checksum{0};   ///< XOR-fold of payload[]; independent BLAKE2b check.
    uint8_t  payload[4084]{};   ///< 8 + 4 + 4084 = 4096 bytes.
};
static_assert(sizeof(StressSlotData) == 4096,
              "StressSlotData must be exactly 4096 bytes — one physical page per slot");
static_assert(std::is_trivially_copyable_v<StressSlotData>);

// BLDS schemas — needed by create_datablock_producer / find_datablock_consumer
// for dual-schema hash validation.
PYLABHUB_SCHEMA_BEGIN(StressFlexZone)
    PYLABHUB_SCHEMA_MEMBER(producer_pid)
PYLABHUB_SCHEMA_END(StressFlexZone)

PYLABHUB_SCHEMA_BEGIN(StressSlotData)
    PYLABHUB_SCHEMA_MEMBER(sequence)
    PYLABHUB_SCHEMA_MEMBER(app_checksum)
    PYLABHUB_SCHEMA_MEMBER(payload)
PYLABHUB_SCHEMA_END(StressSlotData)

namespace pylabhub::tests::worker::stress_raii
{

// --- Lifecycle module helpers -----------------------------------------------

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Internal utilities
// ============================================================================

namespace
{

/// Returns a DataBlockConfig for the Latest_only (racing consumers) scenario.
DataBlockConfig make_latest_only_config()
{
    DataBlockConfig cfg{};
    cfg.physical_page_size     = DataBlockPageSize::Size4K;
    cfg.logical_unit_size      = 4096;
    cfg.ring_buffer_capacity   = static_cast<uint32_t>(kRingCapacity);
    cfg.policy                 = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy   = ConsumerSyncPolicy::Latest_only;
    cfg.checksum_policy        = ChecksumPolicy::Enforced;
    cfg.flex_zone_size         = static_cast<size_t>(kFlexZoneSize);
    cfg.shared_secret          = kStressSecret;
    return cfg;
}

/// Returns a DataBlockConfig for the Single_reader (back-pressure) scenario.
DataBlockConfig make_backpressure_config()
{
    DataBlockConfig cfg{};
    cfg.physical_page_size     = DataBlockPageSize::Size4K;
    cfg.logical_unit_size      = 4096;
    cfg.ring_buffer_capacity   = static_cast<uint32_t>(kRingCapacityBP);
    cfg.policy                 = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy   = ConsumerSyncPolicy::Single_reader;
    cfg.checksum_policy        = ChecksumPolicy::Enforced;
    cfg.flex_zone_size         = static_cast<size_t>(kFlexZoneSize);
    cfg.shared_secret          = kStressSecret;
    return cfg;
}

/// Fills buf[0..len-1] with a deterministic pattern seeded by seq.
void fill_payload(uint8_t *buf, size_t len, uint64_t seq)
{
    for (size_t i = 0; i < len; ++i)
    {
        // Knuth multiplicative hash with index mixing.
        buf[i] = static_cast<uint8_t>((seq * 2654435761ULL + i * 1000003ULL) >> 16);
    }
}

/// XOR-folds buf[0..len-1] into a 32-bit checksum (byte-position-sensitive).
uint32_t compute_app_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum ^= static_cast<uint32_t>(buf[i]) << (8 * (i % 4));
    return sum;
}

/// Returns true iff every byte in buf matches the pattern for the given sequence.
bool verify_payload_bytes(const uint8_t *buf, size_t len, uint64_t seq)
{
    for (size_t i = 0; i < len; ++i)
    {
        uint8_t expected = static_cast<uint8_t>((seq * 2654435761ULL + i * 1000003ULL) >> 16);
        if (buf[i] != expected)
            return false;
    }
    return true;
}

/// Sleeps for a uniformly random duration in [0, max_ms] milliseconds.
/// Thread-local RNG — no external synchronization required.
void random_sleep(int max_ms)
{
    if (max_ms <= 0)
        return;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, max_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(dist(rng)));
}

} // anonymous namespace

// ============================================================================
// stress_producer — Latest_only scenario
// ============================================================================

int stress_producer(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: stress_producer requires argv[2]: channel_name\n");
        return 1;
    }
    const std::string channel = argv[2];

    return run_gtest_worker(
        [&channel]()
        {
            auto cfg      = make_latest_only_config();
            auto producer = create_datablock_producer<StressFlexZone, StressSlotData>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr) << "stress_producer: failed to create DataBlock";

            // Record our PID in the flex zone so consumers can identify us.
            producer->with_transaction<StressFlexZone, StressSlotData>(
                200ms,
                [](WriteTransactionContext<StressFlexZone, StressSlotData> &ctx)
                {
                    auto zone            = ctx.flexzone();
                    zone.get().producer_pid = static_cast<uint64_t>(getpid());
                    // No data slot written — flexzone-only update.
                });

            // Signal ready so consumers can attach before we start writing.
            signal_test_ready();

            // Write kNumSlots full-4KB slots with random inter-write delays.
            for (int seq = 0; seq < kNumSlots; ++seq)
            {
                bool written  = false;
                int  retries  = 0;

                while (!written && retries < 500)
                {
                    producer->with_transaction<StressFlexZone, StressSlotData>(
                        200ms,
                        [&](WriteTransactionContext<StressFlexZone, StressSlotData> &ctx)
                        {
                            for (auto &result : ctx.slots(50ms))
                            {
                                if (!result.is_ok())
                                {
                                    ++retries;
                                    break;
                                }
                                auto &data     = result.content().get();
                                data.sequence  = static_cast<uint64_t>(seq);
                                fill_payload(data.payload, sizeof(data.payload),
                                             data.sequence);
                                data.app_checksum = compute_app_checksum(
                                    data.payload, sizeof(data.payload));
                                written = true;
                                break;
                            }
                        });

                    // Random delay 0–5 ms between write attempts.
                    random_sleep(5);
                }

                ASSERT_TRUE(written)
                    << "stress_producer: failed to write slot " << seq
                    << " after " << retries << " retries";
            }

            // Keep DataBlock alive briefly so consumers can see the final slot.
            std::this_thread::sleep_for(500ms);

            fmt::print(stderr, "[stress_producer] wrote {} slots (ring={} wraps≈{})\n",
                       kNumSlots, kRingCapacity, kNumSlots / kRingCapacity);
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "stress_raii.stress_producer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// stress_consumer — Latest_only scenario
// ============================================================================

int stress_consumer(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: stress_consumer requires argv[2]: channel_name\n");
        return 1;
    }
    const std::string channel      = argv[2];
    const int         consumer_idx = (argc > 3) ? std::stoi(argv[3]) : 0;

    return run_gtest_worker(
        [&channel, consumer_idx]()
        {
            auto cfg = make_latest_only_config();

            // Retry attach — producer signals ready before writing, but the DataBlock
            // mmap may not be visible immediately.
            std::unique_ptr<DataBlockConsumer> consumer;
            for (int attempt = 0; attempt < 50 && !consumer; ++attempt)
            {
                consumer = find_datablock_consumer<StressFlexZone, StressSlotData>(
                    channel, kStressSecret, cfg);
                if (!consumer)
                    std::this_thread::sleep_for(100ms);
            }
            ASSERT_NE(consumer, nullptr)
                << "stress_consumer[" << consumer_idx << "]: failed to attach to DataBlock";

            uint64_t last_seq      = std::numeric_limits<uint64_t>::max(); // sentinel: none yet
            bool     have_first    = false;
            uint64_t reads         = 0;
            uint64_t pattern_errors = 0;
            int      timeouts      = 0;
            bool     done          = false;

            // Read until we observe the terminal sequence or exhaust the timeout budget.
            while (!done && timeouts < 200)
            {
                consumer->with_transaction<StressFlexZone, StressSlotData>(
                    100ms,
                    [&](ReadTransactionContext<StressFlexZone, StressSlotData> &ctx)
                    {
                        for (auto &result : ctx.slots(50ms))
                        {
                            if (!result.is_ok())
                            {
                                ++timeouts;
                                break;
                            }

                            const auto &data = result.content().get();

                            // Sequence must be non-decreasing (Latest_only guarantees
                            // we never go backward; we may skip forward).
                            if (have_first)
                            {
                                EXPECT_GE(data.sequence, last_seq)
                                    << "stress_consumer[" << consumer_idx
                                    << "]: sequence went backward";
                            }

                            // Independent app-level checksum check.
                            uint32_t expected_cs = compute_app_checksum(
                                data.payload, sizeof(data.payload));
                            if (expected_cs != data.app_checksum)
                            {
                                ++pattern_errors;
                                ADD_FAILURE()
                                    << "stress_consumer[" << consumer_idx
                                    << "]: app_checksum mismatch at seq=" << data.sequence
                                    << " (expected=" << expected_cs
                                    << " got=" << data.app_checksum << ")";
                            }

                            // Full byte-level pattern verification.
                            if (!verify_payload_bytes(data.payload, sizeof(data.payload),
                                                      data.sequence))
                            {
                                ++pattern_errors;
                                ADD_FAILURE()
                                    << "stress_consumer[" << consumer_idx
                                    << "]: byte-pattern mismatch at seq=" << data.sequence;
                            }

                            last_seq   = data.sequence;
                            have_first = true;
                            ++reads;
                            timeouts = 0; // reset on successful read

                            if (last_seq >= static_cast<uint64_t>(kNumSlots - 1))
                                done = true;

                            break;
                        }
                    });

                // Random inter-read delay: 0–10 ms.
                random_sleep(10);
            }

            EXPECT_LT(timeouts, 200)
                << "stress_consumer[" << consumer_idx << "]: timed out before seeing terminal slot";
            EXPECT_GE(reads, 1u)
                << "stress_consumer[" << consumer_idx << "]: never read any slot";
            EXPECT_EQ(pattern_errors, 0u)
                << "stress_consumer[" << consumer_idx << "]: payload pattern errors detected";

            // BLAKE2b checksum failures are tracked internally by release_consume_slot().
            DataBlockMetrics metrics{};
            if (consumer->get_metrics(metrics) == 0)
            {
                EXPECT_EQ(metrics.checksum_failures, 0u)
                    << "stress_consumer[" << consumer_idx
                    << "]: BLAKE2b checksum failures detected in metrics";
            }

            fmt::print(stderr,
                       "[stress_consumer{}] reads={} last_seq={} pattern_errors={}\n",
                       consumer_idx, reads, last_seq, pattern_errors);
        },
        "stress_raii.stress_consumer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// multi_process_stress_orchestrator — Latest_only scenario
// ============================================================================

int multi_process_stress_orchestrator(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr,
                   "ERROR: multi_process_stress_orchestrator requires argv[2]: channel_name\n");
        return 1;
    }
    const std::string channel = argv[2];

    return run_gtest_worker(
        [&channel]()
        {
            // Spawn producer with ready-signal: it creates the DataBlock then signals.
            WorkerProcess producer(
                g_self_exe_path,
                "stress_raii.stress_producer",
                {channel},
                /*redirect_stderr_to_console=*/false,
                /*with_ready_signal=*/true);

            // Block until DataBlock exists and is ready for consumers to attach.
            producer.wait_for_ready();

            // Launch two concurrent consumers (Latest_only — they race the ring).
            WorkerProcess consumer0(
                g_self_exe_path, "stress_raii.stress_consumer", {channel, "0"},
                false, false);
            WorkerProcess consumer1(
                g_self_exe_path, "stress_raii.stress_consumer", {channel, "1"},
                false, false);

            consumer0.wait_for_exit();
            expect_worker_ok(consumer0);

            consumer1.wait_for_exit();
            expect_worker_ok(consumer1);

            producer.wait_for_exit();
            expect_worker_ok(producer);
        },
        "stress_raii.multi_process_stress_orchestrator",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// backpressure_producer — Single_reader scenario
// ============================================================================

int backpressure_producer(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: backpressure_producer requires argv[2]: channel_name\n");
        return 1;
    }
    const std::string channel = argv[2];

    return run_gtest_worker(
        [&channel]()
        {
            auto cfg      = make_backpressure_config();
            auto producer = create_datablock_producer<StressFlexZone, StressSlotData>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr)
                << "backpressure_producer: failed to create DataBlock";

            // Signal ready before writing so the consumer can attach and start reading;
            // with Single_reader the consumer MUST be ready before we fill the ring.
            signal_test_ready();

            for (int seq = 0; seq < kNumSlotsBP; ++seq)
            {
                bool written = false;
                int  retries = 0;

                while (!written && retries < 1000)
                {
                    producer->with_transaction<StressFlexZone, StressSlotData>(
                        500ms, // generous timeout: consumer may be delayed 20 ms
                        [&](WriteTransactionContext<StressFlexZone, StressSlotData> &ctx)
                        {
                            for (auto &result : ctx.slots(200ms))
                            {
                                if (!result.is_ok())
                                {
                                    ++retries;
                                    break;
                                }
                                auto &data        = result.content().get();
                                data.sequence     = static_cast<uint64_t>(seq);
                                fill_payload(data.payload, sizeof(data.payload),
                                             data.sequence);
                                data.app_checksum = compute_app_checksum(
                                    data.payload, sizeof(data.payload));
                                written = true;
                                break;
                            }
                        });

                    // Random delay 0–5 ms.
                    random_sleep(5);
                }

                ASSERT_TRUE(written)
                    << "backpressure_producer: failed to write slot " << seq
                    << " after " << retries << " retries";
            }

            // Keep DataBlock alive until the consumer has read all slots.
            std::this_thread::sleep_for(3000ms);

            fmt::print(stderr, "[backpressure_producer] wrote {} slots (ring={})\n",
                       kNumSlotsBP, kRingCapacityBP);
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "stress_raii.backpressure_producer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// backpressure_consumer — Single_reader scenario
// ============================================================================

int backpressure_consumer(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: backpressure_consumer requires argv[2]: channel_name\n");
        return 1;
    }
    const std::string channel = argv[2];

    return run_gtest_worker(
        [&channel]()
        {
            auto cfg = make_backpressure_config();

            std::unique_ptr<DataBlockConsumer> consumer;
            for (int attempt = 0; attempt < 50 && !consumer; ++attempt)
            {
                consumer = find_datablock_consumer<StressFlexZone, StressSlotData>(
                    channel, kStressSecret, cfg);
                if (!consumer)
                    std::this_thread::sleep_for(100ms);
            }
            ASSERT_NE(consumer, nullptr)
                << "backpressure_consumer: failed to attach to DataBlock";

            uint64_t expected_seq  = 0;
            uint64_t pattern_errors = 0;
            int      timeouts      = 0;

            // With Single_reader, every slot is delivered; we expect exactly kNumSlotsBP
            // slots in ascending order with no gaps.
            while (expected_seq < static_cast<uint64_t>(kNumSlotsBP) && timeouts < 500)
            {
                bool got_slot = false;

                consumer->with_transaction<StressFlexZone, StressSlotData>(
                    500ms, // generous: producer may be delayed
                    [&](ReadTransactionContext<StressFlexZone, StressSlotData> &ctx)
                    {
                        for (auto &result : ctx.slots(200ms))
                        {
                            if (!result.is_ok())
                            {
                                ++timeouts;
                                break;
                            }

                            const auto &data = result.content().get();

                            // With Single_reader, slots arrive in exact order.
                            EXPECT_EQ(data.sequence, expected_seq)
                                << "backpressure_consumer: unexpected sequence"
                                << " (expected=" << expected_seq
                                << " got=" << data.sequence << ")";

                            // Independent app checksum.
                            uint32_t expected_cs = compute_app_checksum(
                                data.payload, sizeof(data.payload));
                            if (expected_cs != data.app_checksum)
                            {
                                ++pattern_errors;
                                ADD_FAILURE()
                                    << "backpressure_consumer: app_checksum mismatch"
                                    << " at seq=" << data.sequence;
                            }

                            // Byte-level pattern.
                            if (!verify_payload_bytes(data.payload, sizeof(data.payload),
                                                      data.sequence))
                            {
                                ++pattern_errors;
                                ADD_FAILURE()
                                    << "backpressure_consumer: byte-pattern mismatch"
                                    << " at seq=" << data.sequence;
                            }

                            got_slot = true;
                            ++expected_seq;
                            timeouts = 0;
                            break;
                        }
                    });

                if (got_slot)
                {
                    // Random delay 0–20 ms: forces producer to block when ring is full.
                    random_sleep(20);
                }
            }

            EXPECT_EQ(expected_seq, static_cast<uint64_t>(kNumSlotsBP))
                << "backpressure_consumer: did not receive all " << kNumSlotsBP << " slots"
                << " (received=" << expected_seq << " timeouts=" << timeouts << ")";
            EXPECT_EQ(pattern_errors, 0u)
                << "backpressure_consumer: payload pattern errors detected";

            DataBlockMetrics metrics{};
            if (consumer->get_metrics(metrics) == 0)
            {
                EXPECT_EQ(metrics.checksum_failures, 0u)
                    << "backpressure_consumer: BLAKE2b checksum failures in metrics";
            }

            fmt::print(stderr,
                       "[backpressure_consumer] received {} / {} slots, "
                       "pattern_errors={}\n",
                       expected_seq, kNumSlotsBP, pattern_errors);
        },
        "stress_raii.backpressure_consumer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// backpressure_orchestrator — Single_reader scenario
// ============================================================================

int backpressure_orchestrator(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr,
                   "ERROR: backpressure_orchestrator requires argv[2]: channel_name\n");
        return 1;
    }
    const std::string channel = argv[2];

    return run_gtest_worker(
        [&channel]()
        {
            // Producer signals ready after DataBlock creation; consumer must attach
            // BEFORE the ring fills (otherwise producer blocks indefinitely).
            WorkerProcess producer(
                g_self_exe_path,
                "stress_raii.backpressure_producer",
                {channel},
                false, /*with_ready_signal=*/true);

            producer.wait_for_ready();

            WorkerProcess consumer(
                g_self_exe_path,
                "stress_raii.backpressure_consumer",
                {channel},
                false, false);

            consumer.wait_for_exit();
            expect_worker_ok(consumer);

            producer.wait_for_exit();
            expect_worker_ok(producer);
        },
        "stress_raii.backpressure_orchestrator",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::stress_raii

// ============================================================================
// Worker dispatcher registrar (static-init at link time)
// ============================================================================

namespace
{

struct StressRaiiWorkerRegistrar
{
    StressRaiiWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto             dot  = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "stress_raii")
                    return -1;
                std::string scenario(mode.substr(dot + 1));

                using namespace pylabhub::tests::worker::stress_raii;
                if (scenario == "multi_process_stress_orchestrator")
                    return multi_process_stress_orchestrator(argc, argv);
                if (scenario == "stress_producer")
                    return stress_producer(argc, argv);
                if (scenario == "stress_consumer")
                    return stress_consumer(argc, argv);
                if (scenario == "backpressure_orchestrator")
                    return backpressure_orchestrator(argc, argv);
                if (scenario == "backpressure_producer")
                    return backpressure_producer(argc, argv);
                if (scenario == "backpressure_consumer")
                    return backpressure_consumer(argc, argv);

                fmt::print(stderr, "ERROR: Unknown stress_raii scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static StressRaiiWorkerRegistrar kStressRaiiRegistrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
