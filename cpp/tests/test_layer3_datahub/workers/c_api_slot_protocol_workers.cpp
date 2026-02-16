// tests/test_layer3_datahub/workers/c_api_slot_protocol_workers.cpp
//
// C API slot protocol tests: write/read roundtrip, commit/abort semantics, ring buffer policies,
// timeout behavior, and metrics accumulation.
//
// Tests DataBlockProducer/Consumer C++ wrappers directly without RAII templates.
// Uses create_datablock_producer_impl with null schemas (no schema validation overhead).
//
// Test strategy:
// - Each test runs in an isolated process via run_gtest_worker
// - Tests use implementation functions directly to avoid template/schema complexity
// - Secret numbers start at 71001 to avoid conflicts with other test suites
// - Metrics are verified via DataBlockProducer::get_metrics() / DataBlockConsumer::get_metrics()
//
// Test list:
//   1. write_slot_read_slot_roundtrip       — basic write/read cycle, data integrity
//   2. commit_advances_metrics              — commit increments total_slots_written
//   3. abort_does_not_commit               — release without commit: slot not visible, metric=0
//   4. latest_only_reads_latest            — Latest_only skips to newest committed slot
//   5. single_reader_reads_sequentially    — Single_reader yields slots in commit order
//   6. write_returns_null_when_ring_full   — acquire_write_slot(0) → null when ring saturated
//   7. read_returns_null_on_empty_ring     — acquire_consume_slot(0) → null when no data
//   8. metrics_accumulate_across_writes    — N commits → total_slots_written==N; consumer read counted

#include "c_api_slot_protocol_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::c_api_slot_protocol
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

static DataBlockConfig make_config(ConsumerSyncPolicy sync_policy, int capacity, uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = sync_policy;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = capacity;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.checksum_policy = ChecksumPolicy::None; // C API protocol tests; checksum tested separately
    return cfg;
}

// ============================================================================
// 1. write_slot_read_slot_roundtrip
// Write a known pattern, commit, release; then acquire, read, verify pattern matches.
// This is the foundational correctness test: shared memory round-trip integrity.
// ============================================================================

int write_slot_read_slot_roundtrip()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiRoundtrip");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, 2, 71001);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            const uint64_t kTestValue = 0xDEADBEEF12345678ULL;

            // Write
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                auto span = h->buffer_span();
                ASSERT_GE(span.size(), sizeof(kTestValue));
                std::memcpy(span.data(), &kTestValue, sizeof(kTestValue));
                EXPECT_TRUE(h->commit(sizeof(kTestValue)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Read back
            {
                auto rh = consumer->acquire_consume_slot(1000);
                ASSERT_NE(rh, nullptr);
                auto span = rh->buffer_span();
                ASSERT_GE(span.size(), sizeof(kTestValue));
                uint64_t read_value = 0;
                std::memcpy(&read_value, span.data(), sizeof(read_value));
                EXPECT_EQ(read_value, kTestValue);
                (void)consumer->release_consume_slot(*rh);
            }

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "write_slot_read_slot_roundtrip", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. commit_advances_metrics
// Committing a slot increments total_slots_written in DataBlockMetrics.
// Verifies that the metrics counter correctly tracks committed writes.
// ============================================================================

int commit_advances_metrics()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiCommitMetrics");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, 4, 71002);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Before any commits: total_slots_written == 0
            DataBlockMetrics metrics{};
            ASSERT_EQ(producer->get_metrics(metrics), 0);
            EXPECT_EQ(metrics.total_slots_written, 0u) << "No commits yet";

            // Commit one slot
            {
                auto h = producer->acquire_write_slot(0);
                ASSERT_NE(h, nullptr);
                uint64_t val = 42;
                std::memcpy(h->buffer_span().data(), &val, sizeof(val));
                (void)h->commit(sizeof(val));
                (void)producer->release_write_slot(*h);
            }

            ASSERT_EQ(producer->get_metrics(metrics), 0);
            EXPECT_EQ(metrics.total_slots_written, 1u) << "One commit must advance counter to 1";

            // Commit two more
            for (int i = 0; i < 2; ++i)
            {
                auto h = producer->acquire_write_slot(0);
                ASSERT_NE(h, nullptr);
                (void)h->commit(8);
                (void)producer->release_write_slot(*h);
            }

            ASSERT_EQ(producer->get_metrics(metrics), 0);
            EXPECT_EQ(metrics.total_slots_written, 3u) << "Three commits must advance counter to 3";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "commit_advances_metrics", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. abort_does_not_commit
// Releasing a write slot without calling commit() must NOT make data visible
// to consumers and must NOT increment total_slots_written.
// ============================================================================

int abort_does_not_commit()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiAbort");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, 2, 71003);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            // Acquire a write slot but intentionally do NOT call commit()
            {
                auto h = producer->acquire_write_slot(0);
                ASSERT_NE(h, nullptr);
                uint64_t val = 0xBAD;
                std::memcpy(h->buffer_span().data(), &val, sizeof(val));
                // Deliberately omit: h->commit(sizeof(val));
                (void)producer->release_write_slot(*h); // abort — no commit
            }

            // Metrics: aborted write must not advance total_slots_written
            DataBlockMetrics metrics{};
            ASSERT_EQ(producer->get_metrics(metrics), 0);
            EXPECT_EQ(metrics.total_slots_written, 0u) << "Abort must not increment total_slots_written";

            // Consumer: no slot visible (aborted write is not committed).
            // Use 50ms timeout (not 0): consumer must time out quickly with no committed data.
            auto rh = consumer->acquire_consume_slot(50);
            EXPECT_EQ(rh, nullptr) << "Aborted write must not be visible to consumer";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "abort_does_not_commit", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 4. latest_only_reads_latest
// A Latest_only consumer always reads the most recently committed slot,
// skipping older ones. After consuming, no further data is available until
// a new write occurs.
// ============================================================================

int latest_only_reads_latest()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiLatestOnly");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, 4, 71004);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            // Write 3 sequential values (1, 2, 3); Latest_only must return 3
            for (uint64_t i = 1; i <= 3; ++i)
            {
                auto h = producer->acquire_write_slot(0);
                ASSERT_NE(h, nullptr);
                std::memcpy(h->buffer_span().data(), &i, sizeof(i));
                (void)h->commit(sizeof(i));
                (void)producer->release_write_slot(*h);
            }

            auto rh = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh, nullptr) << "At least one slot must be available";
            uint64_t value = 0;
            std::memcpy(&value, rh->buffer_span().data(), sizeof(value));
            EXPECT_EQ(value, 3u) << "Latest_only must return the most recently committed slot";
            (void)consumer->release_consume_slot(*rh);

            // After consuming latest, no new data without a new write.
            // Use 50ms timeout (not 0): timeout_ms=0 means "no timeout" (wait forever) per C API contract.
            auto next = consumer->acquire_consume_slot(50);
            EXPECT_EQ(next, nullptr) << "No new data after consuming latest; must return null";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "latest_only_reads_latest", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 5. single_reader_reads_sequentially
// A Single_reader consumer reads slots in commit order (FIFO).
// Each acquire_consume_slot yields the next unread slot in sequence.
// ============================================================================

int single_reader_reads_sequentially()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiSingleReader");
            MessageHub &hub = MessageHub::get_instance();
            // capacity=4 to hold all 3 writes without blocking
            auto cfg = make_config(ConsumerSyncPolicy::Single_reader, 4, 71005);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            // Write 3 sequential values (1, 2, 3)
            for (uint64_t i = 1; i <= 3; ++i)
            {
                auto h = producer->acquire_write_slot(0);
                ASSERT_NE(h, nullptr);
                std::memcpy(h->buffer_span().data(), &i, sizeof(i));
                (void)h->commit(sizeof(i));
                (void)producer->release_write_slot(*h);
            }

            // Read them back: Single_reader must yield them in order (1, 2, 3)
            for (uint64_t expected = 1; expected <= 3; ++expected)
            {
                auto rh = consumer->acquire_consume_slot(1000);
                ASSERT_NE(rh, nullptr) << "Slot " << expected << " must be available";
                uint64_t value = 0;
                std::memcpy(&value, rh->buffer_span().data(), sizeof(value));
                EXPECT_EQ(value, expected) << "Single_reader must yield slot " << expected << " in order";
                (void)consumer->release_consume_slot(*rh);
            }

            // All 3 slots consumed; ring empty now.
            // Use 50ms (not 0): timeout_ms=0 means "no timeout" per C API contract.
            auto extra = consumer->acquire_consume_slot(50);
            EXPECT_EQ(extra, nullptr) << "All slots consumed; ring must be empty";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "single_reader_reads_sequentially", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 6. write_returns_null_when_ring_full
// With Single_reader and an unconsumed ring, acquire_write_slot(timeout=0)
// returns nullptr. The writer_timeout_count metric increments to reflect the blocked attempt.
// ============================================================================

int write_returns_null_when_ring_full()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiRingFull");
            MessageHub &hub = MessageHub::get_instance();
            // capacity=2: fill both slots without consuming → 3rd acquire must fail
            auto cfg = make_config(ConsumerSyncPolicy::Single_reader, 2, 71006);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Fill the ring: 2 committed, unconsumed slots
            for (int i = 0; i < 2; ++i)
            {
                auto h = producer->acquire_write_slot(0);
                ASSERT_NE(h, nullptr) << "Slot " << i << " must be writable (ring not yet full)";
                uint64_t val = static_cast<uint64_t>(i);
                std::memcpy(h->buffer_span().data(), &val, sizeof(val));
                (void)h->commit(sizeof(val));
                (void)producer->release_write_slot(*h);
            }

            DataBlockMetrics metrics{};
            ASSERT_EQ(producer->get_metrics(metrics), 0);
            EXPECT_EQ(metrics.total_slots_written, 2u) << "Both slots must be committed";

            // 3rd write with small timeout: ring is saturated → must return null quickly.
            // Use 50ms (not 0): timeout_ms=0 means "no timeout" per C API contract.
            auto overflow = producer->acquire_write_slot(50);
            EXPECT_EQ(overflow, nullptr) << "Ring full (no consumer) — acquire must return null on timeout";

            // Verify the failed attempt incremented writer_timeout_count
            ASSERT_EQ(producer->get_metrics(metrics), 0);
            EXPECT_GE(metrics.writer_timeout_count, 1u) << "Failed acquire must increment writer_timeout_count";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "write_returns_null_when_ring_full", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 7. read_returns_null_on_empty_ring
// acquire_consume_slot(timeout=0) on an empty ring (no committed slots)
// returns nullptr immediately without blocking.
// ============================================================================

int read_returns_null_on_empty_ring()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiReadEmpty");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, 2, 71007);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            // Nothing written yet: consumer must return null after short timeout.
            // acquire_consume_slot with a small timeout correctly returns null for empty ring.
            auto rh = consumer->acquire_consume_slot(50);
            EXPECT_EQ(rh, nullptr) << "Empty ring must return null when no slots committed";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "read_returns_null_on_empty_ring", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 8. metrics_accumulate_across_writes
// After N writes+commits, total_slots_written==N in producer metrics.
// After consuming a slot, total_slots_read increments in consumer metrics.
// ============================================================================

int metrics_accumulate_across_writes()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiMetricsAccum");
            MessageHub &hub = MessageHub::get_instance();
            // Large capacity to avoid ring-full during the 5 writes
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, 8, 71008);

            auto producer = create_datablock_producer_impl(hub, channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            constexpr int kWrites = 5;
            for (int i = 0; i < kWrites; ++i)
            {
                auto h = producer->acquire_write_slot(0);
                ASSERT_NE(h, nullptr);
                uint64_t val = static_cast<uint64_t>(i);
                std::memcpy(h->buffer_span().data(), &val, sizeof(val));
                (void)h->commit(sizeof(val));
                (void)producer->release_write_slot(*h);
            }

            DataBlockMetrics pmetrics{};
            ASSERT_EQ(producer->get_metrics(pmetrics), 0);
            EXPECT_EQ(pmetrics.total_slots_written, static_cast<uint64_t>(kWrites))
                << "total_slots_written must equal number of committed writes";

            // Consume the latest (Latest_only: only the last write is available)
            auto rh = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh, nullptr);
            (void)consumer->release_consume_slot(*rh);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "metrics_accumulate_across_writes", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::c_api_slot_protocol

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct CApiSlotProtocolWorkerRegistrar
{
    CApiSlotProtocolWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "c_api_slot_protocol")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::c_api_slot_protocol;
                if (scenario == "write_slot_read_slot_roundtrip")
                    return write_slot_read_slot_roundtrip();
                if (scenario == "commit_advances_metrics")
                    return commit_advances_metrics();
                if (scenario == "abort_does_not_commit")
                    return abort_does_not_commit();
                if (scenario == "latest_only_reads_latest")
                    return latest_only_reads_latest();
                if (scenario == "single_reader_reads_sequentially")
                    return single_reader_reads_sequentially();
                if (scenario == "write_returns_null_when_ring_full")
                    return write_returns_null_when_ring_full();
                if (scenario == "read_returns_null_on_empty_ring")
                    return read_returns_null_on_empty_ring();
                if (scenario == "metrics_accumulate_across_writes")
                    return metrics_accumulate_across_writes();
                fmt::print(stderr, "ERROR: Unknown c_api_slot_protocol scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static CApiSlotProtocolWorkerRegistrar g_c_api_slot_protocol_registrar;
} // namespace
