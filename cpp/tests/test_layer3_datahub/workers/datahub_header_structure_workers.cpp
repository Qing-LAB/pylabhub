// tests/test_layer3_datahub/workers/datahub_header_structure_workers.cpp
//
// SharedMemoryHeader structure tests — verify dual-schema hash fields.
//
// Test strategy:
//   - Use DiagnosticHandle::header() to access the raw SharedMemoryHeader.
//   - When a producer is created via the template API (with FlexZone/DataBlock types),
//     both flexzone_schema_hash and datablock_schema_hash must be non-zero.
//   - When created without schemas (impl with nullptr), both hash arrays must be all-zero.
//   - Different type pairs must produce different hashes.
//
// Secret numbers: 74001–74099

#include "datahub_header_structure_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <algorithm>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::header_structure
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

static bool has_nonzero_bytes(const uint8_t *arr, size_t len)
{
    return std::any_of(arr, arr + len, [](uint8_t b) { return b != 0; });
}

// ============================================================================
// 1. schema_hashes_populated_with_template_api
// Template API generates schemas → both hash fields must be non-zero.
// ============================================================================

int schema_hashes_populated_with_template_api()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("HdrSchemaPopulated");

            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            cfg.shared_secret = 74001;
            cfg.ring_buffer_capacity = 2;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;
            cfg.flex_zone_size = sizeof(TestFlexZone);

            // Template API: both schemas generated and stored
            auto producer =
                create_datablock_producer<TestFlexZone, TestDataBlock>(channel,
                                                                        DataBlockPolicy::RingBuffer,
                                                                        cfg);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            auto *hdr = diag->header();
            ASSERT_NE(hdr, nullptr);

            EXPECT_TRUE(has_nonzero_bytes(hdr->flexzone_schema_hash, 32))
                << "flexzone_schema_hash must be non-zero when producer created with FlexZone type";
            EXPECT_TRUE(has_nonzero_bytes(hdr->datablock_schema_hash, 32))
                << "datablock_schema_hash must be non-zero when producer created with DataBlock type";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "schema_hashes_populated_with_template_api", logger_module(), crypto_module(),
        hub_module());
}

// ============================================================================
// 2. schema_hashes_zero_without_schema
// impl API with nullptr schemas → both hash fields must be all-zero.
// ============================================================================

int schema_hashes_zero_without_schema()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("HdrSchemaZero");

            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            cfg.shared_secret = 74002;
            cfg.ring_buffer_capacity = 2;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;

            // impl API: no schemas (nullptr)
            auto producer =
                create_datablock_producer_impl(channel, cfg.policy, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            auto *hdr = diag->header();
            ASSERT_NE(hdr, nullptr);

            EXPECT_FALSE(has_nonzero_bytes(hdr->flexzone_schema_hash, 32))
                << "flexzone_schema_hash must be all-zero when producer created without schema";
            EXPECT_FALSE(has_nonzero_bytes(hdr->datablock_schema_hash, 32))
                << "datablock_schema_hash must be all-zero when producer created without schema";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "schema_hashes_zero_without_schema", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. different_types_produce_different_hashes
// Two producers with different type pairs must have different hash values.
// TestFlexZone/TestDataBlock vs EmptyFlexZone/MinimalData must differ.
// ============================================================================

int different_types_produce_different_hashes()
{
    return run_gtest_worker(
        []()
        {
            std::string ch1 = make_test_channel_name("HdrHashDiffA");
            std::string ch2 = make_test_channel_name("HdrHashDiffB");

            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            cfg.ring_buffer_capacity = 2;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;

            cfg.shared_secret = 74003;
            cfg.flex_zone_size = sizeof(TestFlexZone);
            auto prod1 =
                create_datablock_producer<TestFlexZone, TestDataBlock>(ch1,
                                                                        DataBlockPolicy::RingBuffer,
                                                                        cfg);
            ASSERT_NE(prod1, nullptr);

            cfg.shared_secret = 74004;
            cfg.flex_zone_size = sizeof(EmptyFlexZone);
            auto prod2 =
                create_datablock_producer<EmptyFlexZone, MinimalData>(ch2,
                                                                       DataBlockPolicy::RingBuffer,
                                                                       cfg);
            ASSERT_NE(prod2, nullptr);

            auto diag1 = open_datablock_for_diagnostic(ch1);
            auto diag2 = open_datablock_for_diagnostic(ch2);
            ASSERT_NE(diag1, nullptr);
            ASSERT_NE(diag2, nullptr);
            auto *hdr1 = diag1->header();
            auto *hdr2 = diag2->header();
            ASSERT_NE(hdr1, nullptr);
            ASSERT_NE(hdr2, nullptr);

            // Both schemas populated
            ASSERT_TRUE(has_nonzero_bytes(hdr1->flexzone_schema_hash, 32));
            ASSERT_TRUE(has_nonzero_bytes(hdr1->datablock_schema_hash, 32));
            ASSERT_TRUE(has_nonzero_bytes(hdr2->flexzone_schema_hash, 32));
            ASSERT_TRUE(has_nonzero_bytes(hdr2->datablock_schema_hash, 32));

            // Different types → different hashes
            EXPECT_NE(std::memcmp(hdr1->datablock_schema_hash, hdr2->datablock_schema_hash, 32), 0)
                << "Different DataBlock types must produce different datablock_schema_hash values";

            prod1.reset();
            prod2.reset();
            cleanup_test_datablock(ch1);
            cleanup_test_datablock(ch2);
        },
        "different_types_produce_different_hashes", logger_module(), crypto_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::header_structure

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct HeaderStructureWorkerRegistrar
{
    HeaderStructureWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "header_structure")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::header_structure;
                if (scenario == "schema_hashes_populated_with_template_api")
                    return schema_hashes_populated_with_template_api();
                if (scenario == "schema_hashes_zero_without_schema")
                    return schema_hashes_zero_without_schema();
                if (scenario == "different_types_produce_different_hashes")
                    return different_types_produce_different_hashes();
                fmt::print(stderr, "ERROR: Unknown header_structure scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static HeaderStructureWorkerRegistrar s_header_structure_registrar;
} // namespace
