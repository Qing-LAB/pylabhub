// tests/test_layer3_datahub/workers/phase_a_workers.cpp
// Phase A – Protocol/API correctness: flexible zone, checksum, config/agreement.
#include "phase_a_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstdint>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::phase_a
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

int flexible_zone_span_empty_when_no_zones()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_NoZones");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50001;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            // No flexible_zone_configs

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            EXPECT_TRUE(producer->flexible_zone_span(0).empty());
            EXPECT_TRUE(producer->flexible_zone_span(1).empty());
            EXPECT_TRUE(consumer->flexible_zone_span(0).empty());

            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->flexible_zone_span(0).empty());
            EXPECT_TRUE(write_handle->commit(0)); // Must commit so consumer can acquire a slot
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            EXPECT_TRUE(consume_handle->flexible_zone_span(0).empty());
            consume_handle.reset();
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "flexible_zone_span_empty_when_no_zones", logger_module(), crypto_module(), hub_module());
}

int flexible_zone_span_non_empty_when_zones_defined()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_WithZones");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50002;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 256, -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            EXPECT_FALSE(producer->flexible_zone_span(0).empty());
            EXPECT_EQ(producer->flexible_zone_span(0).size(), 256u);
            EXPECT_TRUE(producer->flexible_zone_span(1).empty()); // index >= size
            EXPECT_FALSE(consumer->flexible_zone_span(0).empty());
            EXPECT_EQ(consumer->flexible_zone_span(0).size(), 256u);

            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_FALSE(write_handle->flexible_zone_span(0).empty());
            EXPECT_EQ(write_handle->flexible_zone_span(0).size(), 256u);
            EXPECT_TRUE(write_handle->commit(0)); // Must commit so consumer can acquire a slot
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            EXPECT_FALSE(consume_handle->flexible_zone_span(0).empty());
            consume_handle.reset();
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "flexible_zone_span_non_empty_when_zones_defined", logger_module(), crypto_module(),
        hub_module());
}

int checksum_flexible_zone_false_when_no_zones()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_ChecksumNoZones");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50003;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            // No flexible_zone_configs

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            EXPECT_FALSE(producer->update_checksum_flexible_zone(0));
            EXPECT_FALSE(consumer->verify_checksum_flexible_zone(0));

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "checksum_flexible_zone_false_when_no_zones", logger_module(), crypto_module(),
        hub_module());
}

int checksum_flexible_zone_true_when_valid()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_ChecksumValid");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50004;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 128, -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            std::span<std::byte> zone = producer->flexible_zone_span(0);
            ASSERT_FALSE(zone.empty());
            std::memset(zone.data(), 0xAB, zone.size());

            EXPECT_TRUE(producer->update_checksum_flexible_zone(0));
            EXPECT_TRUE(consumer->verify_checksum_flexible_zone(0));

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "checksum_flexible_zone_true_when_valid", logger_module(), crypto_module(), hub_module());
}

int consumer_without_expected_config_gets_empty_zones()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_NoExpectedConfig");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50005;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 64, -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            // Attach without expected_config (name + secret only)
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret);
            ASSERT_NE(consumer, nullptr);

            EXPECT_TRUE(consumer->flexible_zone_span(0).empty());

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "consumer_without_expected_config_gets_empty_zones", logger_module(), crypto_module(),
        hub_module());
}

int consumer_with_expected_config_gets_zones()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_WithExpectedConfig");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50006;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 128, -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            EXPECT_FALSE(consumer->flexible_zone_span(0).empty());
            EXPECT_EQ(consumer->flexible_zone_span(0).size(), 128u);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "consumer_with_expected_config_gets_zones", logger_module(), crypto_module(), hub_module());
}

// POD used for structured flexible zone tests (no schema BLDS)
struct FrameMeta
{
    uint64_t frame_id;
    uint64_t timestamp_us;
};

int structured_flex_zone_data_passes()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_StructuredFlex");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50007;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"meta", sizeof(FrameMeta), -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            FrameMeta written{12345, 999888777};
            producer->flexible_zone<FrameMeta>(0) = written;

            FrameMeta read = consumer->flexible_zone<FrameMeta>(0);
            EXPECT_EQ(read.frame_id, written.frame_id);
            EXPECT_EQ(read.timestamp_us, written.timestamp_us);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "structured_flex_zone_data_passes", logger_module(), crypto_module(), hub_module());
}

int error_flex_zone_type_too_large_throws()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_ErrorTooSmall");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50008;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 8, -1}); // only 8 bytes

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            // FrameMeta is 16 bytes; zone is 8 → flexible_zone<FrameMeta>(0) should throw
            EXPECT_THROW({ (void)producer->flexible_zone<FrameMeta>(0); }, std::runtime_error);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "error_flex_zone_type_too_large_throws", logger_module(), crypto_module(), hub_module());
}

int error_checksum_flex_zone_fails_after_tampering()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhaseA_ChecksumTamper");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 50009;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 64, -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            std::span<std::byte> zone = producer->flexible_zone_span(0);
            ASSERT_FALSE(zone.empty());
            std::memset(zone.data(), 0x42, zone.size());
            EXPECT_TRUE(producer->update_checksum_flexible_zone(0));

            // Tamper: change one byte so stored checksum no longer matches
            zone[0] = static_cast<std::byte>(static_cast<unsigned char>(zone[0]) ^ 0xFF);

            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);
            EXPECT_FALSE(consumer->verify_checksum_flexible_zone(0));

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "error_checksum_flex_zone_fails_after_tampering", logger_module(), crypto_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::phase_a

namespace
{
struct PhaseAWorkerRegistrar
{
    PhaseAWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "phase_a")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::phase_a;
                if (scenario == "flexible_zone_empty")
                    return flexible_zone_span_empty_when_no_zones();
                if (scenario == "flexible_zone_non_empty")
                    return flexible_zone_span_non_empty_when_zones_defined();
                if (scenario == "checksum_false_no_zones")
                    return checksum_flexible_zone_false_when_no_zones();
                if (scenario == "checksum_true_valid")
                    return checksum_flexible_zone_true_when_valid();
                if (scenario == "consumer_no_config")
                    return consumer_without_expected_config_gets_empty_zones();
                if (scenario == "consumer_with_config")
                    return consumer_with_expected_config_gets_zones();
                if (scenario == "structured_flex_zone_data_passes")
                    return structured_flex_zone_data_passes();
                if (scenario == "error_flex_zone_type_too_large_throws")
                    return error_flex_zone_type_too_large_throws();
                if (scenario == "error_checksum_flex_zone_fails_after_tampering")
                    return error_checksum_flex_zone_fails_after_tampering();
                fmt::print(stderr, "ERROR: Unknown phase_a scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static PhaseAWorkerRegistrar g_phase_a_registrar;
} // namespace
