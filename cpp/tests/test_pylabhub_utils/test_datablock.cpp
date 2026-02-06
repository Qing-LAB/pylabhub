#include "gtest/gtest.h"
#include "plh_service.hpp"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"

/**
 * @brief Tests for the DataBlock factory functions.
 *
 * These tests cover the initial skeleton implementation of the DataBlock module.
 * As the features are implemented, these tests will be expanded and updated.
 */
class DataBlockTest : public ::testing::Test
{
};

TEST_F(DataBlockTest, FactoryFunctionsCreateValidObjects)
{
    // A disconnected hub is sufficient for this test since the functions are placeholders.
    pylabhub::hub::MessageHub hub;
    pylabhub::hub::DataBlockConfig config{};
    config.shared_secret = 123;
    config.structured_buffer_size = 1024;
    config.flexible_zone_size = 512;
    config.ring_buffer_capacity = 0;

    // These tests now confirm that valid objects are returned when creation succeeds.
    // Error conditions (e.g., shm_open failure) should be tested in separate tests.
    std::unique_ptr<pylabhub::hub::IDataBlockProducer> producer =
        pylabhub::hub::create_datablock_producer(hub, "test_channel_producer",
                                                 pylabhub::hub::DataBlockPolicy::Single, config);
    ASSERT_NE(nullptr, producer);

    // To successfully find a consumer, a producer must have already created the shared memory.
    // The shared secret must match.
    std::unique_ptr<pylabhub::hub::IDataBlockConsumer> consumer =
        pylabhub::hub::find_datablock_consumer(hub, "test_channel_producer", 123);
    ASSERT_NE(nullptr, consumer);
}
