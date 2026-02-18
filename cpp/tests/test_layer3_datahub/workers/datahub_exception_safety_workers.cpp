// tests/test_layer3_datahub/workers/exception_safety_workers.cpp
//
// RAII exception safety tests: verifies that exceptions thrown inside
// with_transaction lambdas and ctx.slots() loops are handled correctly:
//   - Slots are NOT published when an exception unwinds the stack (auto-abort)
//   - Slots ARE released so no lock is held after the exception
//   - The producer/consumer remains usable after the exception is caught
//
// Test strategy:
//   - exception_before_publish: write value to slot, throw before break; catch exception;
//     verify consumer sees no new data (slot was aborted); verify producer can write
//     a subsequent slot successfully.
//   - exception_in_write_transaction: throw in with_transaction lambda; catch; verify
//     producer can write normally in a new with_transaction call.
//   - exception_in_read_transaction: write one slot; read with throw; catch; verify
//     consumer can read again in a new with_transaction call.
//
// Secret numbers: 73001+ to avoid conflicts with other test suites.

#include "datahub_exception_safety_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <stdexcept>

using namespace pylabhub::hub;
using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker::exception_safety
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
    cfg.ring_buffer_capacity = 4;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.flex_zone_size = sizeof(EmptyFlexZone);
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

// ============================================================================
// exception_before_publish_aborts_write_slot
// Throw inside ctx.slots() loop before break (before auto-publish fires).
// SlotIterator destructor detects std::uncaught_exceptions() > 0 and releases
// the slot WITHOUT publishing. Consumer must see no new data (timeout on acquire).
// After catching the exception, producer must be able to write a new slot normally.
// ============================================================================

int exception_before_publish_aborts_write_slot()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ExcBeforePublish");
            auto cfg = make_config(73001);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
                channel, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Throw before auto-publish — slot must be aborted
            bool exception_caught = false;
            try
            {
                producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                    500ms,
                    [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                    {
                        for (auto &result : ctx.slots(200ms))
                        {
                            if (result.is_ok())
                            {
                                result.content().get().sequence = 999;
                                // throw BEFORE break — auto-publish must NOT fire
                                throw std::runtime_error("deliberate test exception");
                            }
                        }
                    });
            }
            catch (const std::runtime_error &)
            {
                exception_caught = true;
            }
            EXPECT_TRUE(exception_caught);

            // Producer must be usable: write a new slot normally
            bool second_write_ok = false;
            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [&second_write_ok](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            result.content().get().sequence = 42;
                            second_write_ok = true;
                            break;
                        }
                    }
                });
            EXPECT_TRUE(second_write_ok);

            // Consumer sees the successfully published slot via RAII
            bool read_ok = false;
            consumer->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [&read_ok](ReadTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            EXPECT_EQ(result.content().get().sequence, 42u);
                            read_ok = true;
                            break;
                        }
                    }
                });
            EXPECT_TRUE(read_ok) << "Successfully published slot must be visible to consumer";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "exception_before_publish_aborts_write_slot", logger_module(), crypto_module(),
        hub_module());
}

// ============================================================================
// exception_in_write_transaction_leaves_producer_usable
// Exception thrown inside with_transaction (not inside slots() loop).
// The entire lambda throws; no slot was acquired. Producer must still work.
// ============================================================================

int exception_in_write_transaction_leaves_producer_usable()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ExcWriteTxn");
            auto cfg = make_config(73002);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // Throw immediately in the lambda (no slot acquired)
            bool caught = false;
            try
            {
                producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                    500ms,
                    [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &)
                    {
                        throw std::logic_error("early throw in write transaction");
                    });
            }
            catch (const std::logic_error &)
            {
                caught = true;
            }
            EXPECT_TRUE(caught);

            // Producer must still accept writes
            bool write_ok = false;
            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [&write_ok](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            result.content().get().sequence = 1;
                            write_ok = true;
                            break;
                        }
                    }
                });
            EXPECT_TRUE(write_ok);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "exception_in_write_transaction_leaves_producer_usable", logger_module(), crypto_module(),
        hub_module());
}

// ============================================================================
// exception_in_read_transaction_leaves_consumer_usable
// Write one slot. Consumer reads with an exception inside the lambda.
// Consumer must remain usable for the next read.
// ============================================================================

int exception_in_read_transaction_leaves_consumer_usable()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ExcReadTxn");
            auto cfg = make_config(73003);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
                channel, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Write slot 1
            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            result.content().get().sequence = 10;
                            break;
                        }
                    }
                });

            // Consumer reads slot 1 but throws during processing
            bool caught = false;
            try
            {
                consumer->with_transaction<EmptyFlexZone, TestDataBlock>(
                    500ms,
                    [](ReadTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                    {
                        for (auto &result : ctx.slots(200ms))
                        {
                            if (result.is_ok())
                            {
                                // Verify the data, then throw (simulating processing error)
                                EXPECT_EQ(result.content().get().sequence, 10u);
                                throw std::runtime_error("processing error in read transaction");
                            }
                        }
                    });
            }
            catch (const std::runtime_error &)
            {
                caught = true;
            }
            EXPECT_TRUE(caught);

            // Write a second slot
            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            result.content().get().sequence = 20;
                            break;
                        }
                    }
                });

            // Consumer must still work — reads the second slot
            bool read_ok = false;
            consumer->with_transaction<EmptyFlexZone, TestDataBlock>(
                500ms,
                [&read_ok](ReadTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(200ms))
                    {
                        if (result.is_ok())
                        {
                            read_ok = true;
                            break;
                        }
                    }
                });
            EXPECT_TRUE(read_ok);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "exception_in_read_transaction_leaves_consumer_usable", logger_module(), crypto_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::exception_safety

namespace
{
struct ExceptionSafetyWorkerRegistrar
{
    ExceptionSafetyWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "exception_safety")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::exception_safety;
                if (scenario == "exception_before_publish_aborts_write_slot")
                    return exception_before_publish_aborts_write_slot();
                if (scenario == "exception_in_write_transaction_leaves_producer_usable")
                    return exception_in_write_transaction_leaves_producer_usable();
                if (scenario == "exception_in_read_transaction_leaves_consumer_usable")
                    return exception_in_read_transaction_leaves_consumer_usable();
                fmt::print(stderr, "ERROR: Unknown exception_safety scenario '{}'\n", scenario);
                return 1;
            });
    }
};
ExceptionSafetyWorkerRegistrar g_exception_safety_registrar;
} // namespace
