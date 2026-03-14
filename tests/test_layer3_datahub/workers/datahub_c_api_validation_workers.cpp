// tests/test_layer3_datahub/workers/datahub_c_api_validation_workers.cpp
//
// C API validation tests — exercise datablock_validate_integrity, datablock_get_metrics,
// datablock_diagnose_slot, and datablock_diagnose_all_slots directly.
//
// Test strategy:
//   - Create datablocks via impl (no templates) to keep dependency on C API surfaces only.
//   - Verify that raw C API functions return expected codes on fresh datablocks.
//   - Verify that integrity validation fails gracefully on a non-existent datablock name.
//
// Secret numbers: 75001–75099

#include "datahub_c_api_validation_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include "utils/recovery_api.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::c_api_validation
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// Returns a fully-valid baseline config.
static DataBlockConfig make_valid_config(uint64_t secret, uint32_t capacity = 2)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = capacity;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

// ============================================================================
// 1. validate_integrity_on_fresh_datablock
// A freshly created DataBlock has valid control structures → RECOVERY_SUCCESS.
// ============================================================================

int validate_integrity_on_fresh_datablock()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiValIntegrity");

            DataBlockConfig cfg = make_valid_config(75001);
            auto producer =
                create_datablock_producer_impl(channel, cfg.policy, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            RecoveryResult r = datablock_validate_integrity(channel.c_str(), false);
            EXPECT_EQ(r, RECOVERY_SUCCESS)
                << "datablock_validate_integrity must return RECOVERY_SUCCESS on a fresh DataBlock";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "validate_integrity_on_fresh_datablock", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. validate_integrity_nonexistent_fails
// A non-existent DataBlock name → not RECOVERY_SUCCESS (open fails).
// ============================================================================

int validate_integrity_nonexistent_fails()
{
    return run_gtest_worker(
        []()
        {
            // Use a name that is never registered as a shared memory segment.
            const char *nonexistent = "/pylabhub_test_nonexistent_75002_xq4z";
            RecoveryResult r = datablock_validate_integrity(nonexistent, false);
            EXPECT_NE(r, RECOVERY_SUCCESS)
                << "datablock_validate_integrity must not return RECOVERY_SUCCESS for a "
                   "non-existent DataBlock";
        },
        "validate_integrity_nonexistent_fails", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. get_metrics_fresh_has_zero_commits
// A freshly created DataBlock has no commits yet → total_slots_written == 0.
// ============================================================================

int get_metrics_fresh_has_zero_commits()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiValMetrics");

            DataBlockConfig cfg = make_valid_config(75003);
            auto producer =
                create_datablock_producer_impl(channel, cfg.policy, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            DataBlockMetrics metrics{};
            int rc = datablock_get_metrics(channel.c_str(), &metrics);
            EXPECT_EQ(rc, 0) << "datablock_get_metrics must return 0 on a valid DataBlock";
            EXPECT_EQ(metrics.total_slots_written, 0u)
                << "A freshly created DataBlock must have total_slots_written == 0";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "get_metrics_fresh_has_zero_commits", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 4. diagnose_slot_fresh_is_free
// A freshly created DataBlock — slot 0 must be in state FREE (0).
// ============================================================================

int diagnose_slot_fresh_is_free()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiValDiagSlot");

            DataBlockConfig cfg = make_valid_config(75004);
            auto producer =
                create_datablock_producer_impl(channel, cfg.policy, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            SlotDiagnostic diag{};
            int rc = datablock_diagnose_slot(channel.c_str(), 0, &diag);
            EXPECT_EQ(rc, 0) << "datablock_diagnose_slot must return 0 on slot 0 of a valid DataBlock";
            EXPECT_EQ(diag.slot_state, 0u)  // SlotState::FREE == 0
                << "Slot 0 of a fresh DataBlock must be in state FREE (0)";
            EXPECT_EQ(diag.slot_index, 0u);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "diagnose_slot_fresh_is_free", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 5. diagnose_all_slots_returns_capacity
// datablock_diagnose_all_slots must report exactly ring_buffer_capacity entries.
// ============================================================================

int diagnose_all_slots_returns_capacity()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CApiValDiagAll");

            constexpr uint32_t capacity = 3;
            DataBlockConfig cfg = make_valid_config(75005, capacity);
            auto producer =
                create_datablock_producer_impl(channel, cfg.policy, cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            SlotDiagnostic slots[8]{};
            size_t out_count = 0;
            int rc = datablock_diagnose_all_slots(channel.c_str(), slots, 8, &out_count);
            EXPECT_EQ(rc, 0) << "datablock_diagnose_all_slots must return 0";
            EXPECT_EQ(out_count, static_cast<size_t>(capacity))
                << "datablock_diagnose_all_slots must report exactly ring_buffer_capacity slots";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "diagnose_all_slots_returns_capacity", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::c_api_validation

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct CApiValidationWorkerRegistrar
{
    CApiValidationWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "c_api_validation")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::c_api_validation;
                if (scenario == "validate_integrity_on_fresh_datablock")
                    return validate_integrity_on_fresh_datablock();
                if (scenario == "validate_integrity_nonexistent_fails")
                    return validate_integrity_nonexistent_fails();
                if (scenario == "get_metrics_fresh_has_zero_commits")
                    return get_metrics_fresh_has_zero_commits();
                if (scenario == "diagnose_slot_fresh_is_free")
                    return diagnose_slot_fresh_is_free();
                if (scenario == "diagnose_all_slots_returns_capacity")
                    return diagnose_all_slots_returns_capacity();
                fmt::print(stderr, "ERROR: Unknown c_api_validation scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static CApiValidationWorkerRegistrar s_c_api_validation_registrar;
} // namespace
