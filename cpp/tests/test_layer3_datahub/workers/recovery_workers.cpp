// tests/test_layer3_datahub/workers/recovery_workers.cpp
#include "recovery_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include "utils/heartbeat_manager.hpp"
#include "utils/integrity_validator.hpp"
#include "utils/slot_diagnostics.hpp"
#include "utils/slot_recovery.hpp"
#include "utils/recovery_api.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::recovery
{

static auto logger_module() { return pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return pylabhub::hub::GetLifecycleModule(); }

int datablock_is_process_alive_returns_true_for_self()
{
    return run_gtest_worker(
        []()
        {
            uint64_t my_pid = pylabhub::platform::get_pid();
            EXPECT_TRUE(datablock_is_process_alive(my_pid))
                << "datablock_is_process_alive should return true for current process";
        },
        "datablock_is_process_alive_returns_true_for_self", logger_module(), crypto_module(), hub_module());
}

int integrity_validator_validate_succeeds_on_created_datablock()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("IntegrityValidator");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 12345;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            IntegrityValidator validator(channel);
            RecoveryResult r = validator.validate(false);
            EXPECT_EQ(r, RECOVERY_SUCCESS) << "validate() should succeed on freshly created DataBlock";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "integrity_validator_validate_succeeds_on_created_datablock", logger_module(), crypto_module(), hub_module());
}

int slot_diagnostics_refresh_succeeds_on_created_datablock()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotDiagnostics");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 12345;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            SlotDiagnostics diag(channel, 0);
            EXPECT_TRUE(diag.refresh()) << "refresh() should succeed on slot 0";
            EXPECT_TRUE(diag.is_valid());
            EXPECT_EQ(diag.get_slot_index(), 0u);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "slot_diagnostics_refresh_succeeds_on_created_datablock", logger_module(), crypto_module(), hub_module());
}

int slot_recovery_release_zombie_readers_on_empty_slot()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotRecovery");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 12345;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            SlotRecovery recovery(channel, 0);
            RecoveryResult r = recovery.release_zombie_readers(false);
            EXPECT_TRUE(r == RECOVERY_SUCCESS || r == RECOVERY_NOT_STUCK)
                << "release_zombie_readers on empty slot should return SUCCESS or NOT_STUCK";

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "slot_recovery_release_zombie_readers_on_empty_slot", logger_module(), crypto_module(), hub_module());
}

int heartbeat_manager_registers_and_pulses()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("HeartbeatManager");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 54321;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            {
                HeartbeatManager mgr(*consumer);
                EXPECT_TRUE(mgr.is_registered()) << "HeartbeatManager should be registered";
                mgr.pulse();
                EXPECT_TRUE(mgr.is_registered()) << "HeartbeatManager should remain registered after pulse";
            }
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "heartbeat_manager_registers_and_pulses", logger_module(), crypto_module(), hub_module());
}

int producer_update_heartbeat_explicit_succeeds()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ProducerHeartbeat");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 65432;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            producer->update_heartbeat();
            producer->update_heartbeat();

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "producer_update_heartbeat_explicit_succeeds", logger_module(), crypto_module(), hub_module());
}

int producer_heartbeat_and_is_writer_alive()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ProducerHeartbeatIsWriterAlive");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 76543;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            const uint64_t my_pid = pylabhub::platform::get_pid();

            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            const char payload[] = "heartbeat-test";
            EXPECT_TRUE(write_handle->write(payload, sizeof(payload)));
            EXPECT_TRUE(write_handle->commit(sizeof(payload)));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            const SharedMemoryHeader *header = diag->header();
            ASSERT_NE(header, nullptr);

            EXPECT_TRUE(is_writer_alive(header, my_pid))
                << "is_writer_alive(header, my_pid) should be true after commit (heartbeat fresh)";
            EXPECT_TRUE(is_writer_alive(header, my_pid))
                << "is_writer_alive idempotent";

            producer.reset();
            diag.reset();
            cleanup_test_datablock(channel);
        },
        "producer_heartbeat_and_is_writer_alive", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::recovery

namespace
{
struct RecoveryWorkerRegistrar
{
    RecoveryWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "recovery")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::recovery;
                if (scenario == "datablock_is_process_alive")
                    return datablock_is_process_alive_returns_true_for_self();
                if (scenario == "integrity_validator_validate")
                    return integrity_validator_validate_succeeds_on_created_datablock();
                if (scenario == "slot_diagnostics_refresh")
                    return slot_diagnostics_refresh_succeeds_on_created_datablock();
                if (scenario == "slot_recovery_release_zombie_readers")
                    return slot_recovery_release_zombie_readers_on_empty_slot();
                if (scenario == "heartbeat_manager_registers")
                    return heartbeat_manager_registers_and_pulses();
                if (scenario == "producer_update_heartbeat_explicit")
                    return producer_update_heartbeat_explicit_succeeds();
                if (scenario == "producer_heartbeat_and_is_writer_alive")
                    return producer_heartbeat_and_is_writer_alive();
                fmt::print(stderr, "ERROR: Unknown recovery scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static RecoveryWorkerRegistrar g_recovery_registrar;
} // namespace
