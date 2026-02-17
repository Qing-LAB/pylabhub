// tests/test_layer3_datahub/workers/datahub_write_attach_workers.cpp
//
// DataBlockOpenMode::WriteAttach tests.
//
// These tests verify that a source process can attach R/W to a hub-created shared memory
// segment (created with create_datablock_producer_impl), write data, and that the creator
// can read it back.  Security and schema validation on the WriteAttach path are also exercised.
//
// Test strategy:
// - All tests are single-process (no std::thread needed).
// - The "hub" role is played by create_datablock_producer_impl (Creator mode).
// - The "source" role is played by attach_datablock_as_writer_impl (WriteAttach mode).
// - Secret numbers start at 76001.
//
// Test list:
//   1. creator_then_writer_attach_basic         — Creator creates; WriteAttach writer writes slot;
//                                                 creator-side consumer reads value.
//   2. writer_attach_validates_secret           — WriteAttach with wrong secret → nullptr.
//   3. writer_attach_validates_schema           — WriteAttach with mismatched schema → nullptr.
//   4. segment_persists_after_writer_detach     — Writer resets; creator still valid;
//                                                 DiagnosticHandle opens successfully.

#include "datahub_write_attach_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

// ============================================================================
// Schema structs at file scope.
// PYLABHUB_SCHEMA_BEGIN/END must be used at non-local scope.
// ============================================================================

// SchemaWA_A: struct { uint64_t x; } — used as the creator's DataBlock schema
struct SchemaWA_A
{
    uint64_t x;
};
PYLABHUB_SCHEMA_BEGIN(SchemaWA_A)
PYLABHUB_SCHEMA_MEMBER(x)
PYLABHUB_SCHEMA_END(SchemaWA_A)

// SchemaWA_B: struct { uint32_t a; uint32_t b; } — different layout → different hash
struct SchemaWA_B
{
    uint32_t a;
    uint32_t b;
};
PYLABHUB_SCHEMA_BEGIN(SchemaWA_B)
PYLABHUB_SCHEMA_MEMBER(a)
PYLABHUB_SCHEMA_MEMBER(b)
PYLABHUB_SCHEMA_END(SchemaWA_B)

namespace pylabhub::tests::worker::write_attach
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// Helper: build a minimal RingBuffer config with Latest_only and no checksum enforcement.
static DataBlockConfig make_write_attach_config(uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 2;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

// ============================================================================
// 1. creator_then_writer_attach_basic
// Creator creates the segment; WriteAttach writer connects and writes a value;
// creator-side consumer reads and verifies the value.
// ============================================================================

int creator_then_writer_attach_basic()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("WABasic");
            MessageHub &hub = MessageHub::get_instance();

            DataBlockConfig cfg = make_write_attach_config(76001);

            // Hub (creator) creates and initializes the segment
            auto creator = create_datablock_producer_impl(hub, channel,
                                                          DataBlockPolicy::RingBuffer,
                                                          cfg, nullptr, nullptr);
            ASSERT_NE(creator, nullptr) << "Creator must create successfully";

            // Consumer on the creator side
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                         &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr) << "Creator-side consumer must attach";

            // Source process: attach R/W (WriteAttach) — no init, no unlink
            auto writer = attach_datablock_as_writer_impl(hub, channel, cfg.shared_secret,
                                                          &cfg, nullptr, nullptr);
            ASSERT_NE(writer, nullptr) << "WriteAttach must succeed with correct secret";

            // Writer acquires a slot and writes a sentinel value
            constexpr uint64_t kSentinel = 0xDEAD'BEEF'CAFE'1234ULL;
            {
                auto wh = writer->acquire_write_slot(500);
                ASSERT_NE(wh, nullptr) << "WriteAttach writer must acquire write slot";
                std::memcpy(wh->buffer_span().data(), &kSentinel, sizeof(kSentinel));
                EXPECT_TRUE(wh->commit(sizeof(kSentinel)));
                EXPECT_TRUE(writer->release_write_slot(*wh));
            }

            // Creator-side consumer reads and verifies
            {
                auto rh = consumer->acquire_consume_slot(500);
                ASSERT_NE(rh, nullptr) << "Consumer must read the committed slot";
                uint64_t read_val = 0;
                std::memcpy(&read_val, rh->buffer_span().data(), sizeof(read_val));
                EXPECT_EQ(read_val, kSentinel) << "Read value must match written sentinel";
                EXPECT_TRUE(consumer->release_consume_slot(*rh));
            }

            writer.reset();
            consumer.reset();
            creator.reset();
            cleanup_test_datablock(channel);
        },
        "creator_then_writer_attach_basic", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. writer_attach_validates_secret
// WriteAttach with wrong shared_secret must return nullptr (no attach).
// ============================================================================

int writer_attach_validates_secret()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("WABadSecret");
            MessageHub &hub = MessageHub::get_instance();

            DataBlockConfig cfg = make_write_attach_config(76002);

            auto creator = create_datablock_producer_impl(hub, channel,
                                                          DataBlockPolicy::RingBuffer,
                                                          cfg, nullptr, nullptr);
            ASSERT_NE(creator, nullptr);

            uint64_t wrong_secret = cfg.shared_secret + 1;
            auto writer = attach_datablock_as_writer_impl(hub, channel, wrong_secret,
                                                          &cfg, nullptr, nullptr);
            EXPECT_EQ(writer, nullptr) << "WriteAttach must fail with wrong shared_secret";

            creator.reset();
            cleanup_test_datablock(channel);
        },
        "writer_attach_validates_secret", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. writer_attach_validates_schema
// WriteAttach with a mismatched schema hash must return nullptr.
// Schema mismatch: creator stores schema A; writer presents schema B.
// ============================================================================

int writer_attach_validates_schema()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("WABadSchema");
            MessageHub &hub = MessageHub::get_instance();

            DataBlockConfig cfg = make_write_attach_config(76003);

            // Creator stores DataBlock schema for SchemaWA_A (file-scope struct)
            auto schema_a = pylabhub::schema::generate_schema_info<SchemaWA_A>(
                "DataBlock", pylabhub::schema::SchemaVersion{1, 0, 0});

            auto creator = create_datablock_producer_impl(hub, channel,
                                                          DataBlockPolicy::RingBuffer,
                                                          cfg, nullptr, &schema_a);
            ASSERT_NE(creator, nullptr);

            // Writer presents SchemaWA_B — different layout → different hash → mismatch
            auto schema_b = pylabhub::schema::generate_schema_info<SchemaWA_B>(
                "DataBlock", pylabhub::schema::SchemaVersion{1, 0, 0});

            auto writer = attach_datablock_as_writer_impl(hub, channel, cfg.shared_secret,
                                                          &cfg, nullptr, &schema_b);
            EXPECT_EQ(writer, nullptr) << "WriteAttach must fail when schema hashes mismatch";

            creator.reset();
            cleanup_test_datablock(channel);
        },
        "writer_attach_validates_schema", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 4. segment_persists_after_writer_detach
// Writer resets; creator still holds the segment; DiagnosticHandle must open successfully.
// This verifies that WriteAttach does NOT unlink the segment on destruction.
// ============================================================================

int segment_persists_after_writer_detach()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("WAPersist");
            MessageHub &hub = MessageHub::get_instance();

            DataBlockConfig cfg = make_write_attach_config(76004);

            auto creator = create_datablock_producer_impl(hub, channel,
                                                          DataBlockPolicy::RingBuffer,
                                                          cfg, nullptr, nullptr);
            ASSERT_NE(creator, nullptr);

            auto writer = attach_datablock_as_writer_impl(hub, channel, cfg.shared_secret,
                                                          &cfg, nullptr, nullptr);
            ASSERT_NE(writer, nullptr);

            // Detach writer — must NOT unlink the segment
            writer.reset();

            // Segment must still be accessible via DiagnosticHandle
            auto diag = open_datablock_for_diagnostic(channel);
            EXPECT_NE(diag, nullptr)
                << "DiagnosticHandle must open after WriteAttach writer detaches (segment still alive)";

            if (diag)
            {
                EXPECT_NE(diag->header(), nullptr) << "Diagnostic header must be non-null";
            }

            creator.reset();
            cleanup_test_datablock(channel);
        },
        "segment_persists_after_writer_detach", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::write_attach

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct WriteAttachWorkerRegistrar
{
    WriteAttachWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "write_attach")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::write_attach;
                if (scenario == "creator_then_writer_attach_basic")
                    return creator_then_writer_attach_basic();
                if (scenario == "writer_attach_validates_secret")
                    return writer_attach_validates_secret();
                if (scenario == "writer_attach_validates_schema")
                    return writer_attach_validates_schema();
                if (scenario == "segment_persists_after_writer_detach")
                    return segment_persists_after_writer_detach();
                fmt::print(stderr, "ERROR: Unknown write_attach scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static WriteAttachWorkerRegistrar s_write_attach_registrar;
} // namespace
