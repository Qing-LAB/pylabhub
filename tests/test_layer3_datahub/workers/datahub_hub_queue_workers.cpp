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
static auto hub_module()    { return ::pylabhub::hub::GetDataBlockModule(); }

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace
{

/// One float64 field — matches sizeof(double).
std::vector<SchemaFieldDesc> double_schema()
{
    SchemaFieldDesc f; f.type_str = "float64"; f.count = 1;
    return {f};
}

/// One int32 field — matches sizeof(int).
std::vector<SchemaFieldDesc> int_schema()
{
    SchemaFieldDesc f; f.type_str = "int32"; f.count = 1;
    return {f};
}

/// One uint64 field — matches sizeof(uint64_t).
std::vector<SchemaFieldDesc> uint64_schema()
{
    SchemaFieldDesc f; f.type_str = "uint64"; f.count = 1;
    return {f};
}

/// Two-field struct: {double value; int id;} — matches sizeof(Slot) with aligned packing.
std::vector<SchemaFieldDesc> slot_schema()
{
    SchemaFieldDesc f1; f1.type_str = "float64"; f1.count = 1;
    SchemaFieldDesc f2; f2.type_str = "int32";   f2.count = 1;
    return {f1, f2};
}

/// DataBlockConfig for raw DataBlock tests (not ShmQueue — remap stubs etc.).
DataBlockConfig make_config(uint64_t shared_secret)
{
    DataBlockConfig cfg{};
    cfg.policy               = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret        = shared_secret;
    cfg.ring_buffer_capacity = 4;
    cfg.physical_page_size   = DataBlockPageSize::Size4K;
    return cfg;
}

/// One float64 field for flexzone.
std::vector<SchemaFieldDesc> fz_schema()
{
    SchemaFieldDesc f; f.type_str = "float64"; f.count = 1;
    return {f};
}

/// SHM params (no sizes — ShmQueue computes from schema).
struct ShmParams
{
    uint64_t           secret;
    uint32_t           capacity{4};
    DataBlockPageSize  page_size{DataBlockPageSize::Size4K};
    DataBlockPolicy    policy{DataBlockPolicy::RingBuffer};
    ConsumerSyncPolicy sync{ConsumerSyncPolicy::Latest_only};
    ChecksumPolicy     checksum{ChecksumPolicy::Enforced};
};

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
            ShmParams p{70001};

            // Create writer first (creates SHM).
            auto writer = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(writer, nullptr);

            // Create reader (attaches to existing SHM).
            auto reader = ShmQueue::create_reader(
                g.channel_name(), p.secret, double_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(reader, nullptr);
            EXPECT_EQ(reader->item_size(), sizeof(double));
            EXPECT_EQ(reader->flexzone_size(), 0u);
            EXPECT_FALSE(reader->name().empty());
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
            ShmParams p{70002};

            auto q = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
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
            ShmParams p{70003};

            // Writer must exist first (creates SHM).
            auto writer = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(writer, nullptr);

            // No data written → read_acquire must return nullptr after 50ms.
            auto reader = ShmQueue::create_reader(
                g.channel_name(), p.secret, double_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(reader, nullptr);

            const void* ptr = reader->read_acquire(std::chrono::milliseconds{50});
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
            ShmParams p{70004};

            auto q = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
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
            ShmParams p{70005};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            void* out = q_write->write_acquire(std::chrono::milliseconds{100});
            ASSERT_NE(out, nullptr);
            // Fill with data that should NOT appear in the output
            double val = 99.9;
            std::memcpy(out, &val, sizeof(val));
            q_write->write_discard(); // discard without committing

            // Consumer should see a timeout — no slot was committed.
            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, double_schema(), "aligned",
                g.channel_name());
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
            ShmParams p{70006};

            // Writer must exist first (creates SHM with flexzone).
            auto writer = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", fz_schema(), "aligned",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(writer, nullptr);

            auto reader = ShmQueue::create_reader(
                g.channel_name(), p.secret, double_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(reader, nullptr);
            EXPECT_GT(reader->flexzone_size(), 0u);

            // flexzone exists → read_flexzone() must return non-null.
            const void* fz = reader->flexzone();
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
            ShmParams p{70007};

            auto q = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", fz_schema(), "aligned",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q, nullptr);
            EXPECT_GT(q->flexzone_size(), 0u);

            void* fz = q->flexzone();
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
            ShmParams p{70008};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, double_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read, nullptr);

            EXPECT_EQ(q_write->flexzone_size(), 0u);
            EXPECT_EQ(q_read->flexzone_size(), 0u);
            EXPECT_EQ(q_write->flexzone(), nullptr);
            EXPECT_EQ(q_read->flexzone(), nullptr);
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
            ShmParams p{70009};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), slot_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, slot_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read, nullptr);

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
            ShmParams p{70010};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read1 = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read1, nullptr);

            auto q_read2 = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
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
            DataBlockTestGuard g("ShmQueueFlexzoneRoundTrip");
            ShmParams p{70011};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", fz_schema(), "aligned",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read, nullptr);

            // Write known pattern into the flexzone.
            void* wfz = q_write->flexzone();
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

            const void* rfz = q_read->flexzone();
            ASSERT_NE(rfz, nullptr);
            EXPECT_EQ(std::memcmp(rfz, kPattern, sizeof(kPattern)), 0);
        },
        "hub_queue.shm_queue_flexzone_round_trip",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_flexzone_bidirectional
//
// Verifies QueueReader::flexzone() and QueueWriter::flexzone() match the
// HEP-CORE-0002 §2.2 design: one shared region per channel, fully read+write
// on every endpoint (user-managed bidirectional coordination).
//
// Concretely:
//   (a) Writer-side and reader-side flexzone() pointers both resolve to
//       non-null on a channel created with a flexzone schema, and point
//       into the same physical shared-memory region.
//   (b) Writer-side writes are observed via the reader-side pointer.
//   (c) Reader-side writes are observed via the writer-side pointer —
//       this is the bidirectional property that distinguishes the flexzone
//       from the slot ring (where writer commits and reader releases).
// ============================================================================

int shm_queue_flexzone_bidirectional()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueFlexzoneBidirectional");
            ShmParams p{70019};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", fz_schema(), "aligned",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read, nullptr);

            // (a) Both endpoints expose a mutable flexzone pointer.
            void *wfz = q_write->flexzone();
            void *rfz = q_read->flexzone();
            ASSERT_NE(wfz, nullptr);
            ASSERT_NE(rfz, nullptr);
            ASSERT_GT(q_write->flexzone_size(), 0u);
            ASSERT_EQ(q_write->flexzone_size(), q_read->flexzone_size());

            // (b) Writer→reader visibility.
            static const char kForward[] = "writer_to_reader";
            std::memcpy(wfz, kForward, sizeof(kForward));

            void *slot = q_write->write_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(slot, nullptr);
            *static_cast<int*>(slot) = 1;
            q_write->write_commit();

            const void *in = q_read->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(in, nullptr);
            q_read->read_release();

            EXPECT_EQ(std::memcmp(rfz, kForward, sizeof(kForward)), 0);

            // (c) Reader→writer visibility (bidirectional per HEP-CORE-0002 §2.2).
            static const char kReply[] = "reader_to_writer";
            std::memcpy(static_cast<char*>(rfz) + 64, kReply, sizeof(kReply));
            EXPECT_EQ(std::memcmp(static_cast<char*>(wfz) + 64,
                                  kReply, sizeof(kReply)),
                      0);
        },
        "hub_queue.shm_queue_flexzone_bidirectional",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_create_factories
// ============================================================================

int shm_queue_create_factories()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueCreateFactories");
            ShmParams p{70012};

            // Owning create_writer / create_reader factories.
            auto q_write = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, double_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read, nullptr);

            void* out = q_write->write_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(out, nullptr);
            *static_cast<double*>(out) = 1.23;
            q_write->write_commit();

            const void* in = q_read->read_acquire(std::chrono::milliseconds{200});
            ASSERT_NE(in, nullptr);
            EXPECT_DOUBLE_EQ(*static_cast<const double*>(in), 1.23);
            q_read->read_release();
        },
        "hub_queue.shm_queue_create_factories",
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
            // ConsumerSyncPolicy::Latest_only (set in ShmParams): reading after multiple
            // writes returns the most recently written slot, not the oldest.
            ShmParams p{70013};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
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
            ShmParams p{70014};
            p.capacity = 2; // tiny ring to force wrapping quickly

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
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
            ShmParams p{70015};

            // Create queues and immediately destroy them — no acquire/release performed.
            {
                auto q_write = ShmQueue::create_writer(
                    g.channel_name(), int_schema(), "aligned", {}, "",
                    p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
                ASSERT_NE(q_write, nullptr);

                auto q_read = ShmQueue::create_reader(
                    g.channel_name(), p.secret, int_schema(), "aligned",
                    g.channel_name());
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
            ShmParams p{70016};

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
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
            ShmParams p{70017};
            // ring_buffer_capacity == 4 (set in ShmParams)

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read, nullptr);

            EXPECT_EQ(q_write->capacity(), p.capacity);
            EXPECT_EQ(q_read->capacity(),  p.capacity);
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
            // Use None policy so release_write_slot() does NOT auto-compute checksums.
            // (Both Enforced and Manual auto-update in release_write_slot; only None skips.)
            // This lets us exercise the "slot written without checksum" → verify fails path.
            ShmParams p{70018};
            p.checksum = ChecksumPolicy::None;

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), int_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, int_schema(), "aligned",
                g.channel_name());
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
            ShmParams p{70019};

            // Writer queue: is_running() true after construction.
            auto q_write = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);
            EXPECT_TRUE(q_write->is_running());

            // Reader queue also reports running after construction.
            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, double_schema(), "aligned",
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
            // doing nothing or corrupting state.  Pin the message
            // substring (function-name prefix) per audit §1.1 — type
            // alone (`runtime_error`) is shared with other hub_queue
            // throw sites and would not catch a regression where one
            // stub's throw fires from the wrong path.
            {
                bool threw = false; std::string msg;
                try { (void)producer->request_structure_remap(std::nullopt, std::nullopt); }
                catch (const std::runtime_error &e) { threw = true; msg = e.what(); }
                EXPECT_TRUE(threw);
                EXPECT_NE(msg.find("DataBlockProducer::request_structure_remap"),
                          std::string::npos)
                    << "wrong runtime_error path; what(): " << msg;
            }
            {
                bool threw = false; std::string msg;
                try { producer->commit_structure_remap(0, std::nullopt, std::nullopt); }
                catch (const std::runtime_error &e) { threw = true; msg = e.what(); }
                EXPECT_TRUE(threw);
                EXPECT_NE(msg.find("DataBlockProducer::commit_structure_remap"),
                          std::string::npos)
                    << "wrong runtime_error path; what(): " << msg;
            }
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
            // Both consumer remap stubs are not implemented.  Pin
            // function-name substring per audit §1.1.
            {
                bool threw = false; std::string msg;
                try { consumer->release_for_remap(); }
                catch (const std::runtime_error &e) { threw = true; msg = e.what(); }
                EXPECT_TRUE(threw);
                EXPECT_NE(msg.find("DataBlockConsumer::release_for_remap"),
                          std::string::npos)
                    << "wrong runtime_error path; what(): " << msg;
            }
            {
                bool threw = false; std::string msg;
                try { consumer->reattach_after_remap(std::nullopt, std::nullopt); }
                catch (const std::runtime_error &e) { threw = true; msg = e.what(); }
                EXPECT_TRUE(threw);
                EXPECT_NE(msg.find("DataBlockConsumer::reattach_after_remap"),
                          std::string::npos)
                    << "wrong runtime_error path; what(): " << msg;
            }
#pragma GCC diagnostic pop
        },
        "hub_queue.datablock_consumer_remap_stubs_throw",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// shm_queue_discard_then_reacquire
// ============================================================================

int shm_queue_discard_then_reacquire()
{
    return run_gtest_worker(
        []()
        {
            // Regression test for write_index burn-on-discard bug.
            //
            // Under Sequential consumer sync policy, write_acquire() blocks when
            // write_idx - read_idx >= slot_count.  Before the fix, write_discard()
            // did not decrement write_index, so after `capacity` discards the ring
            // appeared permanently full and all subsequent write_acquire() calls
            // timed out.
            //
            // This test uses Sequential policy (not Latest_only) and discards more
            // slots than the ring capacity, then verifies that write_acquire +
            // write_commit still works and the consumer receives the data.

            DataBlockTestGuard g("ShmQueueDiscardReacquire");
            ShmParams p{70022};
            p.sync = ConsumerSyncPolicy::Sequential;

            auto q_write = ShmQueue::create_writer(
                g.channel_name(), uint64_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(q_write, nullptr);

            auto q_read = ShmQueue::create_reader(
                g.channel_name(), p.secret, uint64_schema(), "aligned",
                g.channel_name());
            ASSERT_NE(q_read, nullptr);

            // Discard MORE slots than ring capacity (the exact scenario that
            // triggered the original bug).
            constexpr int kDiscards = 8; // 2× capacity
            for (int i = 0; i < kDiscards; ++i)
            {
                void* out = q_write->write_acquire(std::chrono::milliseconds{500});
                ASSERT_NE(out, nullptr)
                    << "write_acquire failed on discard iteration " << i
                    << " (ring should not be full after discard)";
                q_write->write_discard();
            }

            // Now commit a real slot — must succeed.
            void* out = q_write->write_acquire(std::chrono::milliseconds{500});
            ASSERT_NE(out, nullptr)
                << "write_acquire failed after " << kDiscards
                << " discards — write_index was not restored on discard";
            uint64_t val = 42;
            std::memcpy(out, &val, sizeof(val));
            q_write->write_commit();

            // Consumer must see the committed data.
            const void* in = q_read->read_acquire(std::chrono::milliseconds{500});
            ASSERT_NE(in, nullptr) << "consumer read_acquire timed out";
            uint64_t got = 0;
            std::memcpy(&got, in, sizeof(got));
            EXPECT_EQ(got, 42u);
            q_read->read_release();
        },
        "hub_queue.shm_queue_discard_then_reacquire",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Error path: create_writer with empty schema → nullptr
// ============================================================================

int shm_queue_create_writer_empty_schema()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueCreateWriterEmptySchema");
            ShmParams p{70022};

            auto q = ShmQueue::create_writer(
                g.channel_name(), {}, "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            EXPECT_EQ(q, nullptr) << "create_writer with empty schema must return nullptr";
        },
        "hub_queue.shm_queue_create_writer_empty_schema",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Error path: create_reader with wrong secret → nullptr
// ============================================================================

int shm_queue_create_reader_wrong_secret()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueCreateReaderWrongSecret");
            ShmParams p{70023};

            auto writer = ShmQueue::create_writer(
                g.channel_name(), double_schema(), "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            ASSERT_NE(writer, nullptr);

            // Wrong secret → attachment fails → nullptr.
            auto reader = ShmQueue::create_reader(
                g.channel_name(), p.secret + 1, double_schema(), "aligned",
                g.channel_name());
            EXPECT_EQ(reader, nullptr) << "create_reader with wrong secret must return nullptr";
        },
        "hub_queue.shm_queue_create_reader_wrong_secret",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Error path: create_reader for nonexistent SHM → nullptr
// ============================================================================

int shm_queue_create_reader_nonexistent()
{
    return run_gtest_worker(
        []()
        {
            // No writer created → SHM doesn't exist.
            auto reader = ShmQueue::create_reader(
                "nonexistent_shm_segment_12345", 99999,
                double_schema(), "aligned", "test");
            EXPECT_EQ(reader, nullptr) << "create_reader for nonexistent SHM must return nullptr";
        },
        "hub_queue.shm_queue_create_reader_nonexistent",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Error path: create_writer with zero-size schema (bytes length=0) → nullptr
// ============================================================================

int shm_queue_create_writer_zero_size_schema()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g("ShmQueueCreateWriterZeroSize");
            ShmParams p{70024};

            // Bypass the parser: create a SchemaFieldDesc with bytes length=0.
            // compute_field_layout produces item_size=0 → create_writer must reject.
            std::vector<hub::SchemaFieldDesc> schema = {{"bytes", 1, 0}};

            auto q = ShmQueue::create_writer(
                g.channel_name(), schema, "aligned", {}, "",
                p.capacity, p.page_size, p.secret, p.policy, p.sync, p.checksum);
            EXPECT_EQ(q, nullptr) << "create_writer with zero-size schema must return nullptr";
        },
        "hub_queue.shm_queue_create_writer_zero_size_schema",
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
                if (scenario == "shm_queue_flexzone_bidirectional")
                    return shm_queue_flexzone_bidirectional();
                if (scenario == "shm_queue_create_factories")
                    return shm_queue_create_factories();
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
                if (scenario == "shm_queue_discard_then_reacquire")
                    return shm_queue_discard_then_reacquire();
                if (scenario == "shm_queue_create_writer_empty_schema")
                    return shm_queue_create_writer_empty_schema();
                if (scenario == "shm_queue_create_reader_wrong_secret")
                    return shm_queue_create_reader_wrong_secret();
                if (scenario == "shm_queue_create_reader_nonexistent")
                    return shm_queue_create_reader_nonexistent();
                if (scenario == "shm_queue_create_writer_zero_size_schema")
                    return shm_queue_create_writer_zero_size_schema();
                fmt::print(stderr, "ERROR: Unknown hub_queue scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static HubQueueWorkerRegistrar g_hub_queue_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
