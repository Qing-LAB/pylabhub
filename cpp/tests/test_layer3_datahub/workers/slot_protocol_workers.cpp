// tests/test_layer3_datahub/workers/slot_protocol_workers.cpp
#include "slot_protocol_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include "utils/slot_rw_coordinator.h"
#include "utils/logger.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>

#if PYLABHUB_IS_POSIX
#include <unistd.h>
#endif

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
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 11111;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

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

// Structured data through slot: write POD, commit, consumer reads same struct
struct SlotPayload
{
    uint64_t id;
    uint32_t value;
};

int structured_slot_data_passes()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolStructured");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 44444;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            SlotPayload written{1001, 42};
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->write(&written, sizeof(written)));
            EXPECT_TRUE(write_handle->commit(sizeof(written)));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            SlotPayload read{};
            EXPECT_TRUE(consume_handle->read(&read, sizeof(read)));
            EXPECT_EQ(read.id, written.id);
            EXPECT_EQ(read.value, written.value);

            consume_handle.reset();
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "structured_slot_data_passes", logger_module(), crypto_module(), hub_module());
}

int ring_buffer_iteration_content_verified()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolRingIter");
            MessageHub &hub_ref = MessageHub::get_instance();
            constexpr uint32_t kRingCapacity = 4;
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 66666;
            config.ring_buffer_capacity = kRingCapacity;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Interleave write/read: commit_index is "last committed"; consumer reads one per commit.
            // So we must write one, read one, etc. to verify each ring unit.
            for (uint32_t i = 0; i < kRingCapacity; ++i)
            {
                SlotPayload written{static_cast<uint64_t>(i), i * 10u};
                auto write_handle = producer->acquire_write_slot(5000);
                ASSERT_NE(write_handle, nullptr) << "acquire_write_slot failed at iteration " << i;
                EXPECT_TRUE(write_handle->write(&written, sizeof(written)));
                EXPECT_TRUE(write_handle->commit(sizeof(written)));
                EXPECT_TRUE(producer->release_write_slot(*write_handle));

                auto consume_handle = consumer->acquire_consume_slot(5000);
                ASSERT_NE(consume_handle, nullptr) << "acquire_consume_slot failed at iteration " << i;
                SlotPayload read{};
                EXPECT_TRUE(consume_handle->read(&read, sizeof(read)));
                EXPECT_EQ(read.id, static_cast<uint64_t>(i))
                    << "ring unit " << i << ": id mismatch (expected " << i << ", got " << read.id << ")";
                EXPECT_EQ(read.value, i * 10u)
                    << "ring unit " << i << ": value mismatch (expected " << (i * 10u) << ", got " << read.value << ")";
                consume_handle.reset();
            }
            LOGGER_INFO("{}", "[SlotTest:Producer] lap1 wrote 4 units ok");
            LOGGER_INFO("{}", "[SlotTest:Consumer] lap1 read 4 units ok");

            // Lap2 (wrap-around): reuse physical slots 0..3 for logical slots 4..7
            for (uint32_t i = 0; i < kRingCapacity; ++i)
            {
                uint32_t logical = kRingCapacity + i;
                SlotPayload written{static_cast<uint64_t>(logical), logical * 10u};
                auto write_handle = producer->acquire_write_slot(5000);
                ASSERT_NE(write_handle, nullptr) << "wrap lap acquire_write_slot at " << i;
                EXPECT_TRUE(write_handle->write(&written, sizeof(written)));
                EXPECT_TRUE(write_handle->commit(sizeof(written)));
                EXPECT_TRUE(producer->release_write_slot(*write_handle));

                auto consume_handle = consumer->acquire_consume_slot(5000);
                ASSERT_NE(consume_handle, nullptr) << "wrap lap acquire_consume_slot at " << i;
                SlotPayload read{};
                EXPECT_TRUE(consume_handle->read(&read, sizeof(read)));
                EXPECT_EQ(read.id, static_cast<uint64_t>(logical))
                    << "lap2 ring unit " << i << ": id mismatch";
                EXPECT_EQ(read.value, logical * 10u)
                    << "lap2 ring unit " << i << ": value mismatch";
                consume_handle.reset();
            }
            LOGGER_INFO("{}", "[SlotTest:Producer] lap2 wrote 4 units ok");
            LOGGER_INFO("{}", "[SlotTest:Consumer] lap2 read 4 units ok");

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "ring_buffer_iteration_content_verified", logger_module(), crypto_module(), hub_module());
}

int writer_blocks_on_reader_then_unblocks()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolContention");
            MessageHub &hub_ref = MessageHub::get_instance();
            // Single slot so writer and reader contend for the same slot (writer waits for reader_count to drain)
            constexpr uint32_t kRingCapacity = 1;
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 77777;
            config.ring_buffer_capacity = kRingCapacity;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // First frame: producer writes and commits so consumer can acquire
            SlotPayload first{1, 10};
            {
                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                LOGGER_INFO("{}", "[SlotTest:Producer] first write acquired");
                EXPECT_TRUE(wh->write(&first, sizeof(first)));
                EXPECT_TRUE(wh->commit(sizeof(first)));
                EXPECT_TRUE(producer->release_write_slot(*wh));
                LOGGER_INFO("{}", "[SlotTest:Producer] first write committed, released");
            }

            std::atomic<bool> writer_timed_out{false};
            std::atomic<bool> writer_succeeded_after_release{false};
            std::unique_ptr<SlotConsumeHandle> reader_handle;

            std::thread reader_thread([&]() {
                reader_handle = consumer->acquire_consume_slot(5000);
                ASSERT_NE(reader_handle.get(), nullptr);
                LOGGER_INFO("{}", "[SlotTest:Consumer] acquired consume, holding");
                // Hold the read lock long enough for writer to try and timeout
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                reader_handle.reset();
                LOGGER_INFO("{}", "[SlotTest:Consumer] released");
            });

            std::thread writer_thread([&]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let reader acquire first
                // Writer wants same slot (ring size 1); reader still holds -> must timeout
                auto wh = producer->acquire_write_slot(200);
                writer_timed_out.store(wh == nullptr, std::memory_order_release);
                if (wh)
                {
                    EXPECT_TRUE(producer->release_write_slot(*wh));
                }
                else
                {
                    LOGGER_INFO("{}", "[SlotTest:Producer] acquire_write(100) timeout (reader holds)");
                }
                // Wait for reader to release, then acquire (reader releases at ~500ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                wh = producer->acquire_write_slot(5000);
                writer_succeeded_after_release.store(wh != nullptr, std::memory_order_release);
                if (wh)
                {
                    LOGGER_INFO("{}", "[SlotTest:Producer] acquire_write(2000) ok after reader released");
                    SlotPayload second{2, 20};
                    EXPECT_TRUE(wh->write(&second, sizeof(second)));
                    EXPECT_TRUE(wh->commit(sizeof(second)));
                    EXPECT_TRUE(producer->release_write_slot(*wh));
                    LOGGER_INFO("{}", "[SlotTest:Producer] second write committed");
                }
            });

            reader_thread.join();
            writer_thread.join();

            EXPECT_TRUE(writer_timed_out.load(std::memory_order_acquire))
                << "Writer should timeout when reader holds slot (blocking/spin behavior)";
            EXPECT_TRUE(writer_succeeded_after_release.load(std::memory_order_acquire))
                << "Writer should acquire after reader releases (unblocking behavior)";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "writer_blocks_on_reader_then_unblocks", logger_module(), crypto_module(), hub_module());
}

int checksum_update_verify_succeeds()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolChecksum");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 22222;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.checksum_policy = ChecksumPolicy::Enforced;

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
            EXPECT_TRUE(producer->release_write_slot(*write_handle)); // Enforced policy updates checksum in release

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
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 44444;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.checksum_policy = ChecksumPolicy::Enforced;
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

// Canonical payload for cross-process data exchange (verifies offset, format, both processes see same data)
static constexpr uint64_t kCrossProcessExpectedId = 0xCAFEBABEULL;
static constexpr uint32_t kCrossProcessExpectedValue = 0xDEAD;

int cross_process_writer(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: cross_process_writer requires channel as argv[2]\n");
        return 1;
    }
    std::string channel(argv[2]);
    return run_gtest_worker(
        [&channel]()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 55555;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            SlotPayload written{kCrossProcessExpectedId, kCrossProcessExpectedValue};
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            LOGGER_INFO("{}", "[SlotTest:Producer] cross-process write acquired");
            EXPECT_TRUE(write_handle->write(&written, sizeof(written)));
            EXPECT_TRUE(write_handle->commit(sizeof(written)));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));
            LOGGER_INFO("{}", "[SlotTest:Producer] cross-process write committed ok");

            write_handle.reset();
            // Keep producer alive so shm persists until reader attaches; sleep then exit
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            producer.reset();
            // Do NOT cleanup: reader process will clean up
        },
        "cross_process_writer", logger_module(), crypto_module(), hub_module());
}

int cross_process_reader(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: cross_process_reader requires channel as argv[2]\n");
        return 1;
    }
    std::string channel(argv[2]);
    return run_gtest_worker(
        [&channel]()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 55555;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Let writer create and write
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);
            LOGGER_INFO("{}", "[SlotTest:Consumer] cross-process read acquired");

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            SlotPayload read{};
            EXPECT_TRUE(consume_handle->read(&read, sizeof(read)));
            EXPECT_EQ(read.id, kCrossProcessExpectedId)
                << "cross-process data exchange: id mismatch (offset/format error)";
            EXPECT_EQ(read.value, kCrossProcessExpectedValue)
                << "cross-process data exchange: value mismatch (offset/format error)";
            LOGGER_INFO("[SlotTest:Consumer] cross-process read ok id={} value={}", read.id, read.value);

            consume_handle.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "cross_process_reader", logger_module(), crypto_module(), hub_module());
}

int layout_checksum_validates_and_tamper_fails()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolLayoutChecksum");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 77777;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            SharedMemoryHeader *header = diag->header();
            ASSERT_NE(header, nullptr);

            EXPECT_TRUE(pylabhub::hub::validate_layout_checksum(header))
                << "Layout checksum should match after creation";

            // Tamper layout-defining field; checksum should fail
            uint32_t saved_cap = header->ring_buffer_capacity;
            header->ring_buffer_capacity = saved_cap + 1;
            EXPECT_FALSE(pylabhub::hub::validate_layout_checksum(header))
                << "Layout checksum must fail after tampering ring_buffer_capacity";
            header->ring_buffer_capacity = saved_cap; // restore for cleanup

            // Tamper logical_unit_size; checksum should fail
            uint32_t saved_logical = header->logical_unit_size;
            header->logical_unit_size = saved_logical + 1;
            EXPECT_FALSE(pylabhub::hub::validate_layout_checksum(header))
                << "Layout checksum must fail after tampering logical_unit_size";
            header->logical_unit_size = saved_logical;

            producer.reset();
            diag.reset();
            cleanup_test_datablock(channel);
        },
        "layout_checksum_validates_and_tamper_fails", logger_module(), crypto_module(), hub_module());
}

int physical_logical_unit_size_used_and_tested()
{
    return run_gtest_worker(
        []()
        {
            constexpr uint32_t kPhysical = 4096u; // Size4K
            constexpr uint32_t kLogicalExplicit = 8192u;

            std::string channel = make_test_channel_name("SlotProtocolPhysLogical");
            MessageHub &hub_ref = MessageHub::get_instance();

            // --- 1. logical_unit_size = 0 at config: resolved to physical (4K); header stores 4096 ---
            {
                DataBlockConfig config{};
                config.policy = DataBlockPolicy::RingBuffer;
                config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
                config.shared_secret = 11111;
                config.ring_buffer_capacity = 2;
                config.physical_page_size = DataBlockPageSize::Size4K;
                config.logical_unit_size = 0; // use physical

                auto producer = create_datablock_producer(hub_ref, channel,
                                                          DataBlockPolicy::RingBuffer, config);
                ASSERT_NE(producer, nullptr);

                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                const SharedMemoryHeader *header = diag->header();
                ASSERT_NE(header, nullptr);
                EXPECT_EQ(header->physical_page_size, kPhysical);
                EXPECT_EQ(header->logical_unit_size, kPhysical)
                    << "When logical_unit_size is 0 at config, header stores resolved physical (never 0)";

                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                EXPECT_EQ(wh->buffer_span().size(), kPhysical)
                    << "Slot buffer size must equal physical when logical_unit_size is 0";
                const char payload[] = "phys";
                EXPECT_TRUE(wh->write(payload, sizeof(payload)));
                EXPECT_TRUE(wh->commit(sizeof(payload)));
                EXPECT_TRUE(producer->release_write_slot(*wh));

                auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
                ASSERT_NE(consumer, nullptr);
                auto rh = consumer->acquire_consume_slot(5000);
                ASSERT_NE(rh, nullptr);
                EXPECT_EQ(rh->buffer_span().size(), kPhysical)
                    << "Consumer slot size must equal physical when logical_unit_size is 0";
                rh.reset();
                consumer.reset();
                producer.reset();
                diag.reset();
                cleanup_test_datablock(channel);
            }

            // --- 2. logical_unit_size = 8192 (multiple of physical): slot stride = 8192 ---
            {
                DataBlockConfig config{};
                config.policy = DataBlockPolicy::RingBuffer;
                config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
                config.shared_secret = 22222;
                config.ring_buffer_capacity = 2;
                config.physical_page_size = DataBlockPageSize::Size4K;
                config.logical_unit_size = kLogicalExplicit;

                auto producer = create_datablock_producer(hub_ref, channel,
                                                          DataBlockPolicy::RingBuffer, config);
                ASSERT_NE(producer, nullptr);

                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                const SharedMemoryHeader *header = diag->header();
                ASSERT_NE(header, nullptr);
                EXPECT_EQ(header->physical_page_size, kPhysical);
                EXPECT_EQ(header->logical_unit_size, kLogicalExplicit);

                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                EXPECT_EQ(wh->buffer_span().size(), kLogicalExplicit)
                    << "Slot buffer size must equal logical_unit_size when set";
                const char payload2[] = "logical";
                EXPECT_TRUE(wh->write(payload2, sizeof(payload2)));
                EXPECT_TRUE(wh->commit(sizeof(payload2)));
                EXPECT_TRUE(producer->release_write_slot(*wh));

                DataBlockConfig expected_config = config;
                auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret,
                                                       expected_config);
                ASSERT_NE(consumer, nullptr);
                auto rh = consumer->acquire_consume_slot(5000);
                ASSERT_NE(rh, nullptr);
                EXPECT_EQ(rh->buffer_span().size(), kLogicalExplicit)
                    << "Consumer slot size must match producer logical_unit_size";
                rh.reset();
                consumer.reset();
                producer.reset();
                diag.reset();
                cleanup_test_datablock(channel);
            }

            // --- 3. Ring iteration step = logical: two slots with distinct content (logical 8192) ---
            {
                DataBlockConfig config{};
                config.policy = DataBlockPolicy::RingBuffer;
                config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader; // read in order (slot0 then slot1)
                config.shared_secret = 33333;
                config.ring_buffer_capacity = 2;
                config.physical_page_size = DataBlockPageSize::Size4K;
                config.logical_unit_size = kLogicalExplicit;

                auto producer = create_datablock_producer(hub_ref, channel,
                                                          DataBlockPolicy::RingBuffer, config);
                ASSERT_NE(producer, nullptr);

                const char payload_s0[] = "slot0";
                const char payload_s1[] = "slot1";
                auto wh0 = producer->acquire_write_slot(5000);
                ASSERT_NE(wh0, nullptr);
                EXPECT_TRUE(wh0->write(payload_s0, sizeof(payload_s0)));
                EXPECT_TRUE(wh0->commit(sizeof(payload_s0)));
                EXPECT_TRUE(producer->release_write_slot(*wh0));

                auto wh1 = producer->acquire_write_slot(5000);
                ASSERT_NE(wh1, nullptr);
                EXPECT_TRUE(wh1->write(payload_s1, sizeof(payload_s1)));
                EXPECT_TRUE(wh1->commit(sizeof(payload_s1)));
                EXPECT_TRUE(producer->release_write_slot(*wh1));

                DataBlockConfig expected_config = config;
                auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret,
                                                       expected_config);
                ASSERT_NE(consumer, nullptr);
                auto rh0 = consumer->acquire_consume_slot(5000);
                ASSERT_NE(rh0, nullptr);
                char read0[sizeof(payload_s0)];
                EXPECT_TRUE(rh0->read(read0, sizeof(read0)));
                EXPECT_EQ(std::memcmp(read0, payload_s0, sizeof(payload_s0)), 0)
                    << "Slot 0 content must match (ring iteration step = logical)";
                rh0.reset();

                auto rh1 = consumer->acquire_consume_slot(5000);
                ASSERT_NE(rh1, nullptr);
                char read1[sizeof(payload_s1)];
                EXPECT_TRUE(rh1->read(read1, sizeof(read1)));
                EXPECT_EQ(std::memcmp(read1, payload_s1, sizeof(payload_s1)), 0)
                    << "Slot 1 content must match (ring iteration step = logical)";
                rh1.reset();

                consumer.reset();
                producer.reset();
                cleanup_test_datablock(channel);
            }
        },
        "physical_logical_unit_size_used_and_tested", logger_module(), crypto_module(), hub_module());
}

int diagnostic_handle_opens_and_accesses_header()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolDiag");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 33333;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr) << "open_datablock_for_diagnostic failed";

            const SharedMemoryHeader *header = diag->header();
            ASSERT_NE(header, nullptr);
            EXPECT_EQ(header->ring_buffer_capacity, 2u);
            EXPECT_EQ(header->physical_page_size, static_cast<uint32_t>(DataBlockPageSize::Size4K));
            EXPECT_EQ(header->logical_unit_size, static_cast<uint32_t>(DataBlockPageSize::Size4K))
                << "Default config does not set logical_unit_size; header stores resolved physical (never 0)";

            const SlotRWState *rw0 = diag->slot_rw_state(0);
            ASSERT_NE(rw0, nullptr);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "diagnostic_handle_opens_and_accesses_header", logger_module(), crypto_module(), hub_module());
}

int high_contention_wrap_around()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SlotProtocolHighContention");
            MessageHub &hub_ref = MessageHub::get_instance();
            // Ring capacity 1: reader holds the only slot; writer blocks until reader releases
            constexpr uint32_t kRingCapacity = 1;
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 88888;
            config.ring_buffer_capacity = kRingCapacity;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write one slot so consumer has something to read
            SlotPayload p0{0, 0};
            auto wh0 = producer->acquire_write_slot(5000);
            ASSERT_NE(wh0, nullptr);
            EXPECT_TRUE(wh0->write(&p0, sizeof(p0)));
            EXPECT_TRUE(wh0->commit(sizeof(p0)));
            EXPECT_TRUE(producer->release_write_slot(*wh0));
            LOGGER_INFO("{}", "[SlotTest:Producer] slot 0 written, writer will block until reader drains");

            std::atomic<bool> writer_blocked{false};
            std::atomic<bool> writer_unblocked{false};

            std::thread reader([&]() {
                auto h = consumer->acquire_consume_slot(5000);
                ASSERT_NE(h.get(), nullptr);
                LOGGER_INFO("{}", "[SlotTest:Consumer] R1 acquired slot 0, holding");
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                h.reset();
                LOGGER_INFO("{}", "[SlotTest:Consumer] R1 released");
            });
            std::thread writer([&]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Let reader acquire first
                auto wh = producer->acquire_write_slot(300);
                if (!wh)
                {
                    writer_blocked.store(true, std::memory_order_release);
                    LOGGER_INFO("{}", "[SlotTest:Producer] writer blocked (ring full, readers hold)");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(400)); // Wait for reader to release
                wh = producer->acquire_write_slot(5000);
                writer_unblocked.store(wh != nullptr, std::memory_order_release);
                if (wh)
                {
                    LOGGER_INFO("{}", "[SlotTest:Producer] writer unblocked after R1 released");
                    SlotPayload p1{1, 1};
                    EXPECT_TRUE(wh->write(&p1, sizeof(p1)));
                    EXPECT_TRUE(wh->commit(sizeof(p1)));
                    EXPECT_TRUE(producer->release_write_slot(*wh));
                }
            });

            reader.join();
            writer.join();

            EXPECT_TRUE(writer_blocked.load(std::memory_order_acquire))
                << "Writer should block when ring full and readers hold";
            EXPECT_TRUE(writer_unblocked.load(std::memory_order_acquire))
                << "Writer should unblock after readers drain";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "high_contention_wrap_around", logger_module(), crypto_module(), hub_module());
}

int zombie_writer_acquire_then_exit(int argc, char **argv)
{
#if !PYLABHUB_IS_POSIX
    (void)argc;
    (void)argv;
    fmt::print(stderr, "Zombie writer test only supported on POSIX\n");
    return 1;
#else
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: zombie_writer_acquire_then_exit requires channel as argv[2]\n");
        return 1;
    }
    std::string channel(argv[2]);
    int r = run_gtest_worker(
        [&channel]()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 99999;
            config.ring_buffer_capacity = 1; // Single slot so same physical slot reused
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto wh = producer->acquire_write_slot(5000);
            ASSERT_NE(wh.get(), nullptr);
            SlotPayload p{1, 1};
            // NODISCARD: We intentionally do not check write/commit return — process exits immediately
            // without release to simulate zombie writer; see NODISCARD_DECISIONS.md.
            (void)wh->write(&p, sizeof(p));
            (void)wh->commit(sizeof(p));
            // Do NOT release; exit without destructors so write_lock stays held
            _exit(0);
        },
        "zombie_writer_acquire_then_exit", logger_module(), crypto_module(), hub_module());
    (void)r;
    return 0; // _exit never returns
#endif
}

int zombie_writer_reclaimer(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: zombie_writer_reclaimer requires channel as argv[2]\n");
        return 1;
    }
    std::string channel(argv[2]);
    return run_gtest_worker(
        [&channel]()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 99999;
            config.ring_buffer_capacity = 1;
            config.physical_page_size = DataBlockPageSize::Size4K;

            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Let zombie exit
            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            // Should succeed via force reclaim (is_writer_alive(header, zombie_pid)==false; heartbeat stale, PID dead)
            auto wh = producer->acquire_write_slot(5000);
            ASSERT_NE(wh.get(), nullptr) << "Reclaimer should acquire after zombie exit (force reclaim)";
            LOGGER_INFO("{}", "[SlotTest:Producer] zombie writer reclaimed, write ok");
            SlotPayload p{2, 2};
            EXPECT_TRUE(wh->write(&p, sizeof(p)));
            EXPECT_TRUE(wh->commit(sizeof(p)));
            EXPECT_TRUE(producer->release_write_slot(*wh));
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "zombie_writer_reclaimer", logger_module(), crypto_module(), hub_module());
}

int policy_latest_only()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PolicyLatestOnly");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 99991;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write two slots; consumer sees only "latest" (slot 1)
            for (uint64_t i = 0; i < 2; ++i)
            {
                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                EXPECT_TRUE(wh->write(&i, sizeof(i)));
                EXPECT_TRUE(wh->commit(sizeof(i)));
                EXPECT_TRUE(producer->release_write_slot(*wh));
            }
            auto ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);
            EXPECT_EQ(ch->slot_id(), 1u) << "Latest_only: consumer gets latest committed slot";
            uint64_t v = 0;
            EXPECT_TRUE(ch->read(&v, sizeof(v)));
            EXPECT_EQ(v, 1u);
            ch.reset();
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "policy_latest_only", logger_module(), crypto_module(), hub_module());
}

int policy_single_reader()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PolicySingleReader");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 99992;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write 0,1,2; consumer must read in order 0, 1, 2
            for (uint64_t i = 0; i < 3; ++i)
            {
                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                EXPECT_TRUE(wh->write(&i, sizeof(i)));
                EXPECT_TRUE(wh->commit(sizeof(i)));
                EXPECT_TRUE(producer->release_write_slot(*wh));
            }
            for (uint64_t expected = 0; expected < 3; ++expected)
            {
                auto ch = consumer->acquire_consume_slot(5000);
                ASSERT_NE(ch, nullptr) << "Single_reader: acquire slot " << expected;
                EXPECT_EQ(ch->slot_id(), expected) << "Single_reader: read in order";
                uint64_t v = 0;
                EXPECT_TRUE(ch->read(&v, sizeof(v)));
                EXPECT_EQ(v, expected);
                ch.reset();
            }
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "policy_single_reader", logger_module(), crypto_module(), hub_module());
}

int policy_sync_reader()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PolicySyncReader");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 99993;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.consumer_sync_policy = ConsumerSyncPolicy::Sync_reader;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Sync_reader: consumer registers (via first acquire), then reads in order
            int slot = consumer->register_heartbeat();
            ASSERT_GE(slot, 0) << "Sync_reader: need heartbeat slot";

            for (uint64_t i = 0; i < 3; ++i)
            {
                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                EXPECT_TRUE(wh->write(&i, sizeof(i)));
                EXPECT_TRUE(wh->commit(sizeof(i)));
                EXPECT_TRUE(producer->release_write_slot(*wh));
            }
            for (uint64_t expected = 0; expected < 3; ++expected)
            {
                auto ch = consumer->acquire_consume_slot(5000);
                ASSERT_NE(ch, nullptr) << "Sync_reader: acquire slot " << expected;
                EXPECT_EQ(ch->slot_id(), expected) << "Sync_reader: read in order";
                uint64_t v = 0;
                EXPECT_TRUE(ch->read(&v, sizeof(v)));
                EXPECT_EQ(v, expected);
                ch.reset();
            }
            consumer->unregister_heartbeat(slot);
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "policy_sync_reader", logger_module(), crypto_module(), hub_module());
}

int high_load_single_reader()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PolicySingleReaderHighLoad");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 99994;
            config.ring_buffer_capacity = 4; // small ring to force frequent wrap-around
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // STRESS_TEST_LEVEL (Low/Medium/High) controls load: Low=5000, Medium=27500, High=50000.
            int iterations = get_stress_iterations(50000, 5000);
            uint64_t expected = 0;

            for (int i = 0; i < iterations; ++i)
            {
                // Producer: write monotonic sequence number
                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                uint64_t value = static_cast<uint64_t>(i);
                EXPECT_TRUE(wh->write(&value, sizeof(value)));
                EXPECT_TRUE(wh->commit(sizeof(value)));
                EXPECT_TRUE(producer->release_write_slot(*wh));

                // Consumer: must see slots strictly in order 0,1,2,...
                auto ch = consumer->acquire_consume_slot(5000);
                ASSERT_NE(ch, nullptr) << "high_load_single_reader: acquire slot " << expected;
                EXPECT_EQ(ch->slot_id(), expected)
                    << "high_load_single_reader: slot_id sequence broken under load";
                uint64_t read_val = 0;
                EXPECT_TRUE(ch->read(&read_val, sizeof(read_val)));
                EXPECT_EQ(read_val, expected)
                    << "high_load_single_reader: payload mismatch under load";
                ch.reset();
                ++expected;
            }

            fmt::print(stderr, "[SlotTest:HighLoadSingleReader] ok iterations={}\n", iterations);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "high_load_single_reader", logger_module(), crypto_module(), hub_module());
}

int writer_timeout_metrics_split()
{
    return run_gtest_worker(
        []()
        {
            using namespace std::chrono_literals;

            std::string channel = make_test_channel_name("WriterTimeoutMetricsSplit");
            MessageHub &hub_ref = MessageHub::get_instance();

            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 99995;
            config.ring_buffer_capacity = 1; // Single slot to force contention on same SlotRWState
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            auto *header = diag->header();
            ASSERT_NE(header, nullptr);

            // Ensure metrics start from a clean slate using the C SlotRWCoordinator API.
            ASSERT_EQ(slot_rw_reset_metrics(header), 0);

            DataBlockMetrics metrics{};
            // ---- 1) Lock-timeout path: writer times out while waiting for write_lock ----
            std::atomic<bool> holder_acquired{false};

            std::thread holder([&]() {
                auto wh = producer->acquire_write_slot(5000);
                ASSERT_NE(wh, nullptr);
                holder_acquired.store(true, std::memory_order_release);
                std::this_thread::sleep_for(300ms);
                EXPECT_TRUE(producer->release_write_slot(*wh));
            });

            // Wait until the holder has actually acquired the write slot.
            for (int i = 0; i < 50 && !holder_acquired.load(std::memory_order_acquire); ++i)
            {
                std::this_thread::sleep_for(5ms);
            }
            ASSERT_TRUE(holder_acquired.load(std::memory_order_acquire))
                << "holder should have acquired write slot";

            // Second writer: should time out waiting for write_lock (no readers involved).
            auto wh_timeout = producer->acquire_write_slot(100);
            EXPECT_EQ(wh_timeout, nullptr) << "expected timeout while waiting for write_lock";

            holder.join();

            // Read metrics via the C API: expect exactly one writer timeout, attributed to lock timeout.
            ASSERT_EQ(slot_rw_get_metrics(header, &metrics), 0);
            EXPECT_EQ(metrics.writer_timeout_count, 1u);
            EXPECT_EQ(metrics.writer_lock_timeout_count, 1u);
            EXPECT_EQ(metrics.writer_reader_timeout_count, 0u);

            // ---- 2) Reader-drain-timeout path: writer times out waiting for readers to drain ----
            ASSERT_EQ(slot_rw_reset_metrics(header), 0);

            // Produce one committed slot for the consumer to hold.
            auto wh = producer->acquire_write_slot(5000);
            ASSERT_NE(wh, nullptr);
            uint64_t value = 42;
            EXPECT_TRUE(wh->write(&value, sizeof(value)));
            EXPECT_TRUE(wh->commit(sizeof(value)));
            EXPECT_TRUE(producer->release_write_slot(*wh));

            // Consumer acquires and holds the slot, keeping reader_count > 0.
            auto ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);

            // Writer now acquires write_lock but should time out waiting for readers to drain.
            auto wh_reader_timeout = producer->acquire_write_slot(100);
            EXPECT_EQ(wh_reader_timeout, nullptr)
                << "expected timeout while waiting for readers to drain";

            // Release reader so the block can be cleaned up.
            ch.reset();

            ASSERT_EQ(slot_rw_get_metrics(header, &metrics), 0);
            EXPECT_EQ(metrics.writer_timeout_count, 1u);
            EXPECT_EQ(metrics.writer_lock_timeout_count, 0u);
            EXPECT_EQ(metrics.writer_reader_timeout_count, 1u);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "writer_timeout_metrics_split", logger_module(), crypto_module(), hub_module());
}

int policy_single_buffer_smoke()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PolicySingleBuffer");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::Single;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 88881;
            config.ring_buffer_capacity = 1;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::Single, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char first[] = "first";
            const char second[] = "second";
            auto wh = producer->acquire_write_slot(5000);
            ASSERT_NE(wh, nullptr);
            EXPECT_TRUE(wh->write(first, sizeof(first)));
            EXPECT_TRUE(wh->commit(sizeof(first)));
            EXPECT_TRUE(producer->release_write_slot(*wh));

            auto ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);
            std::string read1(sizeof(first), '\0');
            EXPECT_TRUE(ch->read(read1.data(), sizeof(first)));
            EXPECT_EQ(std::memcmp(read1.data(), first, sizeof(first)), 0) << "read first";
            ch.reset();

            wh = producer->acquire_write_slot(5000);
            ASSERT_NE(wh, nullptr);
            EXPECT_TRUE(wh->write(second, sizeof(second)));
            EXPECT_TRUE(wh->commit(sizeof(second)));
            EXPECT_TRUE(producer->release_write_slot(*wh));

            ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);
            std::string read2(sizeof(second), '\0');
            EXPECT_TRUE(ch->read(read2.data(), sizeof(second)));
            EXPECT_EQ(std::memcmp(read2.data(), second, sizeof(second)), 0) << "read overwritten second";
            ch.reset();

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "policy_single_buffer_smoke", logger_module(), crypto_module(), hub_module());
}

int policy_double_buffer_smoke()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PolicyDoubleBuffer");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::DoubleBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
            config.shared_secret = 88882;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::DoubleBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            SlotPayload frame_a{0xAAAAAAAA, 111};
            SlotPayload frame_b{0xBBBBBBBB, 222};
            auto wh_a = producer->acquire_write_slot(5000);
            ASSERT_NE(wh_a, nullptr);
            EXPECT_TRUE(wh_a->write(&frame_a, sizeof(frame_a)));
            EXPECT_TRUE(wh_a->commit(sizeof(frame_a)));
            EXPECT_TRUE(producer->release_write_slot(*wh_a));

            auto wh_b = producer->acquire_write_slot(5000);
            ASSERT_NE(wh_b, nullptr);
            EXPECT_TRUE(wh_b->write(&frame_b, sizeof(frame_b)));
            EXPECT_TRUE(wh_b->commit(sizeof(frame_b)));
            EXPECT_TRUE(producer->release_write_slot(*wh_b));

            auto ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);
            EXPECT_EQ(ch->slot_id(), 0u) << "Single_reader: first slot";
            SlotPayload read_a{};
            EXPECT_TRUE(ch->read(&read_a, sizeof(read_a)));
            EXPECT_EQ(read_a.id, frame_a.id);
            EXPECT_EQ(read_a.value, frame_a.value);
            ch.reset();

            ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);
            EXPECT_EQ(ch->slot_id(), 1u) << "Single_reader: second slot";
            SlotPayload read_b{};
            EXPECT_TRUE(ch->read(&read_b, sizeof(read_b)));
            EXPECT_EQ(read_b.id, frame_b.id);
            EXPECT_EQ(read_b.value, frame_b.value);
            ch.reset();

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "policy_double_buffer_smoke", logger_module(), crypto_module(), hub_module());
}

int checksum_manual_policy()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ChecksumManual");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 88883;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.checksum_policy = ChecksumPolicy::Manual;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char payload[] = "manual checksum payload";
            auto wh = producer->acquire_write_slot(5000);
            ASSERT_NE(wh, nullptr);
            EXPECT_TRUE(wh->write(payload, sizeof(payload)));
            EXPECT_TRUE(wh->update_checksum_slot()) << "Manual: producer must update before commit";
            EXPECT_TRUE(wh->commit(sizeof(payload)));
            EXPECT_TRUE(producer->release_write_slot(*wh));

            auto ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);
            EXPECT_TRUE(ch->verify_checksum_slot()) << "Manual: consumer must verify before read";
            std::string read_buf(sizeof(payload), '\0');
            EXPECT_TRUE(ch->read(read_buf.data(), sizeof(payload)));
            EXPECT_EQ(std::memcmp(read_buf.data(), payload, sizeof(payload)), 0);
            ch.reset();

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "checksum_manual_policy", logger_module(), crypto_module(), hub_module());
}

int physical_page_size_4m_smoke()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("PhysicalPage4M");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 88884;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4M;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            SlotPayload written{0x4D345934, 4096};
            auto wh = producer->acquire_write_slot(5000);
            ASSERT_NE(wh, nullptr);
            EXPECT_TRUE(wh->write(&written, sizeof(written)));
            EXPECT_TRUE(wh->commit(sizeof(written)));
            EXPECT_TRUE(producer->release_write_slot(*wh));

            auto ch = consumer->acquire_consume_slot(5000);
            ASSERT_NE(ch, nullptr);
            SlotPayload read{};
            EXPECT_TRUE(ch->read(&read, sizeof(read)));
            EXPECT_EQ(read.id, written.id);
            EXPECT_EQ(read.value, written.value);
            ch.reset();

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "physical_page_size_4m_smoke", logger_module(), crypto_module(), hub_module());
}

int flexible_zone_multi_zones()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("FlexZoneMulti");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 88885;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 64, -1});
            config.flexible_zone_configs.push_back({"zone1", 64, -1});

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char data0[] = "zone0-data";
            const char data1[] = "zone1-data";
            std::span<std::byte> z0 = producer->flexible_zone_span(0);
            std::span<std::byte> z1 = producer->flexible_zone_span(1);
            ASSERT_GE(z0.size(), sizeof(data0));
            ASSERT_GE(z1.size(), sizeof(data1));
            std::memcpy(z0.data(), data0, sizeof(data0));
            std::memcpy(z1.data(), data1, sizeof(data1));

            std::span<const std::byte> cz0 = consumer->flexible_zone_span(0);
            std::span<const std::byte> cz1 = consumer->flexible_zone_span(1);
            ASSERT_GE(cz0.size(), sizeof(data0));
            ASSERT_GE(cz1.size(), sizeof(data1));
            EXPECT_EQ(std::memcmp(cz0.data(), data0, sizeof(data0)), 0) << "zone0 content";
            EXPECT_EQ(std::memcmp(cz1.data(), data1, sizeof(data1)), 0) << "zone1 content";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "flexible_zone_multi_zones", logger_module(), crypto_module(), hub_module());
}

int flexible_zone_with_spinlock()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("FlexZoneSpinlock");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 88886;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;
            config.flexible_zone_configs.push_back({"zone0", 64, 0}); // spinlock index 0

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char payload[] = "spinlock-protected";
            SharedSpinLock sl_prod = producer->get_spinlock(0);
            sl_prod.lock();
            std::span<std::byte> z0 = producer->flexible_zone_span(0);
            ASSERT_GE(z0.size(), sizeof(payload));
            std::memcpy(z0.data(), payload, sizeof(payload));
            sl_prod.unlock();

            SharedSpinLock sl_cons = consumer->get_spinlock(0);
            sl_cons.lock();
            std::span<const std::byte> cz0 = consumer->flexible_zone_span(0);
            ASSERT_GE(cz0.size(), sizeof(payload));
            EXPECT_EQ(std::memcmp(cz0.data(), payload, sizeof(payload)), 0) << "zone with spinlock";
            sl_cons.unlock();

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "flexible_zone_with_spinlock", logger_module(), crypto_module(), hub_module());
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
                if (scenario == "structured_slot_data_passes")
                    return structured_slot_data_passes();
                if (scenario == "checksum")
                    return checksum_update_verify_succeeds();
                if (scenario == "layout_smoke")
                    return layout_with_checksum_and_flexible_zone_succeeds();
                if (scenario == "layout_checksum")
                    return layout_checksum_validates_and_tamper_fails();
                if (scenario == "physical_logical_unit_size")
                    return physical_logical_unit_size_used_and_tested();
                if (scenario == "ring_buffer_iteration")
                    return ring_buffer_iteration_content_verified();
                if (scenario == "writer_blocks_on_reader_then_unblocks")
                    return writer_blocks_on_reader_then_unblocks();
                if (scenario == "diagnostic_handle")
                    return diagnostic_handle_opens_and_accesses_header();
                if (scenario == "cross_process_writer")
                    return cross_process_writer(argc, argv);
                if (scenario == "cross_process_reader")
                    return cross_process_reader(argc, argv);
                if (scenario == "high_contention_wrap_around")
                    return high_contention_wrap_around();
                if (scenario == "zombie_writer_acquire_then_exit")
                    return zombie_writer_acquire_then_exit(argc, argv);
                if (scenario == "zombie_writer_reclaimer")
                    return zombie_writer_reclaimer(argc, argv);
                if (scenario == "policy_latest_only")
                    return policy_latest_only();
                if (scenario == "policy_single_reader")
                    return policy_single_reader();
                if (scenario == "policy_sync_reader")
                    return policy_sync_reader();
                if (scenario == "high_load_single_reader")
                    return high_load_single_reader();
                if (scenario == "writer_timeout_metrics_split")
                    return writer_timeout_metrics_split();
                if (scenario == "policy_single_buffer_smoke")
                    return policy_single_buffer_smoke();
                if (scenario == "policy_double_buffer_smoke")
                    return policy_double_buffer_smoke();
                if (scenario == "checksum_manual_policy")
                    return checksum_manual_policy();
                if (scenario == "physical_page_size_4m_smoke")
                    return physical_page_size_4m_smoke();
                if (scenario == "flexible_zone_multi_zones")
                    return flexible_zone_multi_zones();
                if (scenario == "flexible_zone_with_spinlock")
                    return flexible_zone_with_spinlock();
                fmt::print(stderr, "ERROR: Unknown slot_protocol scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static SlotProtocolWorkerRegistrar g_slot_protocol_registrar;
} // namespace
