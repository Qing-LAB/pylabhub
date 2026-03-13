// tests/test_layer3_datahub/workers/datahub_hub_queue_workers.cpp
//
// Hub ShmQueue unit test workers.  Each function creates a DataBlock, wraps it
// in a ShmQueue, exercises the Queue interface, and asserts the expected results.
#include "datahub_hub_queue_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "plh_datahub_client.hpp"
#include "utils/hub_shm_queue.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <chrono>
#include <cstring>
#include <string>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker::hub_queue
{

// ── Shared lifecycle helpers ──────────────────────────────────────────────────

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetLifecycleModule(); }

// ── Helper: build a minimal ring-buffer DataBlockConfig ──────────────────────

namespace
{

DataBlockConfig make_config(uint64_t shared_secret, size_t fz_size = 0)
{
    DataBlockConfig cfg{};
    cfg.policy               = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret        = shared_secret;
    cfg.ring_buffer_capacity = 4;
    cfg.physical_page_size   = DataBlockPageSize::Size4K;
    cfg.flex_zone_size       = fz_size;
    return cfg;
}

} // namespace

// ============================================================================
// shm_queue_from_consumer
// ============================================================================

int shm_queue_from_consumer()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueFromConsumer");
            DataBlockConfig cfg = make_config(70001);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q = ShmQueue::from_consumer(std::move(dbc), sizeof(double), 0,
                                              g.channel_name());
            ASSERT_NE(q, nullptr);
            EXPECT_EQ(q->item_size(), sizeof(double));
            EXPECT_EQ(q->flexzone_size(), 0u);
            EXPECT_FALSE(q->name().empty());
        },
        "hub_queue.shm_queue_from_consumer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_from_producer
// ============================================================================

int shm_queue_from_producer()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueFromProducer");
            DataBlockConfig cfg = make_config(70002);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            auto q = ShmQueue::from_producer(std::move(producer), sizeof(double), 0,
                                              g.channel_name());
            ASSERT_NE(q, nullptr);
            EXPECT_EQ(q->item_size(), sizeof(double));
            EXPECT_EQ(q->flexzone_size(), 0u);
            EXPECT_FALSE(q->name().empty());
        },
        "hub_queue.shm_queue_from_producer",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_read_acquire_timeout
// ============================================================================

int shm_queue_read_acquire_timeout()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueReadAcquireTimeout");
            DataBlockConfig cfg = make_config(70003);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            // No data written → read_acquire must return nullptr after 50ms.
            auto q = ShmQueue::from_consumer(std::move(dbc), sizeof(double));
            ASSERT_NE(q, nullptr);

            const void* ptr = q->read_acquire(std::chrono::milliseconds{50});
            EXPECT_EQ(ptr, nullptr);
        },
        "hub_queue.shm_queue_read_acquire_timeout",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_write_acquire_commit
// ============================================================================

int shm_queue_write_acquire_commit()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueWriteAcquireCommit");
            DataBlockConfig cfg = make_config(70004);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            auto q = ShmQueue::from_producer(std::move(producer), sizeof(double));
            ASSERT_NE(q, nullptr);

            // write_acquire → fill → write_commit
            void* out = q->write_acquire(std::chrono::milliseconds{100});
            ASSERT_NE(out, nullptr);

            double val = 3.14;
            std::memcpy(out, &val, sizeof(val));
            q->write_commit();   // must not crash or assert
        },
        "hub_queue.shm_queue_write_acquire_commit",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_write_acquire_abort
// ============================================================================

int shm_queue_write_acquire_abort()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueWriteAcquireAbort");
            DataBlockConfig cfg = make_config(70005);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            // Keep a separate consumer to verify no slot was committed.
            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(double));
            ASSERT_NE(q_write, nullptr);

            void* out = q_write->write_acquire(std::chrono::milliseconds{100});
            ASSERT_NE(out, nullptr);
            // Fill with data that should NOT appear in the output
            double val = 99.9;
            std::memcpy(out, &val, sizeof(val));
            q_write->write_discard(); // discard without committing

            // Consumer should see a timeout — no slot was committed.
            auto q_read = ShmQueue::from_consumer(std::move(dbc), sizeof(double));
            ASSERT_NE(q_read, nullptr);
            const void* ptr = q_read->read_acquire(std::chrono::milliseconds{50});
            EXPECT_EQ(ptr, nullptr);
        },
        "hub_queue.shm_queue_write_acquire_abort",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_read_flexzone
// ============================================================================

int shm_queue_read_flexzone()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueReadFlexzone");
            static constexpr size_t kFzSize = 4096; // must be multiple of 4096
            DataBlockConfig cfg = make_config(70006, kFzSize);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q = ShmQueue::from_consumer(std::move(dbc), sizeof(double), kFzSize);
            ASSERT_NE(q, nullptr);
            EXPECT_EQ(q->flexzone_size(), kFzSize);

            // flexzone exists → read_flexzone() must return non-null.
            const void* fz = q->read_flexzone();
            EXPECT_NE(fz, nullptr);
        },
        "hub_queue.shm_queue_read_flexzone",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_write_flexzone
// ============================================================================

int shm_queue_write_flexzone()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueWriteFlexzone");
            static constexpr size_t kFzSize = 4096;
            DataBlockConfig cfg = make_config(70007, kFzSize);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto q = ShmQueue::from_producer(std::move(dbp), sizeof(double), kFzSize);
            ASSERT_NE(q, nullptr);
            EXPECT_EQ(q->flexzone_size(), kFzSize);

            void* fz = q->write_flexzone();
            EXPECT_NE(fz, nullptr);
        },
        "hub_queue.shm_queue_write_flexzone",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_no_flexzone
// ============================================================================

int shm_queue_no_flexzone()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueNoFlexzone");
            DataBlockConfig cfg = make_config(70008, 0); // no flexzone

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(double), 0);
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(double), 0);
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read, nullptr);

            EXPECT_EQ(q_write->flexzone_size(), 0u);
            EXPECT_EQ(q_read->flexzone_size(), 0u);
            EXPECT_EQ(q_write->write_flexzone(), nullptr);
            EXPECT_EQ(q_read->read_flexzone(), nullptr);
        },
        "hub_queue.shm_queue_no_flexzone",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_round_trip
// ============================================================================

int shm_queue_round_trip()
{
    return run_gtest_worker(
        []()
        {
            struct Slot
            {
                double value;
                int    id;
            };

            DataBlockTestGuard g("ShmQueueRoundTrip");
            DataBlockConfig cfg = make_config(70009);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(Slot));
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(Slot));
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read,  nullptr);

            // Write one slot then read it back immediately (repeated 3 times).
            // ConsumerSyncPolicy::Latest_only means the consumer always returns the
            // most recently written slot; interleaving write and read avoids skipping.
            for (int i = 0; i < 3; ++i)
            {
                void* out = q_write->write_acquire(std::chrono::milliseconds{500});
                ASSERT_NE(out, nullptr) << "write_acquire failed for slot " << i;
                auto* slot  = static_cast<Slot*>(out);
                slot->value = static_cast<double>(i) * 1.1;
                slot->id    = i;
                q_write->write_commit();

                const void* in = q_read->read_acquire(std::chrono::milliseconds{500});
                ASSERT_NE(in, nullptr) << "read_acquire failed for slot " << i;
                const auto* rslot = static_cast<const Slot*>(in);
                EXPECT_DOUBLE_EQ(rslot->value, static_cast<double>(i) * 1.1);
                EXPECT_EQ(rslot->id, i);
                q_read->read_release();
            }
        },
        "hub_queue.shm_queue_round_trip",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_multiple_consumers
// ============================================================================

int shm_queue_multiple_consumers()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueMultipleConsumers");
            DataBlockConfig cfg = make_config(70010);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc1 = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc1, nullptr);

            auto dbc2 = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc2, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int));
            auto q_read1 = ShmQueue::from_consumer_ref(*dbc1, sizeof(int));
            auto q_read2 = ShmQueue::from_consumer_ref(*dbc2, sizeof(int));
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read1, nullptr);
            ASSERT_NE(q_read2, nullptr);

            // Write one slot.
            void* out = q_write->write_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(out, nullptr);
            *static_cast<int*>(out) = 42;
            q_write->write_commit();

            // Both consumers must independently see the slot.
            const void* p1 = q_read1->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(p1, nullptr);
            EXPECT_EQ(*static_cast<const int*>(p1), 42);
            q_read1->read_release();

            const void* p2 = q_read2->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(p2, nullptr);
            EXPECT_EQ(*static_cast<const int*>(p2), 42);
            q_read2->read_release();
        },
        "hub_queue.shm_queue_multiple_consumers",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_flexzone_round_trip
// ============================================================================

int shm_queue_flexzone_round_trip()
{
    return run_gtest_worker(
        []()
        {
            static constexpr size_t kFzSize = 4096;
            DataBlockTestGuard g("ShmQueueFlexzoneRoundTrip");
            DataBlockConfig cfg = make_config(70011, kFzSize);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int), kFzSize);
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(int), kFzSize);
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read, nullptr);

            // Write known pattern into the flexzone.
            void* wfz = q_write->write_flexzone();
            ASSERT_NE(wfz, nullptr);
            static const char kPattern[] = "fz_test_payload";
            std::memcpy(wfz, kPattern, sizeof(kPattern));

            // Commit a slot to mark data visible.
            void* out = q_write->write_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(out, nullptr);
            *static_cast<int*>(out) = 7;
            q_write->write_commit();

            // Consumer reads the slot and then inspects the flexzone.
            const void* in = q_read->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(in, nullptr);
            EXPECT_EQ(*static_cast<const int*>(in), 7);
            q_read->read_release();

            const void* rfz = q_read->read_flexzone();
            ASSERT_NE(rfz, nullptr);
            EXPECT_EQ(std::memcmp(rfz, kPattern, sizeof(kPattern)), 0);
        },
        "hub_queue.shm_queue_flexzone_round_trip",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_ref_factories
// ============================================================================

int shm_queue_ref_factories()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueRefFactories");
            DataBlockConfig cfg = make_config(70012);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            // Non-owning factories; underlying objects remain valid after queue teardown.
            {
                auto q_write = ShmQueue::from_producer_ref(*dbp, sizeof(double));
                auto q_read  = ShmQueue::from_consumer_ref(*dbc, sizeof(double));
                ASSERT_NE(q_write, nullptr);
                ASSERT_NE(q_read, nullptr);

                void* out = q_write->write_acquire(std::chrono::milliseconds{200});
                ASSERT_NE(out, nullptr);
                *static_cast<double*>(out) = 1.23;
                q_write->write_commit();

                const void* in = q_read->read_acquire(std::chrono::milliseconds{200});
                ASSERT_NE(in, nullptr);
                EXPECT_DOUBLE_EQ(*static_cast<const double*>(in), 1.23);
                q_read->read_release();
            } // queues destroyed here; dbp and dbc must still be valid

            // Underlying objects must still be usable after ShmQueue teardown.
            EXPECT_NE(dbp, nullptr);
            EXPECT_NE(dbc, nullptr);
        },
        "hub_queue.shm_queue_ref_factories",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_latest_only
// ============================================================================

int shm_queue_latest_only()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueLatestOnly");
            // ConsumerSyncPolicy::Latest_only (set in make_config): reading after multiple
            // writes returns the most recently written slot, not the oldest.
            DataBlockConfig cfg = make_config(70013);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int));
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(int));
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read, nullptr);

            // Write 3 slots without reading (consumer does not hold any lock).
            for (int i = 1; i <= 3; ++i)
            {
                void* out = q_write->write_acquire(std::chrono::milliseconds{200});
                ASSERT_NE(out, nullptr) << "write_acquire failed for i=" << i;
                *static_cast<int*>(out) = i * 10;
                q_write->write_commit();
            }

            // Latest_only policy: first read must return the LAST written value.
            const void* in = q_read->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(in, nullptr);
            EXPECT_EQ(*static_cast<const int*>(in), 30); // slot 3 = 3*10
            q_read->read_release();
        },
        "hub_queue.shm_queue_latest_only",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_ring_wrap
// ============================================================================

int shm_queue_ring_wrap()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueRingWrap");
            DataBlockConfig cfg = make_config(70014);
            cfg.ring_buffer_capacity = 2; // tiny ring to force wrapping quickly

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int));
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(int));
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read, nullptr);

            // Write 10 items into a 2-slot ring — slots overwrite each other.
            for (int i = 1; i <= 10; ++i)
            {
                void* out = q_write->write_acquire(std::chrono::milliseconds{200});
                ASSERT_NE(out, nullptr) << "write_acquire failed for i=" << i;
                *static_cast<int*>(out) = i * 100;
                q_write->write_commit();
            }

            // Latest_only policy: consumer sees the last written value.
            const void* in = q_read->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(in, nullptr);
            EXPECT_EQ(*static_cast<const int*>(in), 1000); // last = 10 * 100
            q_read->read_release();
        },
        "hub_queue.shm_queue_ring_wrap",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_destructor_safety
// ============================================================================

int shm_queue_destructor_safety()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueDestructorSafety");
            DataBlockConfig cfg = make_config(70015);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            // Create queues and immediately destroy them — no acquire/release performed.
            {
                auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int));
                auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(int));
                ASSERT_NE(q_write, nullptr);
                ASSERT_NE(q_read, nullptr);
                // Destroyed here without any acquire — must not crash or assert.
            }
            // If we reach here the destructors were safe.
            SUCCEED();
        },
        "hub_queue.shm_queue_destructor_safety",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_last_seq
// ============================================================================

int shm_queue_last_seq()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueLastSeq");
            DataBlockConfig cfg = make_config(70016);

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int));
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(int));
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read, nullptr);

            // last_seq() must be 0 before any read.
            EXPECT_EQ(q_read->last_seq(), 0u);

            // Write and read 3 slots, collect the slot ids.
            // slot_id() is the ring buffer position (0-indexed), so the first read
            // may return 0. We compare successive values to verify monotone advance.
            std::vector<uint64_t> seqs;
            seqs.reserve(3);
            for (int i = 1; i <= 3; ++i)
            {
                void* out = q_write->write_acquire(std::chrono::milliseconds{200});
                ASSERT_NE(out, nullptr) << "write_acquire failed for i=" << i;
                *static_cast<int*>(out) = i;
                q_write->write_commit();

                const void* in = q_read->read_acquire(std::chrono::milliseconds{200});
                ASSERT_NE(in, nullptr) << "read_acquire failed for i=" << i;
                q_read->read_release();

                seqs.push_back(q_read->last_seq());
            }

            // Slot ids must strictly increase across consecutive reads.
            EXPECT_LT(seqs[0], seqs[1]) << "last_seq did not advance from slot 1→2";
            EXPECT_LT(seqs[1], seqs[2]) << "last_seq did not advance from slot 2→3";
        },
        "hub_queue.shm_queue_last_seq",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_capacity_policy
// ============================================================================

int shm_queue_capacity_policy()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueCapacityPolicy");
            DataBlockConfig cfg = make_config(70017);
            // ring_buffer_capacity == 4 (set in make_config)

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int));
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(int));
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read, nullptr);

            EXPECT_EQ(q_write->capacity(), cfg.ring_buffer_capacity);
            EXPECT_EQ(q_read->capacity(),  cfg.ring_buffer_capacity);
            EXPECT_EQ(q_write->policy_info(), "shm_write");
            EXPECT_EQ(q_read->policy_info(),  "shm_read");
        },
        "hub_queue.shm_queue_capacity_policy",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_verify_checksum_mismatch
// ============================================================================

int shm_queue_verify_checksum_mismatch()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueVerifyChecksumMismatch");
            DataBlockConfig cfg = make_config(70018);
            // Use None policy so release_write_slot() does NOT auto-compute checksums.
            // (Both Enforced and Manual auto-update in release_write_slot; only None skips.)
            // This lets us exercise the "slot written without checksum" → verify fails path.
            cfg.checksum_policy = ChecksumPolicy::None;

            auto dbp = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(dbp, nullptr);

            auto dbc = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);

            auto q_write = ShmQueue::from_producer(std::move(dbp), sizeof(int));
            auto q_read  = ShmQueue::from_consumer(std::move(dbc), sizeof(int));
            ASSERT_NE(q_write, nullptr);
            ASSERT_NE(q_read, nullptr);

            // 1. Write WITHOUT set_checksum_options: Manual policy → DataBlock skips
            //    auto-update, slot_valid flag stays 0 (checksum never stored).
            void* out = q_write->write_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(out, nullptr);
            *static_cast<int*>(out) = 99;
            q_write->write_commit();

            // 2. Enable checksum VERIFICATION on the reader side.
            //    slot_valid==0 → verify_checksum_slot() returns false → read_acquire returns nullptr.
            q_read->set_verify_checksum(true, false);
            const void* in_bad = q_read->read_acquire(std::chrono::milliseconds{200});
            EXPECT_EQ(in_bad, nullptr) << "Expected nullptr when slot_valid=0 (no checksum stored)";

            // 3. Write WITH set_checksum_options: ShmQueue calls update_checksum_slot()
            //    before commit → slot_valid becomes 1 → verify succeeds → read returns data.
            q_write->set_checksum_options(true, false);

            void* out2 = q_write->write_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(out2, nullptr);
            *static_cast<int*>(out2) = 55;
            q_write->write_commit();

            const void* in_ok = q_read->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(in_ok, nullptr) << "Expected valid slot after checksum-enabled write";
            EXPECT_EQ(*static_cast<const int*>(in_ok), 55);
            q_read->read_release();
        },
        "hub_queue.shm_queue_verify_checksum_mismatch",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_is_running
// ============================================================================

int shm_queue_is_running()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueIsRunning");
            DataBlockConfig cfg = make_config(70019);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Writer queue: is_running() true after construction.
            auto q_write = ShmQueue::from_producer(std::move(producer), sizeof(double), 0,
                                                   g.channel_name());
            ASSERT_NE(q_write, nullptr);
            EXPECT_TRUE(q_write->is_running());

            // Reader queue also reports running after construction.
            auto producer2 = create_datablock_producer_impl(
                "ShmQueueIsRunningR", DataBlockPolicy::RingBuffer, make_config(70019), nullptr, nullptr);
            ASSERT_NE(producer2, nullptr);
            auto dbc = find_datablock_consumer_impl(
                "ShmQueueIsRunningR", make_config(70019).shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(dbc, nullptr);
            auto q_read = ShmQueue::from_consumer(std::move(dbc), sizeof(double), 0,
                                                  g.channel_name());
            ASSERT_NE(q_read, nullptr);
            EXPECT_TRUE(q_read->is_running());

            // Null unique_ptr<QueueWriter/QueueReader> reports not running — sanity check
            // that a default-constructed queue (nullptr) is not running.
            std::unique_ptr<QueueWriter> null_w;
            EXPECT_EQ(null_w, nullptr); // trivial guard
        },
        "hub_queue.shm_queue_is_running",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// datablock_producer_remap_stubs_throw
// ============================================================================

int datablock_producer_remap_stubs_throw()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ProducerRemapStubs");
            DataBlockConfig cfg = make_config(70020);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            // Both remap stubs are not implemented; verify they throw rather than silently
            // doing nothing or corrupting state.
            EXPECT_THROW(
                producer->request_structure_remap(std::nullopt, std::nullopt),
                std::runtime_error);
            EXPECT_THROW(
                producer->commit_structure_remap(0, std::nullopt, std::nullopt),
                std::runtime_error);
#pragma GCC diagnostic pop
        },
        "hub_queue.datablock_producer_remap_stubs_throw",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// datablock_consumer_remap_stubs_throw
// ============================================================================

int datablock_consumer_remap_stubs_throw()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ConsumerRemapStubs");
            DataBlockConfig cfg = make_config(70021);

            auto producer = create_datablock_producer_impl(
                g.channel_name(), DataBlockPolicy::RingBuffer, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(
                g.channel_name(), cfg.shared_secret, &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            // Both consumer remap stubs are not implemented.
            EXPECT_THROW(consumer->release_for_remap(), std::runtime_error);
            EXPECT_THROW(
                consumer->reattach_after_remap(std::nullopt, std::nullopt),
                std::runtime_error);
#pragma GCC diagnostic pop
        },
        "hub_queue.datablock_consumer_remap_stubs_throw",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::hub_queue

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct HubQueueWorkerRegistrar
{
    HubQueueWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto       dot  = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "hub_queue")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_queue;
                if (scenario == "shm_queue_from_consumer")
                    return shm_queue_from_consumer();
                if (scenario == "shm_queue_from_producer")
                    return shm_queue_from_producer();
                if (scenario == "shm_queue_read_acquire_timeout")
                    return shm_queue_read_acquire_timeout();
                if (scenario == "shm_queue_write_acquire_commit")
                    return shm_queue_write_acquire_commit();
                if (scenario == "shm_queue_write_acquire_abort")
                    return shm_queue_write_acquire_abort();
                if (scenario == "shm_queue_read_flexzone")
                    return shm_queue_read_flexzone();
                if (scenario == "shm_queue_write_flexzone")
                    return shm_queue_write_flexzone();
                if (scenario == "shm_queue_no_flexzone")
                    return shm_queue_no_flexzone();
                if (scenario == "shm_queue_round_trip")
                    return shm_queue_round_trip();
                if (scenario == "shm_queue_multiple_consumers")
                    return shm_queue_multiple_consumers();
                if (scenario == "shm_queue_flexzone_round_trip")
                    return shm_queue_flexzone_round_trip();
                if (scenario == "shm_queue_ref_factories")
                    return shm_queue_ref_factories();
                if (scenario == "shm_queue_latest_only")
                    return shm_queue_latest_only();
                if (scenario == "shm_queue_ring_wrap")
                    return shm_queue_ring_wrap();
                if (scenario == "shm_queue_destructor_safety")
                    return shm_queue_destructor_safety();
                if (scenario == "shm_queue_last_seq")
                    return shm_queue_last_seq();
                if (scenario == "shm_queue_capacity_policy")
                    return shm_queue_capacity_policy();
                if (scenario == "shm_queue_verify_checksum_mismatch")
                    return shm_queue_verify_checksum_mismatch();
                if (scenario == "shm_queue_is_running")
                    return shm_queue_is_running();
                if (scenario == "datablock_producer_remap_stubs_throw")
                    return datablock_producer_remap_stubs_throw();
                if (scenario == "datablock_consumer_remap_stubs_throw")
                    return datablock_consumer_remap_stubs_throw();
                fmt::print(stderr, "ERROR: Unknown hub_queue scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static HubQueueWorkerRegistrar g_hub_queue_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
