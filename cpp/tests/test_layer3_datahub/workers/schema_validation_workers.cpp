// tests/test_layer3_datahub/workers/schema_validation_workers.cpp
#include "schema_validation_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>

using namespace pylabhub::hub;
using namespace pylabhub::schema;
using namespace pylabhub::tests::helper;

// Schema structs at file scope so PYLABHUB_SCHEMA_* expands in correct namespace (pylabhub::schema)
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

namespace pylabhub::tests::worker::schema_validation
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

int consumer_connects_with_matching_schema()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SchemaValidation");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 67890;
            config.ring_buffer_capacity = 1;
            config.physical_page_size = DataBlockPageSize::Size4K;

            TestSchemaV1 schema_v1{42, 'x'};

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config,
                                                      schema_v1);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret,
                                                    config, schema_v1);
            ASSERT_NE(consumer, nullptr);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "consumer_connects_with_matching_schema", logger_module(), crypto_module(), hub_module());
}

int consumer_fails_to_connect_with_mismatched_schema()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SchemaValidationMismatch");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 67890;
            config.ring_buffer_capacity = 1;
            config.physical_page_size = DataBlockPageSize::Size4K;

            TestSchemaV1 schema_v1{42, 'x'};
            TestSchemaV2 schema_v2{42, 3.14};

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config,
                                                      schema_v1);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret,
                                                    config, schema_v2);
            ASSERT_EQ(consumer, nullptr);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "consumer_fails_to_connect_with_mismatched_schema", logger_module(), crypto_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::schema_validation

namespace
{
struct SchemaValidationWorkerRegistrar
{
    SchemaValidationWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "schema_validation")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::schema_validation;
                if (scenario == "consumer_connects_matching")
                    return consumer_connects_with_matching_schema();
                if (scenario == "consumer_fails_mismatched")
                    return consumer_fails_to_connect_with_mismatched_schema();
                fmt::print(stderr, "ERROR: Unknown schema_validation scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static SchemaValidationWorkerRegistrar g_schema_validation_registrar;
} // namespace
