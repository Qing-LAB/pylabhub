// tests/test_layer3_datahub/workers/schema_validation_workers.cpp
//
// Schema validation tests: dual-schema producer/consumer attach and mismatch detection.
// Uses the template API (create_datablock_producer<FlexZoneT, DataBlockT>) so both
// FlexZone and DataBlock schemas are stored in shared memory and validated on attach.
//
// Rewritten from old single-schema non-template API (removed) to dual-schema template API.
// See: docs/TEST_REFACTOR_TODO.md T2.3, T4.1

#include "schema_validation_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

// ============================================================================
// Schema structs at file scope.
// PYLABHUB_SCHEMA_BEGIN/END expands at non-local scope — structs must be at
// file scope (not inside a namespace or function body).
// ============================================================================

// DataBlock type V1: int32_t + char
struct SchemaValidV1
{
    int32_t a;
    char b;
};
PYLABHUB_SCHEMA_BEGIN(SchemaValidV1)
PYLABHUB_SCHEMA_MEMBER(a)
PYLABHUB_SCHEMA_MEMBER(b)
PYLABHUB_SCHEMA_END(SchemaValidV1)

// DataBlock type V2: layout differs (char b → double c); schema hash will differ from V1
struct SchemaValidV2
{
    int32_t a;
    double c;
};
PYLABHUB_SCHEMA_BEGIN(SchemaValidV2)
PYLABHUB_SCHEMA_MEMBER(a)
PYLABHUB_SCHEMA_MEMBER(c)
PYLABHUB_SCHEMA_END(SchemaValidV2)

namespace pylabhub::tests::worker::schema_validation
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Helper: build config for schema validation tests.
// flex_zone_size must be >= sizeof(FlexZoneT). Since FlexZoneT = SchemaValidV1
// (sizeof ≈ 8 bytes), 64 bytes is ample and safely page-aligned for the test.
// ============================================================================
static DataBlockConfig make_schema_config(uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 1;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.flex_zone_size = 4096; // page-aligned; must be multiple of 4096 and >= sizeof(SchemaValidV1)
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

// ============================================================================
// consumer_connects_with_matching_schema
// Producer stores SchemaValidV1 as both FlexZone and DataBlock schemas.
// Consumer with the same schemas must connect successfully.
// ============================================================================

int consumer_connects_with_matching_schema()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SchemaValidation");
            MessageHub &hub_ref = MessageHub::get_instance();
            auto config = make_schema_config(67890);

            // Producer: FlexZoneT = SchemaValidV1, DataBlockT = SchemaValidV1
            auto producer = create_datablock_producer<SchemaValidV1, SchemaValidV1>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            // Consumer: same schemas → must connect
            auto consumer = find_datablock_consumer<SchemaValidV1, SchemaValidV1>(
                hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr) << "Consumer with matching schema must connect successfully";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "consumer_connects_with_matching_schema", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_fails_to_connect_with_mismatched_schema
// Producer stores SchemaValidV1 as DataBlock schema.
// Consumer expecting SchemaValidV2 as DataBlock schema must be rejected (nullptr).
// ============================================================================

int consumer_fails_to_connect_with_mismatched_schema()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SchemaValidationMismatch");
            MessageHub &hub_ref = MessageHub::get_instance();
            auto config = make_schema_config(67891);

            // Producer: DataBlockT = SchemaValidV1
            auto producer = create_datablock_producer<SchemaValidV1, SchemaValidV1>(
                hub_ref, channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            // Consumer: DataBlockT = SchemaValidV2 (different fields → schema hash mismatch)
            auto consumer = find_datablock_consumer<SchemaValidV1, SchemaValidV2>(
                hub_ref, channel, config.shared_secret, config);
            ASSERT_EQ(consumer, nullptr) << "Consumer with mismatched DataBlock schema must be rejected";

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
