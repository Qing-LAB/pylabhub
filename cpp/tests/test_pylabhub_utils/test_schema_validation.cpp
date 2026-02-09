#include "shared_test_helpers.h"
#include "plh_datahub.hpp"  // Includes schema_blds.hpp, data_block.hpp, message_hub.hpp
#include <gtest/gtest.h>
#include <string>

using namespace pylabhub::hub;
using namespace pylabhub::schema;

struct TestSchemaV1 {
    int a;
    char b;
};

PYLABHUB_SCHEMA_BEGIN(TestSchemaV1)
    PYLABHUB_SCHEMA_MEMBER(a)
    PYLABHUB_SCHEMA_MEMBER(b)
PYLABHUB_SCHEMA_END(TestSchemaV1)

struct TestSchemaV2 {
    int a;
    double c;
};

PYLABHUB_SCHEMA_BEGIN(TestSchemaV2)
    PYLABHUB_SCHEMA_MEMBER(a)
    PYLABHUB_SCHEMA_MEMBER(c)
PYLABHUB_SCHEMA_END(TestSchemaV2)

class SchemaValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        hub = MessageHub::get_instance();
        ASSERT_NE(hub, nullptr);
    }

    void TearDown() override {
    }

    MessageHub* hub;
    const std::string shm_name = "schema_val_test_db";
    const uint64_t shared_secret = 67890;
};

TEST_F(SchemaValidationTest, ConsumerConnectsWithMatchingSchema) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 0;
    config.ring_buffer_capacity = 1;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    TestSchemaV1 schema_v1{42, 'x'};

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config, schema_v1);
    ASSERT_NE(producer, nullptr);

    auto consumer = find_datablock_consumer(*hub, shm_name, shared_secret, config, schema_v1);
    ASSERT_NE(consumer, nullptr);
}

TEST_F(SchemaValidationTest, ConsumerFailsToConnectWithMismatchedSchema) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 0;
    config.ring_buffer_capacity = 1;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    TestSchemaV1 schema_v1{42, 'x'};
    TestSchemaV2 schema_v2{42, 3.14};

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config, schema_v1);
    ASSERT_NE(producer, nullptr);

    auto consumer = find_datablock_consumer(*hub, shm_name, shared_secret, config, schema_v2);
    ASSERT_EQ(consumer, nullptr);
}
