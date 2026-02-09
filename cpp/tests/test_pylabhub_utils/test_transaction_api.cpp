#include "shared_test_helpers.h"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <string>

using namespace pylabhub::hub;

class TransactionApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        hub = MessageHub::get_instance();
        ASSERT_NE(hub, nullptr);
    }

    void TearDown() override {
        // Clean up any created datablocks if necessary
    }

    MessageHub* hub;
    const std::string shm_name = "txn_api_test_db";
    const uint64_t shared_secret = 12345;
};

TEST_F(TransactionApiTest, WriteTransactionGuardWorks) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 1024;
    config.ring_buffer_capacity = 4;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config);
    ASSERT_NE(producer, nullptr);

    std::string test_data = "Hello, WriteTransactionGuard!";

    {
        WriteTransactionGuard guard(*producer, 1000);
        ASSERT_TRUE(guard);
        auto& slot = guard.slot();
        ASSERT_TRUE(slot.write(test_data.c_str(), test_data.size()));
        slot.commit(test_data.size());
        guard.commit();
    }

    auto consumer = find_datablock_consumer(*hub, shm_name, shared_secret);
    ASSERT_NE(consumer, nullptr);

    auto iterator = consumer->slot_iterator();
    auto result = iterator.try_next(1000);
    ASSERT_TRUE(result.ok);

    std::vector<char> buffer(test_data.size());
    result.next.read(buffer.data(), buffer.size());
    std::string read_data(buffer.begin(), buffer.end());

    ASSERT_EQ(read_data, test_data);
}

TEST_F(TransactionApiTest, WithWriteTransactionLambdaWorks) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 1024;
    config.ring_buffer_capacity = 4;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config);
    ASSERT_NE(producer, nullptr);

    std::string test_data = "Hello, with_write_transaction!";

    auto write_fn = [&](SlotWriteHandle& slot) {
        slot.write(test_data.c_str(), test_data.size());
        slot.commit(test_data.size());
    };

    with_write_transaction(*producer, 1000, write_fn);

    auto consumer = find_datablock_consumer(*hub, shm_name, shared_secret);
    ASSERT_NE(consumer, nullptr);
    
    ReadTransactionGuard reader(*consumer, 0, 1000);
    ASSERT_TRUE(reader);
    
    std::vector<char> buffer(test_data.size());
    reader.slot().read(buffer.data(), buffer.size());
    std::string read_data(buffer.begin(), buffer.end());

    ASSERT_EQ(read_data, test_data);
}

TEST_F(TransactionApiTest, WithNextSlotWorks) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 1024;
    config.ring_buffer_capacity = 4;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config);
    ASSERT_NE(producer, nullptr);

    std::string test_data = "Hello, with_next_slot!";

    {
        WriteTransactionGuard guard(*producer, 1000);
        guard.slot().write(test_data.c_str(), test_data.size());
        guard.slot().commit(test_data.size());
        guard.commit();
    }

    auto consumer = find_datablock_consumer(*hub, shm_name, shared_secret);
    ASSERT_NE(consumer, nullptr);

    auto iterator = consumer->slot_iterator();
    
    auto result = with_next_slot(iterator, 1000, [&](const SlotConsumeHandle& slot) {
        std::vector<char> buffer(test_data.size());
        slot.read(buffer.data(), buffer.size());
        return std::string(buffer.begin(), buffer.end());
    });

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value(), test_data);
}
