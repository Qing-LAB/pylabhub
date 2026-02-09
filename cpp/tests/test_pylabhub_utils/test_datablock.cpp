#include "gtest/gtest.h"
#include "plh_service.hpp"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include <functional> // For std::function, std::invoke
#include <chrono>     // For std::chrono
#include <stdexcept>  // For std::runtime_error

/**
 * @brief Tests for the DataBlock factory functions.
 *
 * These tests cover the initial skeleton implementation of the DataBlock module.
 * As the features are implemented, these tests will be expanded and updated.
 */
class DataBlockTest : public ::testing::Test
{
};

/**
 * @brief Tests for the DataBlock transaction API (Layer 2).
 */
class DataBlockTransactionApiTest : public ::testing::Test
{
  protected:
    pylabhub::hub::MessageHub hub;
    pylabhub::hub::DataBlockConfig config;
    std::unique_ptr<pylabhub::hub::DataBlockProducer> producer;
    std::unique_ptr<pylabhub::hub::DataBlockConsumer> consumer;
    std::string channel_name = "test_transaction_channel";
    uint64_t shared_secret = 456;

    void SetUp() override
    {
        config.shared_secret = shared_secret;
        config.flexible_zone_size = 512;
        config.unit_block_size = pylabhub::hub::DataBlockUnitSize::Size4K;
        config.ring_buffer_capacity = 1; // Single policy for simplicity

        // Create producer
        producer = pylabhub::hub::create_datablock_producer(
            hub, channel_name, pylabhub::hub::DataBlockPolicy::Single, config);
        ASSERT_NE(nullptr, producer) << "Producer creation failed.";

        // Create consumer
        consumer = pylabhub::hub::find_datablock_consumer(hub, channel_name, shared_secret);
        ASSERT_NE(nullptr, consumer) << "Consumer creation failed.";
    }

    void TearDown() override
    {
        // Producer and consumer unique_ptrs will handle destruction
    }
};

TEST_F(DataBlockTransactionApiTest, WithWriteTransaction_SuccessfulWriteAndCommit)
{
    struct TestData {
        uint64_t timestamp;
        uint32_t value;
    };
    TestData test_data = {std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::high_resolution_clock::now().time_since_epoch())
                              .count(),
                          12345};

    // Use with_write_transaction
    ASSERT_NO_THROW(pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            auto span = slot.buffer_span();
            ASSERT_GE(span.size(), sizeof(TestData));
            std::memcpy(span.data(), &test_data, sizeof(TestData));
            slot.commit(sizeof(TestData));
        }));

    // Verify data was committed by consumer
    auto consume_handle = consumer->acquire_consume_slot(100);
    ASSERT_NE(nullptr, consume_handle);
    ASSERT_EQ(0, consume_handle->slot_index()); // First slot
    ASSERT_EQ(0, consume_handle->slot_id());    // First slot ID

    TestData received_data;
    consume_handle->read(&received_data, sizeof(TestData));
    ASSERT_EQ(test_data.timestamp, received_data.timestamp);
    ASSERT_EQ(test_data.value, received_data.value);
}

TEST_F(DataBlockTransactionApiTest, WithWriteTransaction_ExceptionDuringLambda_DoesNotCommit)
{
    struct TestData {
        uint32_t value;
    };
    TestData test_data = {54321};

    // Producer's initial commit_index should be INVALID_SLOT_ID
    ASSERT_EQ(std::numeric_limits<uint64_t>::max(), producer->pImpl->dataBlock->header()->commit_index.load());

    // Use with_write_transaction, but throw an exception inside the lambda
    ASSERT_THROW(
        pylabhub::hub::with_write_transaction(*producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            auto span = slot.buffer_span();
            std::memcpy(span.data(), &test_data, sizeof(TestData));
            slot.commit(sizeof(TestData)); // Commit is called, but should be rolled back
            throw std::runtime_error("Simulated error during write");
        }),
        std::runtime_error);

    // Verify that the commit_index did not advance (data was not committed)
    ASSERT_EQ(std::numeric_limits<uint64_t>::max(), producer->pImpl->dataBlock->header()->commit_index.load());

    // Try to acquire a consume slot; it should return nullptr because nothing was committed
    auto consume_handle = consumer->acquire_consume_slot(100);
    ASSERT_EQ(nullptr, consume_handle);
}

TEST_F(DataBlockTransactionApiTest, WithWriteTransaction_TimeoutOnAcquisition)
{
    // Configure producer for a ring buffer with capacity 1
    // to easily simulate a full buffer and timeout on acquire_write_slot.
    producer->pImpl->dataBlock->header()->ring_buffer_capacity = 1;

    // Write one slot to fill the buffer
    pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) { slot.commit(10); });

    // Now attempt to write again with a short timeout.
    // It should throw an exception because the buffer is full (capacity 1, one written, no consumer read yet)
    // and acquire_write_slot will timeout.
    ASSERT_THROW(
        pylabhub::hub::with_write_transaction(*producer, 10, [&](pylabhub::hub::SlotWriteHandle &slot) {
            (void)slot; // Suppress unused warning
        }),
        std::runtime_error);
}

TEST_F(DataBlockTransactionApiTest, WriteTransactionGuard_SuccessfulAcquisitionAndExplicitCommit)
{
    struct TestData {
        uint32_t value;
    };
    TestData test_data = {67890};

    pylabhub::hub::WriteTransactionGuard guard(*producer, 100);
    ASSERT_TRUE(static_cast<bool>(guard)); // Check explicit bool conversion

    auto &slot = guard.slot();
    auto span = slot.buffer_span();
    ASSERT_GE(span.size(), sizeof(TestData));
    std::memcpy(span.data(), &test_data, sizeof(TestData));
    slot.commit(sizeof(TestData)); // Explicit commit by user
    guard.commit();               // Mark guard as committed

    // Guard goes out of scope and releases the slot.
    // Verify data was committed by consumer
    auto consume_handle = consumer->acquire_consume_slot(100);
    ASSERT_NE(nullptr, consume_handle);
    ASSERT_EQ(0, consume_handle->slot_index()); // First slot
    ASSERT_EQ(0, consume_handle->slot_id());    // First slot ID

    TestData received_data;
    consume_handle->read(&received_data, sizeof(TestData));
    ASSERT_EQ(test_data.value, received_data.value);
}

TEST_F(DataBlockTransactionApiTest, WriteTransactionGuard_ExplicitAbort)
{
    struct TestData {
        uint32_t value;
    };
    TestData test_data = {11223};

    { // Scope for the guard
        pylabhub::hub::WriteTransactionGuard guard(*producer, 100);
        ASSERT_TRUE(static_cast<bool>(guard));

        auto &slot = guard.slot();
        auto span = slot.buffer_span();
        ASSERT_GE(span.size(), sizeof(TestData));
        std::memcpy(span.data(), &test_data, sizeof(TestData));
        slot.commit(sizeof(TestData)); // Commit is called, but should be aborted
        guard.abort();                 // Explicitly abort
    } // Guard goes out of scope and releases, but should not commit

    // Verify that the commit_index did not advance (data was not committed)
    ASSERT_EQ(std::numeric_limits<uint64_t>::max(), producer->pImpl->dataBlock->header()->commit_index.load());

    // Try to acquire a consume slot; it should return nullptr because nothing was committed
    auto consume_handle = consumer->acquire_consume_slot(100);
    ASSERT_EQ(nullptr, consume_handle);
}

TEST_F(DataBlockTransactionApiTest, WriteTransactionGuard_ExceptionDuringUsage)
{
    struct TestData {
        uint32_t value;
    };
    TestData test_data = {44556};

    // Producer's initial commit_index should be INVALID_SLOT_ID
    ASSERT_EQ(std::numeric_limits<uint64_t>::max(), producer->pImpl->dataBlock->header()->commit_index.load());

    ASSERT_THROW(
        { // Scope for the guard and exception
            pylabhub::hub::WriteTransactionGuard guard(*producer, 100);
            ASSERT_TRUE(static_cast<bool>(guard));

            auto &slot = guard.slot();
            auto span = slot.buffer_span();
            std::memcpy(span.data(), &test_data, sizeof(TestData));
            slot.commit(sizeof(TestData)); // Commit is called, but should be rolled back

            throw std::runtime_error("Simulated error during guard usage");
        },
        std::runtime_error);

    // Verify that the commit_index did not advance (data was not committed)
    ASSERT_EQ(std::numeric_limits<uint64_t>::max(), producer->pImpl->dataBlock->header()->commit_index.load());

    // Try to acquire a consume slot; it should return nullptr because nothing was committed
    auto consume_handle = consumer->acquire_consume_slot(100);
    ASSERT_EQ(nullptr, consume_handle);
}

TEST_F(DataBlockTransactionApiTest, WriteTransactionGuard_MoveSemantics)
{
    struct TestData {
        uint32_t value;
    };
    TestData test_data_original = {77889};
    TestData test_data_moved = {99887};

    // Original guard
    pylabhub::hub::WriteTransactionGuard original_guard(*producer, 100);
    ASSERT_TRUE(static_cast<bool>(original_guard));
    auto &original_slot = original_guard.slot();
    std::memcpy(original_slot.buffer_span().data(), &test_data_original, sizeof(TestData));
    original_slot.commit(sizeof(TestData));
    original_guard.commit();

    // Move construction
    pylabhub::hub::WriteTransactionGuard moved_guard = std::move(original_guard);
    ASSERT_FALSE(static_cast<bool>(original_guard)); // Original should now be invalid
    ASSERT_TRUE(static_cast<bool>(moved_guard));      // Moved should be valid

    // Perform operations with moved guard
    auto &moved_slot = moved_guard.slot();
    std::memcpy(moved_slot.buffer_span().data(), &test_data_moved, sizeof(TestData));
    moved_slot.commit(sizeof(TestData));
    moved_guard.commit();

    // After moved_guard goes out of scope, verify data
    // First slot should contain test_data_original
    // Second slot (overwriting first due to Single policy) should contain test_data_moved

    auto consume_handle = consumer->acquire_consume_slot(100);
    ASSERT_NE(nullptr, consume_handle);
    ASSERT_EQ(0, consume_handle->slot_id());

    TestData received_data;
    consume_handle->read(&received_data, sizeof(TestData));
    ASSERT_EQ(test_data_moved.value, received_data.value);
}

TEST_F(DataBlockTransactionApiTest, WithReadTransaction_SuccessfulRead)
{
    // First, producer writes some data
    struct TestData {
        uint32_t id;
        uint32_t value;
    };
    TestData written_data = {1, 100};
    pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            std::memcpy(slot.buffer_span().data(), &written_data, sizeof(TestData));
            slot.commit(sizeof(TestData));
        });

    // Now, consumer reads it using with_read_transaction
    ASSERT_NO_THROW(pylabhub::hub::with_read_transaction(
        *consumer, 0, 100, [&](const pylabhub::hub::SlotConsumeHandle &slot) {
            TestData read_data;
            slot.read(&read_data, sizeof(TestData));
            ASSERT_EQ(written_data.id, read_data.id);
            ASSERT_EQ(written_data.value, read_data.value);
        }));
}

TEST_F(DataBlockTransactionApiTest, WithReadTransaction_ExceptionDuringLambda_ReleasesSlot)
{
    // First, producer writes some data
    struct TestData {
        uint32_t value;
    };
    TestData written_data = {200};
    pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            std::memcpy(slot.buffer_span().data(), &written_data, sizeof(TestData));
            slot.commit(sizeof(TestData));
        });

    // Consumer attempts to read, but lambda throws
    ASSERT_THROW(pylabhub::hub::with_read_transaction(
                     *consumer, 0, 100,
                     [&](const pylabhub::hub::SlotConsumeHandle &slot) {
                         // Attempt to read, but throw before releasing
                         TestData read_data;
                         slot.read(&read_data, sizeof(TestData));
                         throw std::runtime_error("Simulated error during read processing");
                     }),
                 std::runtime_error);

    // Verify the slot is still accessible (released by the guard)
    auto next_slot = consumer->acquire_consume_slot(100);
    ASSERT_NE(nullptr, next_slot); // Should be able to acquire again from a new instance
    ASSERT_EQ(0, next_slot->slot_id());
}

TEST_F(DataBlockTransactionApiTest, WithReadTransaction_TimeoutOnAcquisition)
{
    // No data written by producer, so consumer acquire_consume_slot will timeout
    ASSERT_THROW(
        pylabhub::hub::with_read_transaction(
            *consumer, 0, 10, // Short timeout for slot_id 0
            [&](const pylabhub::hub::SlotConsumeHandle &slot) { (void)slot; }),
        std::runtime_error);
}

TEST_F(DataBlockTransactionApiTest, WithNextSlot_SuccessfulIterationAndRead)
{
    // Producer writes multiple slots
    struct TestData {
        uint32_t id;
    };
    for (uint32_t i = 0; i < 3; ++i)
    {
        TestData written_data = {i};
        pylabhub::hub::with_write_transaction(
            *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
                std::memcpy(slot.buffer_span().data(), &written_data, sizeof(TestData));
                slot.commit(sizeof(TestData));
            });
    }

    // Consumer iterates using with_next_slot
    pylabhub::hub::DataBlockSlotIterator iterator = consumer->slot_iterator();
    for (uint32_t i = 0; i < 3; ++i)
    {
        bool processed = false;
        std::optional<std::monostate> result_opt = pylabhub::hub::with_next_slot(
            iterator, 100, [&](const pylabhub::hub::SlotConsumeHandle &slot) {
                TestData read_data;
                slot.read(&read_data, sizeof(TestData));
                ASSERT_EQ(i, read_data.id);
                processed = true;
            });
        ASSERT_TRUE(result_opt.has_value()); // Should have processed a slot
        ASSERT_TRUE(processed);
    }

    // After consuming all, next call should return nullopt (no more data)
    std::optional<std::monostate> result_opt_empty = pylabhub::hub::with_next_slot(
        iterator, 10, [&](const pylabhub::hub::SlotConsumeHandle &slot) { (void)slot; });
    ASSERT_FALSE(result_opt_empty.has_value());
}

TEST_F(DataBlockTransactionApiTest, WithNextSlot_TimeoutWhenNoNewData)
{
    // Write one slot
    struct TestData {
        uint32_t id;
    };
    TestData written_data = {0};
    pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            std::memcpy(slot.buffer_span().data(), &written_data, sizeof(TestData));
            slot.commit(sizeof(TestData));
        });

    pylabhub::hub::DataBlockSlotIterator iterator = consumer->slot_iterator();
    // Consume the one slot
    pylabhub::hub::with_next_slot(iterator, 100,
                                  [&](const pylabhub::hub::SlotConsumeHandle &slot) { (void)slot; });

    // Now, try to get next slot with a short timeout; it should return nullopt
    // because no new data is available.
    std::optional<std::monostate> result_opt = pylabhub::hub::with_next_slot(
        iterator, 10, [&](const pylabhub::hub::SlotConsumeHandle &slot) { (void)slot; });
    ASSERT_FALSE(result_opt.has_value());
}

TEST_F(DataBlockTransactionApiTest, ReadTransactionGuard_SuccessfulAcquisitionAndRead)
{
    // First, producer writes some data
    struct TestData {
        uint32_t id;
        uint32_t value;
    };
    TestData written_data = {2, 200};
    pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            std::memcpy(slot.buffer_span().data(), &written_data, sizeof(TestData));
            slot.commit(sizeof(TestData));
        });

    // Now, consumer reads it using ReadTransactionGuard
    { // Scope for the guard
        pylabhub::hub::ReadTransactionGuard guard(*consumer, 0, 100);
        ASSERT_TRUE(static_cast<bool>(guard));

        const pylabhub::hub::SlotConsumeHandle &slot = guard.slot();
        TestData read_data;
        slot.read(&read_data, sizeof(TestData));
        ASSERT_EQ(written_data.id, read_data.id);
        ASSERT_EQ(written_data.value, read_data.value);
    } // Guard goes out of scope and releases the slot.

    // Verify the slot is still accessible (released by the guard)
    auto next_slot = consumer->acquire_consume_slot(100);
    ASSERT_NE(nullptr, next_slot); // Should be able to acquire again from a new instance
    ASSERT_EQ(0, next_slot->slot_id());
}

TEST_F(DataBlockTransactionApiTest, ReadTransactionGuard_ExceptionDuringUsage_ReleasesSlot)
{
    // First, producer writes some data
    struct TestData {
        uint32_t value;
    };
    TestData written_data = {300};
    pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            std::memcpy(slot.buffer_span().data(), &written_data, sizeof(TestData));
            slot.commit(sizeof(TestData));
        });

    // Consumer attempts to read, but throws an exception
    ASSERT_THROW(
        { // Scope for the guard and exception
            pylabhub::hub::ReadTransactionGuard guard(*consumer, 0, 100);
            ASSERT_TRUE(static_cast<bool>(guard));

            const pylabhub::hub::SlotConsumeHandle &slot = guard.slot();
            TestData read_data;
            slot.read(&read_data, sizeof(TestData));
            throw std::runtime_error("Simulated error during read processing");
        },
        std::runtime_error);

    // Verify the slot is still accessible (released by the guard)
    auto next_slot = consumer->acquire_consume_slot(100);
    ASSERT_NE(nullptr, next_slot); // Should be able to acquire again from a new instance
    ASSERT_EQ(0, next_slot->slot_id());
}

TEST_F(DataBlockTransactionApiTest, ReadTransactionGuard_MoveSemantics)
{
    // First, producer writes some data
    struct TestData {
        uint32_t value;
    };
    TestData written_data = {400};
    pylabhub::hub::with_write_transaction(
        *producer, 100, [&](pylabhub::hub::SlotWriteHandle &slot) {
            std::memcpy(slot.buffer_span().data(), &written_data, sizeof(TestData));
            slot.commit(sizeof(TestData));
        });

    // Original guard acquires a slot
    pylabhub::hub::ReadTransactionGuard original_guard(*consumer, 0, 100);
    ASSERT_TRUE(static_cast<bool>(original_guard));

    // Move construct a new guard
    pylabhub::hub::ReadTransactionGuard moved_guard = std::move(original_guard);
    ASSERT_FALSE(static_cast<bool>(original_guard)); // Original should now be invalid
    ASSERT_TRUE(static_cast<bool>(moved_guard));      // Moved should be valid

    // Access slot through moved guard
    const pylabhub::hub::SlotConsumeHandle &slot = moved_guard.slot();
    TestData read_data;
    slot.read(&read_data, sizeof(TestData));
    ASSERT_EQ(written_data.value, read_data.value);
    
    // Both guards go out of scope. The moved_guard's destructor will release.
}
