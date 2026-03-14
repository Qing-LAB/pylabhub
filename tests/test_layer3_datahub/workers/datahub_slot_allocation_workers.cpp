// tests/test_layer3_datahub/workers/datahub_slot_allocation_workers.cpp
//
// DataBlock slot allocation tests — verify that different schema sizes and ring
// buffer capacities produce correctly-sized shared memory segments, and that
// data can be written and read back through the allocated slots.
//
// Test strategy:
//   - Use create_datablock_producer_impl with explicit logical_unit_size to
//     simulate schemas of various sizes (no C++ type needed).
//   - Use DiagnosticHandle::header() to read the SharedMemoryHeader and verify:
//       · logical_unit_size is rounded to 64-byte cache-line boundary
//       · ring_buffer_capacity matches config
//       · total_block_size is reasonable (>= ring_buffer_capacity * logical_unit_size)
//   - Verify write/read roundtrip via acquire_write_slot / acquire_consume_slot.
//
// Secret numbers: 76001–76099

#include "datahub_slot_allocation_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::slot_allocation
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

static DataBlockConfig make_config(uint64_t secret, size_t logical_size, uint32_t capacity)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Sequential;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = capacity;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.logical_unit_size = logical_size;
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

static size_t round_to_cacheline(size_t n)
{
    constexpr size_t kCacheLine = 64;
    return (n + kCacheLine - 1) & ~(kCacheLine - 1);
}

// ============================================================================
// 1. varied_schema_sizes_allocation
// Verify that a range of schema sizes (1 byte to 100 KB) all produce correct
// header fields: logical_unit_size is cache-line aligned, ring_buffer_capacity
// matches, and total_block_size >= ring * slot stride.
// ============================================================================

int varied_schema_sizes_allocation()
{
    return run_gtest_worker(
        []()
        {
            // Test schema sizes spanning sub-cacheline to large
            const std::vector<size_t> schema_sizes = {
                1,      // minimal — should round to 64
                16,     // sub-cacheline
                48,     // sub-cacheline, not power-of-2
                64,     // exactly one cache line
                65,     // just over one cache line → 128
                100,    // arbitrary mid-size → 128
                256,    // power-of-2, multiple of 64
                1000,   // ~1 KB, not aligned → 1024
                4096,   // exactly one OS page
                4112,   // demo-like: just over 4K → 4160
                8192,   // 2 pages
                65536,  // 64 KB
                102400, // 100 KB
            };

            constexpr uint32_t kCapacity = 8;
            uint64_t secret_base = 76001;

            for (size_t schema_sz : schema_sizes)
            {
                std::string tag = fmt::format("SlotAlloc_sz{}", schema_sz);
                std::string channel = make_test_channel_name(tag.c_str());

                auto cfg = make_config(secret_base++, schema_sz, kCapacity);
                size_t expected_slot_stride = round_to_cacheline(schema_sz);

                auto producer = create_datablock_producer_impl(
                    channel, cfg.policy, cfg, nullptr, nullptr);
                ASSERT_NE(producer, nullptr)
                    << "Failed to create producer for schema_size=" << schema_sz;

                // Inspect header via diagnostic handle
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr)
                    << "Failed to open diagnostic for schema_size=" << schema_sz;
                auto *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);

                EXPECT_EQ(hdr->logical_unit_size, static_cast<uint32_t>(expected_slot_stride))
                    << "schema_size=" << schema_sz
                    << " expected slot stride=" << expected_slot_stride;

                EXPECT_EQ(hdr->ring_buffer_capacity, kCapacity)
                    << "schema_size=" << schema_sz;

                size_t min_total = kCapacity * expected_slot_stride;
                EXPECT_GE(hdr->total_block_size, min_total)
                    << "schema_size=" << schema_sz
                    << " total_block_size=" << hdr->total_block_size
                    << " must be >= " << min_total;

                // Verify slot buffer size matches expected stride
                auto write_handle = producer->acquire_write_slot(5000);
                ASSERT_NE(write_handle, nullptr)
                    << "Failed to acquire write slot for schema_size=" << schema_sz;
                EXPECT_EQ(write_handle->buffer_span().size_bytes(), expected_slot_stride)
                    << "schema_size=" << schema_sz;
                EXPECT_TRUE(producer->release_write_slot(*write_handle));

                producer.reset();
                diag.reset();
                cleanup_test_datablock(channel);
            }
        },
        "varied_schema_sizes_allocation", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. ring_buffer_scaling
// Verify that ring buffer capacity from 1 to 1000 allocates correctly and
// total_block_size scales with capacity.
// ============================================================================

int ring_buffer_scaling()
{
    return run_gtest_worker(
        []()
        {
            // Test a representative set of capacities from 1 to 1000
            const std::vector<uint32_t> capacities = {
                1, 2, 3, 4, 8, 16, 32, 64, 100, 128, 256, 500, 1000};

            constexpr size_t kSchemaSize = 4112; // demo-like schema
            const size_t expected_stride = round_to_cacheline(kSchemaSize); // 4160

            uint64_t secret_base = 76030;

            for (uint32_t cap : capacities)
            {
                std::string tag = fmt::format("SlotAlloc_cap{}", cap);
                std::string channel = make_test_channel_name(tag.c_str());

                auto cfg = make_config(secret_base++, kSchemaSize, cap);

                auto producer = create_datablock_producer_impl(
                    channel, cfg.policy, cfg, nullptr, nullptr);
                ASSERT_NE(producer, nullptr)
                    << "Failed to create producer for capacity=" << cap;

                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                auto *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);

                EXPECT_EQ(hdr->ring_buffer_capacity, cap)
                    << "capacity=" << cap;
                EXPECT_EQ(hdr->logical_unit_size, static_cast<uint32_t>(expected_stride))
                    << "capacity=" << cap;

                size_t min_total = cap * expected_stride;
                EXPECT_GE(hdr->total_block_size, min_total)
                    << "capacity=" << cap
                    << " total_block_size=" << hdr->total_block_size
                    << " must be >= " << min_total;

                // Verify we can acquire at least one slot
                auto write_handle = producer->acquire_write_slot(5000);
                ASSERT_NE(write_handle, nullptr)
                    << "Failed to acquire write slot for capacity=" << cap;
                EXPECT_EQ(write_handle->buffer_span().size_bytes(), expected_stride);
                EXPECT_TRUE(producer->release_write_slot(*write_handle));

                producer.reset();
                diag.reset();
                cleanup_test_datablock(channel);
            }
        },
        "ring_buffer_scaling", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. write_read_roundtrip_varied_sizes
// Write known data patterns into slots of varying sizes and read them back
// through a consumer to confirm data integrity.
// ============================================================================

int write_read_roundtrip_varied_sizes()
{
    return run_gtest_worker(
        []()
        {
            const std::vector<size_t> schema_sizes = {
                8,     // tiny — 1 uint64_t
                64,    // exactly one cache line
                200,   // mid-size, not aligned → 256
                4112,  // demo-like
                16384, // 16 KB
            };

            constexpr uint32_t kCapacity = 4;
            uint64_t secret_base = 76050;

            for (size_t schema_sz : schema_sizes)
            {
                std::string tag = fmt::format("SlotRW_sz{}", schema_sz);
                std::string channel = make_test_channel_name(tag.c_str());

                auto cfg = make_config(secret_base++, schema_sz, kCapacity);
                size_t slot_stride = round_to_cacheline(schema_sz);

                auto producer = create_datablock_producer_impl(
                    channel, cfg.policy, cfg, nullptr, nullptr);
                ASSERT_NE(producer, nullptr)
                    << "Failed to create producer for schema_size=" << schema_sz;

                auto consumer = find_datablock_consumer_impl(
                    channel, cfg.shared_secret, &cfg, nullptr, nullptr,
                    "test_consumer", "test_consumer");
                ASSERT_NE(consumer, nullptr)
                    << "Failed to create consumer for schema_size=" << schema_sz;

                // Write 3 slots with known patterns
                for (uint32_t i = 0; i < 3; ++i)
                {
                    auto wh = producer->acquire_write_slot(5000);
                    ASSERT_NE(wh, nullptr)
                        << "Write acquire failed at i=" << i
                        << " schema_size=" << schema_sz;

                    auto buf = wh->buffer_span();
                    ASSERT_EQ(buf.size_bytes(), slot_stride);

                    // Fill with a recognizable pattern: byte value = (i + offset) & 0xFF
                    // Only fill up to schema_sz bytes (the usable portion)
                    auto *ptr = reinterpret_cast<uint8_t *>(buf.data());
                    size_t fill_len = std::min(schema_sz, buf.size_bytes());
                    for (size_t b = 0; b < fill_len; ++b)
                        ptr[b] = static_cast<uint8_t>((i + b) & 0xFF);

                    EXPECT_TRUE(producer->release_write_slot(*wh));
                }

                // Read back and verify
                for (uint32_t i = 0; i < 3; ++i)
                {
                    auto rh = consumer->acquire_consume_slot(5000);
                    ASSERT_NE(rh, nullptr)
                        << "Read acquire failed at i=" << i
                        << " schema_size=" << schema_sz;

                    auto buf = rh->buffer_span();
                    ASSERT_EQ(buf.size_bytes(), slot_stride);

                    const auto *ptr = reinterpret_cast<const uint8_t *>(buf.data());
                    size_t check_len = std::min(schema_sz, buf.size_bytes());
                    for (size_t b = 0; b < check_len; ++b)
                    {
                        uint8_t expected = static_cast<uint8_t>((i + b) & 0xFF);
                        EXPECT_EQ(ptr[b], expected)
                            << "Mismatch at slot=" << i << " byte=" << b
                            << " schema_size=" << schema_sz;
                        if (ptr[b] != expected)
                            break; // one failure message is enough
                    }

                    consumer->release_consume_slot(*rh);
                }

                consumer.reset();
                producer.reset();
                cleanup_test_datablock(channel);
            }
        },
        "write_read_roundtrip_varied_sizes", logger_module(), crypto_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::slot_allocation

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct SlotAllocationWorkerRegistrar
{
    SlotAllocationWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "slot_allocation")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::slot_allocation;
                if (scenario == "varied_schema_sizes_allocation")
                    return varied_schema_sizes_allocation();
                if (scenario == "ring_buffer_scaling")
                    return ring_buffer_scaling();
                if (scenario == "write_read_roundtrip_varied_sizes")
                    return write_read_roundtrip_varied_sizes();
                fmt::print(stderr, "ERROR: Unknown slot_allocation scenario '{}'\n",
                           scenario);
                return 1;
            });
    }
};
static SlotAllocationWorkerRegistrar s_slot_allocation_registrar;
} // namespace
