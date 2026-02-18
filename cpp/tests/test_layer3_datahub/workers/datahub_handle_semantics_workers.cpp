// tests/test_layer3_datahub/workers/handle_semantics_workers.cpp
//
// C++ handle move-semantics and lifecycle tests.
// Verifies that DataBlockProducer, DataBlockConsumer, SlotWriteHandle, and
// SlotConsumeHandle correctly implement move ownership transfer and that
// moved-from objects are safely inert.
//
// Secret numbers: 74001+ to avoid conflicts with other test suites.

#include "datahub_handle_semantics_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <utility>

using namespace pylabhub::hub;
using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker::handle_semantics
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

static DataBlockConfig make_config(uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 2;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.flex_zone_size = sizeof(EmptyFlexZone);
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

// ============================================================================
// move_producer_transfers_ownership
// After std::move, the original producer is empty (nullptr-like) and the
// moved-to producer correctly creates and publishes a slot.
// ============================================================================

int move_producer_transfers_ownership()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("MoveProducer");
            auto cfg = make_config(74001);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // Move the producer — original should be empty
            auto producer2 = std::move(producer);
            EXPECT_EQ(producer, nullptr) << "Moved-from producer must be null";
            EXPECT_NE(producer2, nullptr) << "Moved-to producer must be valid";

            // Moved-to producer must work normally
            bool write_ok = false;
            producer2->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [&write_ok](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            result.content().get().sequence = 7;
                            write_ok = true;
                            break;
                        }
                    }
                });
            EXPECT_TRUE(write_ok);

            producer2.reset();
            cleanup_test_datablock(channel);
        },
        "move_producer_transfers_ownership", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// move_consumer_transfers_ownership
// After std::move, the original consumer is empty and the moved-to consumer
// reads a slot correctly.
// ============================================================================

int move_consumer_transfers_ownership()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("MoveConsumer");
            auto cfg = make_config(74002);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
                channel, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Write a slot first
            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            result.content().get().sequence = 3;
                            break;
                        }
                    }
                });

            // Move consumer — original must be null
            auto consumer2 = std::move(consumer);
            EXPECT_EQ(consumer, nullptr) << "Moved-from consumer must be null";
            EXPECT_NE(consumer2, nullptr) << "Moved-to consumer must be valid";

            // Moved-to consumer must read the slot
            bool read_ok = false;
            consumer2->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [&read_ok](ReadTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            EXPECT_EQ(result.content().get().sequence, 3u);
                            read_ok = true;
                            break;
                        }
                    }
                });
            EXPECT_TRUE(read_ok);

            producer.reset();
            consumer2.reset();
            cleanup_test_datablock(channel);
        },
        "move_consumer_transfers_ownership", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// default_constructed_handles_are_invalid
// A default-constructed SlotWriteHandle or SlotConsumeHandle must report
// invalid state: release operations return false, no UB.
// ============================================================================

int default_constructed_handles_are_invalid()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("DefaultHandles");
            auto cfg = make_config(74003);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
                channel, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Default-constructed handles are invalid — release must return false
            SlotWriteHandle invalid_write{};
            EXPECT_FALSE(producer->release_write_slot(invalid_write))
                << "release_write_slot on default-constructed handle must return false";

            SlotConsumeHandle invalid_consume{};
            EXPECT_FALSE(consumer->release_consume_slot(invalid_consume))
                << "release_consume_slot on default-constructed handle must return false";

            // Move a valid write handle — the moved-from handle is invalid
            auto write_h = producer->acquire_write_slot(500);
            ASSERT_NE(write_h, nullptr);
            SlotWriteHandle moved_write = std::move(*write_h);
            // moved_write holds the slot; write_h is now in a moved-from state
            // Release the moved-to handle — must succeed
            EXPECT_TRUE(producer->release_write_slot(moved_write));

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "default_constructed_handles_are_invalid", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::handle_semantics

namespace
{
struct HandleSemanticsWorkerRegistrar
{
    HandleSemanticsWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "handle_semantics")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::handle_semantics;
                if (scenario == "move_producer_transfers_ownership")
                    return move_producer_transfers_ownership();
                if (scenario == "move_consumer_transfers_ownership")
                    return move_consumer_transfers_ownership();
                if (scenario == "default_constructed_handles_are_invalid")
                    return default_constructed_handles_are_invalid();
                fmt::print(stderr, "ERROR: Unknown handle_semantics scenario '{}'\n", scenario);
                return 1;
            });
    }
};
HandleSemanticsWorkerRegistrar g_handle_semantics_registrar;
} // namespace
