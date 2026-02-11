/**
 * @file test_schema_validation.cpp
 * @brief Layer 3 DataHub schema validation tests (producer/consumer schema match).
 *
 * Migrated from test_pylabhub_utils. Requires lifecycle (CryptoUtils, MessageHub)
 * and shared memory. Uses unique channel names and cleanup for isolation.
 */
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include "test_patterns.h"
#include <gtest/gtest.h>
#include <string>

using namespace pylabhub::hub;
using namespace pylabhub::schema;
using namespace pylabhub::tests;

struct TestSchemaV1
{
    int32_t a;
    char b;
};

PYLABHUB_SCHEMA_BEGIN(TestSchemaV1)
PYLABHUB_SCHEMA_MEMBER(a)
PYLABHUB_SCHEMA_MEMBER(b)
PYLABHUB_SCHEMA_END(TestSchemaV1)

struct TestSchemaV2
{
    int32_t a;
    double c;
};

PYLABHUB_SCHEMA_BEGIN(TestSchemaV2)
PYLABHUB_SCHEMA_MEMBER(a)
PYLABHUB_SCHEMA_MEMBER(c)
PYLABHUB_SCHEMA_END(TestSchemaV2)

class SchemaValidationTest : public LifecycleManagedTest
{
  protected:
    void SetUp() override
    {
        RegisterModule(pylabhub::crypto::GetLifecycleModule());
        RegisterModule(pylabhub::hub::GetLifecycleModule());
        LifecycleManagedTest::SetUp();
        channel_name = helper::make_test_channel_name("SchemaValidation");
    }

    void TearDown() override
    {
        producer.reset();
        consumer.reset();
        helper::cleanup_test_datablock(channel_name);
        LifecycleManagedTest::TearDown();
    }

    std::string channel_name;
    std::unique_ptr<pylabhub::hub::DataBlockProducer> producer;
    std::unique_ptr<pylabhub::hub::DataBlockConsumer> consumer;
    static constexpr uint64_t shared_secret = 67890;
};

TEST_F(SchemaValidationTest, ConsumerConnectsWithMatchingSchema)
{
    MessageHub &hub_ref = MessageHub::get_instance();
    DataBlockConfig config{};
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 0;
    config.ring_buffer_capacity = 1;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    TestSchemaV1 schema_v1{42, 'x'};

    producer.reset(create_datablock_producer(hub_ref, channel_name, DataBlockPolicy::RingBuffer,
                                             config, schema_v1));
    ASSERT_NE(producer, nullptr);

    consumer.reset(
        find_datablock_consumer(hub_ref, channel_name, shared_secret, config, schema_v1));
    ASSERT_NE(consumer, nullptr);
}

TEST_F(SchemaValidationTest, ConsumerFailsToConnectWithMismatchedSchema)
{
    MessageHub &hub_ref = MessageHub::get_instance();
    DataBlockConfig config{};
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 0;
    config.ring_buffer_capacity = 1;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    TestSchemaV1 schema_v1{42, 'x'};
    TestSchemaV2 schema_v2{42, 3.14};

    producer.reset(create_datablock_producer(hub_ref, channel_name, DataBlockPolicy::RingBuffer,
                                             config, schema_v1));
    ASSERT_NE(producer, nullptr);

    consumer.reset(
        find_datablock_consumer(hub_ref, channel_name, shared_secret, config, schema_v2));
    ASSERT_EQ(consumer, nullptr);
}
