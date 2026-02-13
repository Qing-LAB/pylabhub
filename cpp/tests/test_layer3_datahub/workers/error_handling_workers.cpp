// tests/test_layer3_datahub/workers/error_handling_workers.cpp
// DataBlock/slot error paths: timeout, wrong secret, invalid handle, bounds checks.
// Ensures recoverable errors return false/nullptr/empty instead of undefined behavior.
#include "error_handling_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>
#include <vector>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::error_handling
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

int acquire_consume_slot_timeout_returns_null()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrTimeout");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60001;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);
            // Producer never writes/commits → consumer must get nullptr on short timeout
            std::unique_ptr<SlotConsumeHandle> h = consumer->acquire_consume_slot(50);
            EXPECT_EQ(h.get(), nullptr);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "acquire_consume_slot_timeout_returns_null", logger_module(), crypto_module(), hub_module());
}

int find_consumer_wrong_secret_returns_null()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrWrongSecret");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60002;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            uint64_t wrong_secret = config.shared_secret + 1;
            auto consumer = find_datablock_consumer(hub_ref, channel, wrong_secret, config);
            EXPECT_EQ(consumer.get(), nullptr);

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "find_consumer_wrong_secret_returns_null", logger_module(), crypto_module(), hub_module());
}

int release_write_slot_invalid_handle_returns_false()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrReleaseWrite");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60003;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            SlotWriteHandle invalid_handle; // default-constructed
            EXPECT_FALSE(producer->release_write_slot(invalid_handle));

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "release_write_slot_invalid_handle_returns_false", logger_module(), crypto_module(),
        hub_module());
}

int release_consume_slot_invalid_handle_returns_false()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrReleaseConsume");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60004;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);
            SlotConsumeHandle invalid_handle;
            EXPECT_FALSE(consumer->release_consume_slot(invalid_handle));

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "release_consume_slot_invalid_handle_returns_false", logger_module(), crypto_module(),
        hub_module());
}

int write_bounds_return_false()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrWriteBounds");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60005;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            size_t slot_size = write_handle->buffer_span().size();
            ASSERT_GT(slot_size, 0u);

            char buf[1] = {'x'};
            EXPECT_FALSE(write_handle->write(buf, 0));
            EXPECT_FALSE(write_handle->write(buf, slot_size + 1));
            EXPECT_FALSE(write_handle->write(buf, 1, slot_size));

            EXPECT_TRUE(producer->release_write_slot(*write_handle));
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "write_bounds_return_false", logger_module(), crypto_module(), hub_module());
}

int commit_bounds_return_false()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrCommitBounds");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60006;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            size_t slot_size = write_handle->buffer_span().size();
            EXPECT_FALSE(write_handle->commit(slot_size + 1));

            EXPECT_TRUE(producer->release_write_slot(*write_handle));
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "commit_bounds_return_false", logger_module(), crypto_module(), hub_module());
}

int read_bounds_return_false()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrReadBounds");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60007;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char payload[] = "x";
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->write(payload, 1));
            EXPECT_TRUE(write_handle->commit(1));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto consume_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(consume_handle, nullptr);
            size_t slot_size = consume_handle->buffer_span().size();
            std::vector<char> buf(slot_size + 1, 0);
            EXPECT_FALSE(consume_handle->read(buf.data(), 0));
            EXPECT_FALSE(consume_handle->read(buf.data(), slot_size + 1));
            EXPECT_FALSE(consume_handle->read(buf.data(), 1, slot_size));

            consume_handle.reset();
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "read_bounds_return_false", logger_module(), crypto_module(), hub_module());
}

int double_release_write_slot_idempotent()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrDoubleRelease");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60008;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->commit(0));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "double_release_write_slot_idempotent", logger_module(), crypto_module(), hub_module());
}

int slot_iterator_try_next_timeout_returns_not_ok()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrIteratorTimeout");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.shared_secret = 60009;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);
            auto it = consumer->slot_iterator();
            auto res = it.try_next(50);
            EXPECT_FALSE(res.ok);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "slot_iterator_try_next_timeout_returns_not_ok", logger_module(), crypto_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::error_handling

namespace
{
struct ErrorHandlingWorkerRegistrar
{
    ErrorHandlingWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "error_handling")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::error_handling;
                if (scenario == "acquire_consume_slot_timeout_returns_null")
                    return acquire_consume_slot_timeout_returns_null();
                if (scenario == "find_consumer_wrong_secret_returns_null")
                    return find_consumer_wrong_secret_returns_null();
                if (scenario == "release_write_slot_invalid_handle_returns_false")
                    return release_write_slot_invalid_handle_returns_false();
                if (scenario == "release_consume_slot_invalid_handle_returns_false")
                    return release_consume_slot_invalid_handle_returns_false();
                if (scenario == "write_bounds_return_false")
                    return write_bounds_return_false();
                if (scenario == "commit_bounds_return_false")
                    return commit_bounds_return_false();
                if (scenario == "read_bounds_return_false")
                    return read_bounds_return_false();
                if (scenario == "double_release_write_slot_idempotent")
                    return double_release_write_slot_idempotent();
                if (scenario == "slot_iterator_try_next_timeout_returns_not_ok")
                    return slot_iterator_try_next_timeout_returns_not_ok();
                fmt::print(stderr, "ERROR: Unknown error_handling scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static ErrorHandlingWorkerRegistrar g_error_handling_registrar;
} // namespace
