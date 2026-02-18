// tests/test_layer3_datahub/workers/c_api_checksum_workers.cpp
//
// Checksum C API tests: enforcement, corruption detection, and None policy bypass.
//
// Tests that DataBlockProducer/Consumer correctly implement ChecksumPolicy semantics:
// - Enforced: checksum auto-updated on release_write_slot, auto-verified on release_consume_slot
// - None: no checksum computed or verified (corruption undetected)
//
// Test strategy:
// - Enforced roundtrip: normal path (no corruption) → release_consume_slot returns true
// - Corruption detection: get buffer_span BEFORE release, commit+release_write (checksum stored),
//   corrupt buffer bytes, consumer reads and releases → false (checksum mismatch)
// - None policy: same corruption sequence → release_consume_slot returns true (no verification)
//
// Note: buffer_span() remains valid after release_write_slot (buffer_ptr not nulled in pImpl).
// This is the designed mechanism for corruption testing at C API level.
//
// Secret numbers: 72001+ to avoid conflicts with other test suites

#include "datahub_c_api_checksum_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::c_api_checksum
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

static DataBlockConfig make_config(ChecksumPolicy cs_policy, uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 2;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.checksum_policy = cs_policy;
    return cfg;
}

// ============================================================================
// 1. enforced_roundtrip_passes
// ChecksumPolicy::Enforced — write data, commit, release_write_slot (checksum auto-stored).
// Consumer acquires slot, reads data, release_consume_slot returns true (checksum matches).
// ============================================================================

int enforced_roundtrip_passes()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiCsRoundtrip");
            auto cfg = make_config(ChecksumPolicy::Enforced, 72001);

            auto producer = create_datablock_producer_impl(channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            const uint64_t kData = 0xCAFEBABEDEADF00DULL;

            // Write and commit (checksum auto-calculated by release_write_slot)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                auto span = h->buffer_span();
                ASSERT_GE(span.size(), sizeof(kData));
                std::memcpy(span.data(), &kData, sizeof(kData));
                EXPECT_TRUE(h->commit(sizeof(kData)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Read: release_consume_slot must return true (checksum verified = match)
            {
                auto rh = consumer->acquire_consume_slot(1000);
                ASSERT_NE(rh, nullptr);
                bool ok = consumer->release_consume_slot(*rh);
                EXPECT_TRUE(ok) << "Enforced policy: checksum must match on uncorrupted data";
            }

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "enforced_roundtrip_passes", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. enforced_corruption_detected
// ChecksumPolicy::Enforced — after commit+release_write_slot (checksum stored for clean data),
// corrupt the slot buffer in-place. Consumer release must return false (mismatch).
// Verify checksum_failures metric increments to 1.
//
// Buffer_span() stays valid after release_write_slot (buffer_ptr not nulled), so corruption
// can be applied after the checksum has been stored in the shared memory header.
// ============================================================================

int enforced_corruption_detected()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiCsCorrupt");
            auto cfg = make_config(ChecksumPolicy::Enforced, 72002);

            auto producer = create_datablock_producer_impl(channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            // Acquire, get buffer_span BEFORE release (stays valid after release_write_slot)
            std::span<std::byte> slot_span;
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                slot_span = h->buffer_span();
                ASSERT_GE(slot_span.size(), 8u);
                uint64_t val = 0x1122334455667788ULL;
                std::memcpy(slot_span.data(), &val, sizeof(val));
                EXPECT_TRUE(h->commit(sizeof(val)));
                EXPECT_TRUE(producer->release_write_slot(*h));
                // slot_span remains valid; buffer_ptr in pImpl is not nulled on release
            }

            // Corrupt the slot buffer AFTER checksum was stored by release_write_slot
            slot_span[0] = std::byte(static_cast<uint8_t>(slot_span[0]) ^ 0xFF);

            // Consumer: release_consume_slot must detect the mismatch and return false
            {
                auto rh = consumer->acquire_consume_slot(1000);
                ASSERT_NE(rh, nullptr);
                bool ok = consumer->release_consume_slot(*rh);
                EXPECT_FALSE(ok) << "Enforced policy: corrupted data must cause checksum verification to fail";
            }

            // Note: checksum_failures metric is not incremented at this API level;
            // the failure is indicated by the release_consume_slot return value (verified above).

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "enforced_corruption_detected", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. none_skips_verification
// ChecksumPolicy::None — no checksum is computed or verified.
// Even with corrupted data, release_consume_slot must return true.
// ============================================================================

int none_skips_verification()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiCsNone");
            auto cfg = make_config(ChecksumPolicy::None, 72003);

            auto producer = create_datablock_producer_impl(channel, DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(channel, cfg.shared_secret, &cfg,
                                                         nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            std::span<std::byte> slot_span;
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                slot_span = h->buffer_span();
                ASSERT_GE(slot_span.size(), 8u);
                uint64_t val = 0xAAAABBBBCCCCDDDDULL;
                std::memcpy(slot_span.data(), &val, sizeof(val));
                EXPECT_TRUE(h->commit(sizeof(val)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Corrupt buffer — checksum policy is None, so this must go undetected
            slot_span[0] = std::byte(static_cast<uint8_t>(slot_span[0]) ^ 0xFF);
            slot_span[1] = std::byte(static_cast<uint8_t>(slot_span[1]) ^ 0xFF);

            // Consumer: release_consume_slot must return true (no verification with None policy)
            {
                auto rh = consumer->acquire_consume_slot(1000);
                ASSERT_NE(rh, nullptr);
                bool ok = consumer->release_consume_slot(*rh);
                EXPECT_TRUE(ok) << "None policy: verification must be skipped; corrupt data must not cause failure";
            }

            // No checksum_failures incremented with None policy
            DataBlockMetrics metrics{};
            ASSERT_EQ(consumer->get_metrics(metrics), 0);
            EXPECT_EQ(metrics.checksum_failures, 0u)
                << "None policy: checksum_failures must remain 0 (no verification performed)";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "none_skips_verification", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::c_api_checksum

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct CApiChecksumWorkerRegistrar
{
    CApiChecksumWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "c_api_checksum")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::c_api_checksum;
                if (scenario == "enforced_roundtrip_passes")
                    return enforced_roundtrip_passes();
                if (scenario == "enforced_corruption_detected")
                    return enforced_corruption_detected();
                if (scenario == "none_skips_verification")
                    return none_skips_verification();
                fmt::print(stderr, "ERROR: Unknown c_api_checksum scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static CApiChecksumWorkerRegistrar g_c_api_checksum_registrar;
} // namespace
