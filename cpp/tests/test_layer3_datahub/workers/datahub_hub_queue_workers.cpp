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
            q_write->write_abort(); // discard without committing

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
                fmt::print(stderr, "ERROR: Unknown hub_queue scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static HubQueueWorkerRegistrar g_hub_queue_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
