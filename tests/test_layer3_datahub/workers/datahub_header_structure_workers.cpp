// tests/test_layer3_datahub/workers/datahub_header_structure_workers.cpp
//
// SharedMemoryHeader structure tests — verify dual-schema hash fields.
//
// Test strategy (post-#275-S2c-6):
//   - Use the narrow accessors `producer->flexzone_schema_hash()` /
//     `producer->datablock_schema_hash()` to read the schema-hash fields
//     by value (no borrowed view into the SHM mmap).  Replaces the
//     pre-S2c-6 reliance on `open_datablock_for_diagnostic(name)` +
//     `diag->header()->...` which both opened a name-based SHM segment
//     (incompatible with HEP-CORE-0041 fd-source) and exposed the full
//     header struct to the test.
//   - When a producer is created via the template API (with FlexZone/DataBlock types),
//     both flexzone_schema_hash and datablock_schema_hash must be non-zero.
//   - When created without schemas (impl with nullptr), both hash arrays must be all-zero.
//   - Different type pairs must produce different hashes.

#include "datahub_header_structure_workers.h"
#include "datahub_fd_test_helper.h"  // #275-S2: fd-source typed helpers
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include "utils/security/shm_capability_channel.hpp"  // _impl variant: inline transport mint
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <algorithm>
#include <array>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::header_structure
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetDataBlockModule(); }

static bool has_nonzero_bytes(const std::array<uint8_t, 32> &arr)
{
    return std::any_of(arr.begin(), arr.end(), [](uint8_t b) { return b != 0; });
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
            cfg.ring_buffer_capacity = 2;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;
            cfg.flex_zone_size = sizeof(TestFlexZone);

            // Template API: both schemas generated and stored (fd-source).
            auto p = make_fd_backed_producer_typed<TestFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(p.producer, nullptr);
            auto& producer = p.producer;

            EXPECT_TRUE(has_nonzero_bytes(producer->flexzone_schema_hash()))
                << "flexzone_schema_hash must be non-zero when producer created with FlexZone type";
            EXPECT_TRUE(has_nonzero_bytes(producer->datablock_schema_hash()))
                << "datablock_schema_hash must be non-zero when producer created with DataBlock type";

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
            namespace sec = pylabhub::utils::security;
            std::string channel = make_test_channel_name("HdrSchemaZero");

            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            cfg.ring_buffer_capacity = 2;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;

            // impl API with nullptr schemas — exercises the no-schema branch
            // directly; the typed helpers always pass non-null schemas so we
            // mint the transport inline and call the impl factory.
            const size_t total = datablock_layout_total_size(cfg);
            auto transport = sec::create_shm_capability_producer(total);
            ASSERT_NE(transport, nullptr);
            auto producer = create_datablock_producer_from_fd_impl(
                channel, transport->borrow_fd(), cfg.policy, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            EXPECT_FALSE(has_nonzero_bytes(producer->flexzone_schema_hash()))
                << "flexzone_schema_hash must be all-zero when producer created without schema";
            EXPECT_FALSE(has_nonzero_bytes(producer->datablock_schema_hash()))
                << "datablock_schema_hash must be all-zero when producer created without schema";

            producer.reset();
            transport.reset();
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

            cfg.flex_zone_size = sizeof(TestFlexZone);
            auto p1 = make_fd_backed_producer_typed<TestFlexZone, TestDataBlock>(
                ch1, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(p1.producer, nullptr);
            auto& prod1 = p1.producer;

            cfg.flex_zone_size = sizeof(EmptyFlexZone);
            auto p2 = make_fd_backed_producer_typed<EmptyFlexZone, MinimalData>(
                ch2, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(p2.producer, nullptr);
            auto& prod2 = p2.producer;

            // Both schemas populated
            const auto fz1 = prod1->flexzone_schema_hash();
            const auto db1 = prod1->datablock_schema_hash();
            const auto fz2 = prod2->flexzone_schema_hash();
            const auto db2 = prod2->datablock_schema_hash();
            ASSERT_TRUE(has_nonzero_bytes(fz1));
            ASSERT_TRUE(has_nonzero_bytes(db1));
            ASSERT_TRUE(has_nonzero_bytes(fz2));
            ASSERT_TRUE(has_nonzero_bytes(db2));

            // Different types → different hashes (std::array operator!=).
            EXPECT_NE(db1, db2)
                << "Different DataBlock types must produce different datablock_schema_hash values";

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
