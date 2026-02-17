// tests/test_layer3_datahub/workers/transaction_api_workers.cpp
// Transaction API tests for v1.0.0: with_transaction<FlexZoneT, DataBlockT>() RAII layer
// Tests dual-schema architecture, exception safety, and resource cleanup
//
// Test Strategy:
// - All tests use the new v1.0.0 API: producer->with_transaction<FlexZoneT, DataBlockT>()
// - Tests verify exception safety: exceptions trigger automatic slot cleanup
// - Tests use dual-schema types (FlexZone + DataBlock) to verify v1.0.0 architecture
// - Resource lifecycle is carefully managed (handles destroyed before producer/consumer reset)
//
#include "datahub_transaction_api_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>
#include <chrono>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

// ============================================================================
// Test Data Structures (v1.0.0 dual-schema)
// Note: defined at file scope so PYLABHUB_SCHEMA_BEGIN/END macros work correctly
// (they open namespace pylabhub::schema {} which must be at non-local scope).
// ============================================================================

/**
 * @brief FlexZone structure for shared metadata
 * @note Must be trivially copyable for shared memory
 */
struct TxAPITestFlexZone
{
    std::atomic<uint32_t> transaction_count{0};
    std::atomic<bool> test_flag{false};
};
static_assert(std::is_trivially_copyable_v<TxAPITestFlexZone>);

/**
 * @brief DataBlock structure for per-slot messages
 * @note Must be trivially copyable for shared memory
 */
struct TxAPITestMessage
{
    uint64_t sequence;
    uint32_t value;
    char payload[48]; // Total 64 bytes
};
static_assert(std::is_trivially_copyable_v<TxAPITestMessage>);

// Register BLDS schemas for dual-schema validation (must be at file scope)
PYLABHUB_SCHEMA_BEGIN(TxAPITestFlexZone)
    PYLABHUB_SCHEMA_MEMBER(transaction_count)
    PYLABHUB_SCHEMA_MEMBER(test_flag)
PYLABHUB_SCHEMA_END(TxAPITestFlexZone)

PYLABHUB_SCHEMA_BEGIN(TxAPITestMessage)
    PYLABHUB_SCHEMA_MEMBER(sequence)
    PYLABHUB_SCHEMA_MEMBER(value)
    PYLABHUB_SCHEMA_MEMBER(payload)
PYLABHUB_SCHEMA_END(TxAPITestMessage)

namespace pylabhub::tests::worker::transaction_api
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Test: Basic with_transaction Success
// ============================================================================

/**
 * @test with_write_transaction_success
 * @brief Verify basic with_transaction write and read
 * 
 * @test_strategy
 * Producer:
 *   1. Create producer with dual-schema (TxAPITestFlexZone, TxAPITestMessage)
 *   2. Call producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>()
 *   3. Write data to slot via ctx.slots()
 *   4. Verify transaction completes successfully
 * 
 * Consumer:
 *   1. Attach consumer with matching dual-schema
 *   2. Call consumer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>()
 *   3. Read and verify data
 * 
 * Expected: Both transactions succeed, data matches
 * 
 * @test_level C++ Schema-Aware (v1.0.0 RAII)
 * @coverage Normal usage
 */
int with_write_transaction_success()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxAPIv1");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70001;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flex_zone_size = sizeof(TxAPITestFlexZone); // rounded up to PAGE_ALIGNMENT at creation

            // Create producer with dual-schema
            auto producer = create_datablock_producer<TxAPITestFlexZone, TxAPITestMessage>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            
            // Create consumer with matching dual-schema
            auto consumer = find_datablock_consumer<TxAPITestFlexZone, TxAPITestMessage>(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write using v1.0.0 with_transaction API
            const char test_payload[] = "Transaction API v1.0.0 success";
            uint64_t written_seq = 0;
            
            producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                5000ms,
                [&](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    // Access flex zone
                    auto zone = ctx.flexzone();
                    zone.get().transaction_count.store(1, std::memory_order_relaxed);
                    zone.get().test_flag.store(true, std::memory_order_relaxed);
                    
                    // Write one slot
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (!slot.is_ok())
                        {
                            ADD_FAILURE() << "Failed to acquire slot";
                            break;
                        }
                        
                        auto &msg = slot.content();
                        written_seq = 12345;
                        msg.get().sequence = written_seq;
                        msg.get().value = 999;
                        std::memcpy(msg.get().payload, test_payload, sizeof(test_payload));
                        
                        break; // Write only one slot
                    }
                });

            // Read using v1.0.0 with_transaction API
            consumer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                5000ms,
                [&](ReadTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    // Verify flex zone
                    auto zone = ctx.flexzone();
                    EXPECT_EQ(zone.get().transaction_count.load(), 1u);
                    EXPECT_TRUE(zone.get().test_flag.load());
                    
                    // Read one slot
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (!slot.is_ok())
                        {
                            ADD_FAILURE() << "Failed to acquire slot";
                            break;
                        }
                        
                        auto &msg = slot.content();
                        EXPECT_EQ(msg.get().sequence, written_seq);
                        EXPECT_EQ(msg.get().value, 999u);
                        EXPECT_EQ(std::memcmp(msg.get().payload, test_payload, sizeof(test_payload)), 0);
                        
                        break; // Read only one slot
                    }
                });

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] with_write_transaction_success ok\n");
        },
        "with_write_transaction_success", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Test: Transaction Timeout
// ============================================================================

/**
 * @test with_write_transaction_timeout
 * @brief Verify timeout behavior when slots unavailable
 * 
 * @test_strategy
 * Setup: Single-slot ring buffer
 * Consumer: Hold slot (blocks producer)
 * Producer: Attempt with_transaction with short timeout
 * Expected: Producer transaction times out (exception or early exit)
 * Cleanup: Release consumer slot, verify producer can proceed
 * 
 * @test_level C++ RAII
 * @coverage Error path (timeout)
 */
int with_write_transaction_timeout()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxTimeoutv1");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
            config.shared_secret = 70002;
            config.ring_buffer_capacity = 1; // Only one slot
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flex_zone_size = sizeof(TxAPITestFlexZone); // rounded up to PAGE_ALIGNMENT at creation

            auto producer = create_datablock_producer<TxAPITestFlexZone, TxAPITestMessage>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            
            auto consumer = find_datablock_consumer<TxAPITestFlexZone, TxAPITestMessage>(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write and commit one slot
            producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                5000ms,
                [](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (slot.is_ok())
                        {
                            auto &msg = slot.content();
                            msg.get().value = 42;
                            break;
                        }
                    }
                });

            // Consumer acquires and holds the slot (blocks producer)
            {
                auto read_handle = consumer->acquire_consume_slot(5000);
                ASSERT_NE(read_handle, nullptr) << "Consumer must acquire slot";

                // Producer tries with short timeout - should fail/timeout
                bool timeout_occurred = false;
                producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                    100ms, // Short timeout
                    [&timeout_occurred](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                    {
                        for (auto &slot : ctx.slots(50ms))
                        {
                            if (!slot.is_ok())
                            {
                                // Expected - no slot available
                                timeout_occurred = true;
                                break;
                            }
                            
                            // Should not reach here
                            ADD_FAILURE() << "Slot should not be available (consumer holds it)";
                            break;
                        }
                    });
                
                EXPECT_TRUE(timeout_occurred) << "Expected timeout when slot is held by consumer";
                // read_handle destroyed here
            }

            // Now consumer released slot - producer should succeed
            producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                1000ms,
                [](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    for (auto &slot : ctx.slots(50ms))
                    {
                        EXPECT_TRUE(slot.is_ok()) << "Slot should be available after consumer released";
                        break;
                    }
                });

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] with_write_transaction_timeout ok\n");
        },
        "with_write_transaction_timeout", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Test: Exception Safety (Write)
// ============================================================================

/**
 * @test WriteTransactionGuard_exception_releases_slot
 * @brief Verify exception in transaction triggers automatic slot cleanup
 * 
 * @test_strategy
 * Producer:
 *   1. Start with_transaction
 *   2. Acquire slot via ctx.slots()
 *   3. Throw exception before commit
 *   4. Verify slot is automatically released (RAII cleanup)
 *   5. Verify subsequent acquire succeeds
 * 
 * Expected: Slot is automatically released, no resource leak
 * 
 * @test_level C++ RAII
 * @coverage Error path (exception safety)
 */
int WriteTransactionGuard_exception_releases_slot()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxExv1");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70003;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flex_zone_size = sizeof(TxAPITestFlexZone); // rounded up to PAGE_ALIGNMENT at creation

            auto producer = create_datablock_producer<TxAPITestFlexZone, TxAPITestMessage>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            // Test exception safety
            bool exception_caught = false;
            try
            {
                producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                    5000ms,
                    [](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                    {
                        for (auto &slot : ctx.slots(50ms))
                        {
                            if (slot.is_ok())
                            {
                                // Intentionally throw before commit
                                throw std::runtime_error("Intentional exception - testing cleanup");
                            }
                        }
                    });
            }
            catch (const std::exception &)
            {
                exception_caught = true;
            }
            ASSERT_TRUE(exception_caught) << "Expected exception to propagate";

            // Slot must have been released by RAII cleanup
            // Verify by acquiring again
            bool slot_available = false;
            producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                1000ms,
                [&slot_available](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (slot.is_ok())
                        {
                            slot_available = true;
                            break;
                        }
                    }
                });
            
            EXPECT_TRUE(slot_available) << "Slot should be available after exception cleanup";

            producer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] WriteTransactionGuard_exception_releases_slot ok\n");
        },
        "WriteTransactionGuard_exception_releases_slot", logger_module(), crypto_module(),
        hub_module());
}

// ============================================================================
// Test: Exception Safety (Read)
// ============================================================================

/**
 * @test ReadTransactionGuard_exception_releases_slot
 * @brief Verify exception in read transaction triggers cleanup
 * 
 * @test_strategy
 * Producer: Write one slot
 * Consumer:
 *   1. Start with_transaction
 *   2. Acquire slot via ctx.slots()
 *   3. Throw exception before complete
 *   4. Verify slot is automatically released
 *   5. Verify subsequent acquire succeeds
 * 
 * @test_level C++ RAII
 * @coverage Error path (exception safety, read side)
 */
int ReadTransactionGuard_exception_releases_slot()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxReadExv1");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70004;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flex_zone_size = sizeof(TxAPITestFlexZone); // rounded up to PAGE_ALIGNMENT at creation

            auto producer = create_datablock_producer<TxAPITestFlexZone, TxAPITestMessage>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            
            auto consumer = find_datablock_consumer<TxAPITestFlexZone, TxAPITestMessage>(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Producer writes one slot
            producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                5000ms,
                [](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (slot.is_ok())
                        {
                            auto &msg = slot.content();
                            msg.get().value = 42;
                            break;
                        }
                    }
                });

            // Consumer reads with exception
            bool exception_caught = false;
            try
            {
                consumer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                    5000ms,
                    [](ReadTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                    {
                        for (auto &slot : ctx.slots(50ms))
                        {
                            if (slot.is_ok())
                            {
                                // Intentionally throw before completion
                                throw std::runtime_error("Intentional read exception");
                            }
                        }
                    });
            }
            catch (const std::exception &)
            {
                exception_caught = true;
            }
            ASSERT_TRUE(exception_caught) << "Expected exception to propagate";

            // Slot must have been released - verify by reading again
            bool slot_read_ok = false;
            consumer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                1000ms,
                [&slot_read_ok](ReadTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (slot.is_ok())
                        {
                            auto &msg = slot.content();
                            EXPECT_EQ(msg.get().value, 42u);
                            slot_read_ok = true;
                            break;
                        }
                    }
                });
            
            EXPECT_TRUE(slot_read_ok) << "Slot should be readable after exception cleanup";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] ReadTransactionGuard_exception_releases_slot ok\n");
        },
        "ReadTransactionGuard_exception_releases_slot", logger_module(), crypto_module(),
        hub_module());
}

// ============================================================================
// Test: Typed Access (FlexZone + DataBlock)
// ============================================================================

/**
 * @test with_typed_write_read_succeeds
 * @brief Verify typed access to both FlexZone and DataBlock
 * 
 * @test_strategy
 * Producer:
 *   1. Access ctx.flexzone() → TxAPITestFlexZone
 *   2. Access ctx.slots() → TxAPITestMessage
 *   3. Write typed data to both
 * 
 * Consumer:
 *   1. Read ctx.flexzone() → verify TxAPITestFlexZone data
 *   2. Read ctx.slots() → verify TxAPITestMessage data
 * 
 * Expected: Both FlexZone and DataBlock typed access work correctly
 * 
 * @test_level C++ Schema-Aware (v1.0.0)
 * @coverage Normal usage (typed access)
 */
int with_typed_write_read_succeeds()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxTypedv1");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70005;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flex_zone_size = sizeof(TxAPITestFlexZone); // rounded up to PAGE_ALIGNMENT at creation

            auto producer = create_datablock_producer<TxAPITestFlexZone, TxAPITestMessage>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            
            auto consumer = find_datablock_consumer<TxAPITestFlexZone, TxAPITestMessage>(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write with typed access
            const uint64_t expected_seq = 12345;
            const uint32_t expected_value = 999;
            const uint32_t expected_count = 42;
            
            producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                5000ms,
                [&](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    // Access FlexZone (shared metadata)
                    auto zone = ctx.flexzone();
                    zone.get().transaction_count.store(expected_count, std::memory_order_relaxed);
                    zone.get().test_flag.store(true, std::memory_order_relaxed);
                    
                    // Access DataBlock (per-slot message)
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (slot.is_ok())
                        {
                            auto &msg = slot.content();
                            msg.get().sequence = expected_seq;
                            msg.get().value = expected_value;
                            strcpy(msg.get().payload, "Typed access test");
                            break;
                        }
                    }
                });

            // Read with typed access
            consumer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                5000ms,
                [&](ReadTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    // Verify FlexZone data
                    auto zone = ctx.flexzone();
                    EXPECT_EQ(zone.get().transaction_count.load(), expected_count);
                    EXPECT_TRUE(zone.get().test_flag.load());
                    
                    // Verify DataBlock data
                    for (auto &slot : ctx.slots(50ms))
                    {
                        if (slot.is_ok())
                        {
                            auto &msg = slot.content();
                            EXPECT_EQ(msg.get().sequence, expected_seq);
                            EXPECT_EQ(msg.get().value, expected_value);
                            EXPECT_STREQ(msg.get().payload, "Typed access test");
                            break;
                        }
                    }
                });

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] with_typed_write_read_succeeds ok\n");
        },
        "with_typed_write_read_succeeds", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Test: Non-Terminating Iterator
// ============================================================================

/**
 * @test raii_slot_iterator_roundtrip
 * @brief Verify non-terminating ctx.slots() iterator for write/read roundtrip
 *
 * @test_strategy
 * Producer: Write 3 slots sequentially via ctx.slots()
 * Consumer: Read all 3 via ctx.slots() non-terminating iterator
 * Expected: Iterator yields Result<SlotRef, Error>; values match write order
 *
 * @test_level C++ RAII
 * @coverage Normal usage (non-terminating iterator)
 */
int raii_slot_iterator_roundtrip()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxIterv1");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
            config.shared_secret = 70006;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flex_zone_size = sizeof(TxAPITestFlexZone); // rounded up to PAGE_ALIGNMENT at creation

            auto producer = create_datablock_producer<TxAPITestFlexZone, TxAPITestMessage>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            
            auto consumer = find_datablock_consumer<TxAPITestFlexZone, TxAPITestMessage>(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write 3 slots
            for (int i = 0; i < 3; ++i)
            {
                producer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                    5000ms,
                    [i](WriteTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                    {
                        for (auto &slot : ctx.slots(50ms))
                        {
                            if (slot.is_ok())
                            {
                                auto &msg = slot.content();
                                msg.get().value = static_cast<uint32_t>(i);
                                break;
                            }
                        }
                    });
            }

            // Read 3 slots using non-terminating iterator
            std::vector<uint32_t> read_values;
            consumer->with_transaction<TxAPITestFlexZone, TxAPITestMessage>(
                5000ms,
                [&read_values](ReadTransactionContext<TxAPITestFlexZone, TxAPITestMessage> &ctx)
                {
                    for (auto &slot : ctx.slots(2000ms))
                    {
                        if (!slot.is_ok())
                        {
                            // Timeout or error - stop reading
                            break;
                        }
                        
                        auto &msg = slot.content();
                        read_values.push_back(msg.get().value);
                        
                        if (read_values.size() >= 3)
                        {
                            break; // Read enough
                        }
                    }
                });

            EXPECT_EQ(read_values.size(), 3u);
            EXPECT_EQ(read_values[0], 0u) << "First slot should be 0";
            EXPECT_EQ(read_values[1], 1u) << "Second slot should be 1";
            EXPECT_EQ(read_values[2], 2u) << "Third slot should be 2";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] raii_slot_iterator_roundtrip ok\n");
        },
        "raii_slot_iterator_roundtrip", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::transaction_api

namespace
{
struct TransactionAPIWorkerRegistrar
{
    TransactionAPIWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "transaction_api")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::transaction_api;
                if (scenario == "with_write_transaction_success")
                    return with_write_transaction_success();
                if (scenario == "with_write_transaction_timeout")
                    return with_write_transaction_timeout();
                if (scenario == "WriteTransactionGuard_exception_releases_slot")
                    return WriteTransactionGuard_exception_releases_slot();
                if (scenario == "ReadTransactionGuard_exception_releases_slot")
                    return ReadTransactionGuard_exception_releases_slot();
                if (scenario == "with_typed_write_read_succeeds")
                    return with_typed_write_read_succeeds();
                if (scenario == "raii_slot_iterator_roundtrip")
                    return raii_slot_iterator_roundtrip();
                fmt::print(stderr, "ERROR: Unknown transaction_api scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static TransactionAPIWorkerRegistrar g_transaction_api_registrar;
} // namespace
