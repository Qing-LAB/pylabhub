// tests/test_layer3_datahub/workers/datahub_config_validation_workers.cpp
//
// DataBlockConfig validation tests — verify that create_datablock_producer_impl throws
// std::invalid_argument when mandatory config fields are unset or out of range.
//
// Test strategy:
//   - Each test builds a complete valid config, then invalidates exactly one field.
//   - EXPECT_THROW verifies the correct exception type.
//   - The "valid config" test confirms a fully-configured producer succeeds.
//
// Secret numbers: 73001–73099

#include "datahub_config_validation_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <stdexcept>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::config_validation
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// Returns a fully-valid baseline config. Tests override individual fields to trigger throws.
static DataBlockConfig make_valid_config(uint64_t secret)
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
// 1. policy_unset_throws
// ============================================================================

int policy_unset_throws()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CfgPolicyUnset");

            DataBlockConfig cfg = make_valid_config(73001);
            cfg.policy = DataBlockPolicy::Unset;

            EXPECT_THROW(
                (void)create_datablock_producer_impl(channel, DataBlockPolicy::Unset,
                                                     cfg, nullptr, nullptr),
                std::invalid_argument)
                << "create_datablock_producer_impl must throw std::invalid_argument "
                   "when DataBlockConfig::policy is Unset";
        },
        "policy_unset_throws", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. consumer_sync_policy_unset_throws
// ============================================================================

int consumer_sync_policy_unset_throws()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CfgSyncUnset");

            DataBlockConfig cfg = make_valid_config(73002);
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Unset;

            EXPECT_THROW(
                (void)create_datablock_producer_impl(channel, cfg.policy,
                                                     cfg, nullptr, nullptr),
                std::invalid_argument)
                << "create_datablock_producer_impl must throw when consumer_sync_policy is Unset";
        },
        "consumer_sync_policy_unset_throws", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. physical_page_size_unset_throws
// ============================================================================

int physical_page_size_unset_throws()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CfgPageUnset");

            DataBlockConfig cfg = make_valid_config(73003);
            cfg.physical_page_size = DataBlockPageSize::Unset;

            EXPECT_THROW(
                (void)create_datablock_producer_impl(channel, cfg.policy,
                                                     cfg, nullptr, nullptr),
                std::invalid_argument)
                << "create_datablock_producer_impl must throw when physical_page_size is Unset";
        },
        "physical_page_size_unset_throws", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 4. ring_buffer_capacity_zero_throws
// ============================================================================

int ring_buffer_capacity_zero_throws()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CfgCapZero");

            DataBlockConfig cfg = make_valid_config(73004);
            cfg.ring_buffer_capacity = 0;

            EXPECT_THROW(
                (void)create_datablock_producer_impl(channel, cfg.policy,
                                                     cfg, nullptr, nullptr),
                std::invalid_argument)
                << "create_datablock_producer_impl must throw when ring_buffer_capacity is 0";
        },
        "ring_buffer_capacity_zero_throws", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 5. valid_config_creates_successfully
// ============================================================================

int valid_config_creates_successfully()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("CfgValid");

            DataBlockConfig cfg = make_valid_config(73005);
            auto producer = create_datablock_producer_impl(channel, cfg.policy,
                                                           cfg, nullptr, nullptr);
            EXPECT_NE(producer, nullptr)
                << "create_datablock_producer_impl must succeed with a fully valid config";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "valid_config_creates_successfully", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::config_validation

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct ConfigValidationWorkerRegistrar
{
    ConfigValidationWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "config_validation")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::config_validation;
                if (scenario == "policy_unset_throws")
                    return policy_unset_throws();
                if (scenario == "consumer_sync_policy_unset_throws")
                    return consumer_sync_policy_unset_throws();
                if (scenario == "physical_page_size_unset_throws")
                    return physical_page_size_unset_throws();
                if (scenario == "ring_buffer_capacity_zero_throws")
                    return ring_buffer_capacity_zero_throws();
                if (scenario == "valid_config_creates_successfully")
                    return valid_config_creates_successfully();
                fmt::print(stderr, "ERROR: Unknown config_validation scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static ConfigValidationWorkerRegistrar s_config_validation_registrar;
} // namespace
