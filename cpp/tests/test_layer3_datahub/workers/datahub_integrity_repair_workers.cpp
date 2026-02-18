// tests/test_layer3_datahub/workers/datahub_integrity_repair_workers.cpp
//
// Integrity validation tests: fresh-block baseline, layout-checksum corruption detection,
// and magic-number corruption detection.
//
// State injection technique:
//   DiagnosticHandle maps the shared memory segment R/W.  The SharedMemoryHeader pointer
//   returned by diag->header() is writable, so we can directly modify header fields
//   (reserved_header bytes for layout checksum, magic_number atomic field, etc.).
//
// Secrets start at 78001.

#include "datahub_integrity_repair_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include "utils/recovery_api.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::integrity_repair
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

static DataBlockConfig make_integrity_config(uint64_t secret, ChecksumPolicy cp)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 2;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.checksum_policy = cp;
    return cfg;
}

// Helper: write one slot with a known payload and release it.
static bool write_slot(DataBlockProducer &producer, uint64_t payload)
{
    auto wh = producer.acquire_write_slot(500);
    if (!wh)
        return false;
    std::memcpy(wh->buffer_span().data(), &payload, sizeof(payload));
    if (!wh->commit(sizeof(payload)))
        return false;
    return producer.release_write_slot(*wh);
}

// ============================================================================
// 1. validate_integrity_fresh_checksum_block_passes
// Create block with ChecksumPolicy::Enforced, write 2 slots (one per ring slot),
// then call validate_integrity(false) → RECOVERY_SUCCESS.
// This exercises the "checksum_type != Unset" path without injecting any corruption.
// ============================================================================

int validate_integrity_fresh_checksum_block_passes()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("IntegrityFresh");
            DataBlockConfig cfg = make_integrity_config(78001, ChecksumPolicy::Enforced);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Fill both ring slots so commit_index is at a safe boundary.
            // After 2 writes: commit_index=2, 2%capacity=0, only slot 0 checked.
            // Slot 0 was written with BLAKE2b checksum computed on commit → valid.
            ASSERT_TRUE(write_slot(*producer, 0xABCDEF01ULL)) << "Failed to write slot 0";
            ASSERT_TRUE(write_slot(*producer, 0xABCDEF02ULL)) << "Failed to write slot 1";

            RecoveryResult r = datablock_validate_integrity(channel.c_str(), false);
            EXPECT_EQ(r, RECOVERY_SUCCESS)
                << "validate_integrity must succeed on a fresh block with valid checksums";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "validate_integrity_fresh_checksum_block_passes",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. validate_integrity_detects_layout_checksum_mismatch
// Create block with ChecksumPolicy::None (avoids consumer path), then corrupt the
// stored layout checksum in reserved_header[LAYOUT_CHECKSUM_OFFSET].
// validate_integrity(false) → RECOVERY_FAILED.
// validate_integrity(true)  → RECOVERY_FAILED (layout checksum is not repairable).
// Restore the layout checksum before cleanup to allow the segment to close cleanly.
// ============================================================================

int validate_integrity_detects_layout_checksum_mismatch()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("IntegrityLayout");
            DataBlockConfig cfg = make_integrity_config(78002, ChecksumPolicy::None);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Pre-condition: fresh block has a valid layout checksum.
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                EXPECT_TRUE(validate_layout_checksum(diag->header()))
                    << "Pre-condition: layout checksum must be valid on fresh block";
            }

            // Inject corruption: flip the first byte of the stored layout checksum.
            // detail::LAYOUT_CHECKSUM_OFFSET = 32 (offset into reserved_header[]).
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SharedMemoryHeader *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);
                hdr->reserved_header[detail::LAYOUT_CHECKSUM_OFFSET] ^= 0xFF;
            }

            // Verify corruption via the public API.
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                EXPECT_FALSE(validate_layout_checksum(diag->header()))
                    << "Layout checksum must be invalid after corruption";
            }

            // validate_integrity(repair=false) → FAILED.
            RecoveryResult r_check = datablock_validate_integrity(channel.c_str(), false);
            EXPECT_EQ(r_check, RECOVERY_FAILED)
                << "validate_integrity must return FAILED when layout checksum is corrupted";

            // validate_integrity(repair=true) → still FAILED (layout is not repairable).
            RecoveryResult r_repair = datablock_validate_integrity(channel.c_str(), true);
            EXPECT_EQ(r_repair, RECOVERY_FAILED)
                << "validate_integrity must return FAILED even with repair=true for layout corruption";

            // Restore the layout checksum so the segment can be opened cleanly for cleanup.
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                store_layout_checksum(diag->header());
            }

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "validate_integrity_detects_layout_checksum_mismatch",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. validate_integrity_detects_magic_number_corruption
// Create block, corrupt the magic number field, call validate_integrity → RECOVERY_FAILED.
// Restore the magic number before cleanup.
// ============================================================================

int validate_integrity_detects_magic_number_corruption()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("IntegrityMagic");
            DataBlockConfig cfg = make_integrity_config(78003, ChecksumPolicy::None);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Open one DiagnosticHandle and keep it alive through the whole test.
            // After corrupting the magic number, open_datablock_for_diagnostic would fail
            // (it validates the magic) — so we must restore via the handle we already hold.
            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);

            // Inject corruption: overwrite magic_number with a bogus value.
            constexpr uint32_t kBogusMagic = 0xDEADBEEFU;
            diag->header()->magic_number.store(kBogusMagic, std::memory_order_release);

            // validate_integrity opens its own internal handle (via shm_attach) and detects
            // the invalid magic number → RECOVERY_FAILED.
            RecoveryResult r = datablock_validate_integrity(channel.c_str(), false);
            EXPECT_EQ(r, RECOVERY_FAILED)
                << "validate_integrity must return FAILED when magic number is corrupted";

            // Restore the correct magic number via the still-open DiagnosticHandle.
            diag->header()->magic_number.store(detail::DATABLOCK_MAGIC_NUMBER,
                                               std::memory_order_release);
            diag.reset(); // close diagnostic handle before producer destroy

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "validate_integrity_detects_magic_number_corruption",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::integrity_repair

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct IntegrityRepairWorkerRegistrar
{
    IntegrityRepairWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "integrity_repair")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::integrity_repair;
                if (scenario == "validate_integrity_fresh_checksum_block_passes")
                    return validate_integrity_fresh_checksum_block_passes();
                if (scenario == "validate_integrity_detects_layout_checksum_mismatch")
                    return validate_integrity_detects_layout_checksum_mismatch();
                if (scenario == "validate_integrity_detects_magic_number_corruption")
                    return validate_integrity_detects_magic_number_corruption();
                fmt::print(stderr, "ERROR: Unknown integrity_repair scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static IntegrityRepairWorkerRegistrar s_integrity_repair_registrar;
} // namespace
