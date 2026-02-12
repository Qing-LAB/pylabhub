// tests/test_layer3_datahub/workers/slot_protocol_workers.cpp
#include "slot_protocol_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::slot_protocol
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

int write_read_succeeds_in_process()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocol");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 11111;
            config.ring_buffer_capacity = 2;
            config.unit_block_size = DataBlockUnitSize::Size4K;
            config.enable_checksum = false;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char payload[] = "Phase B slot protocol test payload";
            const size_t payload_len = sizeof(payload);

            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr) << "acquire_write_slot failed";
            EXPECT_TRUE(write_handle->write(payload, payload_len));
            EXPECT_TRUE(write_handle->commit(payload_len));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr) << "acquire_consume_slot failed";
            std::string read_buf(payload_len, '\0');
            EXPECT_TRUE(consume_handle->read(read_buf.data(), payload_len));
            EXPECT_EQ(std::memcmp(read_buf.data(), payload, payload_len), 0)
                << "read data does not match written data";

            consume_handle.reset(); // Release slot before destroying DataBlock (avoids use-after-free)
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "write_read_succeeds_in_process", logger_module(), crypto_module(), hub_module());
}

int checksum_update_verify_succeeds()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolChecksum");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 22222;
            config.ring_buffer_capacity = 2;
            config.unit_block_size = DataBlockUnitSize::Size4K;
            config.enable_checksum = true;
            config.checksum_policy = ChecksumPolicy::EnforceOnRelease;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char payload[] = "checksum-protected payload";
            const size_t payload_len = sizeof(payload);

            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->write(payload, payload_len));
            EXPECT_TRUE(write_handle->commit(payload_len));
            EXPECT_TRUE(producer->release_write_slot(*write_handle)); // EnforceOnRelease updates checksum in release

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            EXPECT_TRUE(consume_handle->verify_checksum_slot());
            std::string read_buf(payload_len, '\0');
            EXPECT_TRUE(consume_handle->read(read_buf.data(), payload_len));
            EXPECT_EQ(std::memcmp(read_buf.data(), payload, payload_len), 0);

            consume_handle.reset(); // Release slot before destroying DataBlock
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "checksum_update_verify_succeeds", logger_module(), crypto_module(), hub_module());
}

int layout_with_checksum_and_flexible_zone_succeeds()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolLayout");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 44444;
            config.ring_buffer_capacity = 4;
            config.unit_block_size = DataBlockUnitSize::Size4K;
            config.enable_checksum = true;
            config.checksum_policy = ChecksumPolicy::EnforceOnRelease;
            config.flexible_zone_configs.push_back({"zone0", 128, -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char payload[] = "layout smoke";
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->write(payload, sizeof(payload)));
            EXPECT_TRUE(write_handle->commit(sizeof(payload)));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            EXPECT_TRUE(consume_handle->verify_checksum_slot());
            std::string read_buf(sizeof(payload), '\0');
            EXPECT_TRUE(consume_handle->read(read_buf.data(), sizeof(payload)));
            EXPECT_EQ(std::memcmp(read_buf.data(), payload, sizeof(payload)), 0);

            consume_handle.reset();
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "layout_with_checksum_and_flexible_zone_succeeds", logger_module(), crypto_module(),
        hub_module());
}

int diagnostic_handle_opens_and_accesses_header()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolDiag");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 33333;
            config.ring_buffer_capacity = 2;
            config.unit_block_size = DataBlockUnitSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr) << "open_datablock_for_diagnostic failed";

            const SharedMemoryHeader *header = diag->header();
            ASSERT_NE(header, nullptr);
            EXPECT_EQ(header->ring_buffer_capacity, 2u);
            EXPECT_EQ(header->unit_block_size, static_cast<uint32_t>(DataBlockUnitSize::Size4K));

            const SlotRWState *rw0 = diag->slot_rw_state(0);
            ASSERT_NE(rw0, nullptr);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "diagnostic_handle_opens_and_accesses_header", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::slot_protocol

namespace
{
struct SlotProtocolWorkerRegistrar
{
    SlotProtocolWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "slot_protocol")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::slot_protocol;
                if (scenario == "write_read")
                    return write_read_succeeds_in_process();
                if (scenario == "checksum")
                    return checksum_update_verify_succeeds();
                if (scenario == "layout_smoke")
                    return layout_with_checksum_and_flexible_zone_succeeds();
                if (scenario == "diagnostic_handle")
                    return diagnostic_handle_opens_and_accesses_header();
                fmt::print(stderr, "ERROR: Unknown slot_protocol scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static SlotProtocolWorkerRegistrar g_slot_protocol_registrar;
} // namespace
