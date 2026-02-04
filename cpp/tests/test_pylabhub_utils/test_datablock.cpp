#include "gtest/gtest.h"
#include "plh_service.hpp"
#include "utils/DataBlock.hpp"
#include "utils/MessageHub.hpp"

/**
 * @brief Tests for the DataBlock factory functions.
 *
 * These tests cover the initial skeleton implementation of the DataBlock module.
 * As the features are implemented, these tests will be expanded and updated.
 */
class DataBlockTest : public ::testing::Test
{
};

TEST_F(DataBlockTest, FactoryFunctionsReturnNullptrAsNotImplemented)
{
    // A disconnected hub is sufficient for this test since the functions are placeholders.
    pylabhub::hub::MessageHub hub;
    pylabhub::hub::DataBlockConfig config{};
    config.shared_secret = 123;
    config.structured_buffer_size = 1024;
    config.flexible_zone_size = 512;
    config.ring_buffer_capacity = 0;

    // These tests confirm the current skeleton implementation returns nullptr.
    // This serves as a baseline and will be updated when the feature is implemented.
    ASSERT_EQ(nullptr, pylabhub::hub::create_datablock_producer(
                           hub, "test_channel", pylabhub::hub::DataBlockPolicy::Single, config));
    ASSERT_EQ(nullptr, pylabhub::hub::find_datablock_consumer(hub, "test_channel", 12345));
}
