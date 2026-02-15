// tests/test_layer3_datahub/workers/transaction_api_workers.cpp
// Transaction API tests: with_write_transaction, with_read_transaction, guards, with_typed_*, with_next_slot.
// Verifies exception safety: when lambdas throw, guards release slots.
#include "transaction_api_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>
#include <chrono>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::transaction_api
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

int with_write_transaction_success()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxAPI");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70001;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            const char payload[] = "Transaction API success test";
            with_write_transaction(*producer, 5000, [&](WriteTransactionContext &ctx) {
                std::span<std::byte> buf = ctx.slot().buffer_span();
                ASSERT_GE(buf.size(), sizeof(payload));
                std::memcpy(buf.data(), payload, sizeof(payload));
                EXPECT_TRUE(ctx.slot().commit(sizeof(payload)));
            });

            uint64_t slot_id = producer->last_slot_id();
            ASSERT_NE(slot_id, std::numeric_limits<uint64_t>::max())
                << "No slot committed (last_slot_id is INVALID)";
            with_read_transaction(*consumer, slot_id, 5000, [&](ReadTransactionContext &ctx) {
                std::span<const std::byte> buf = ctx.slot().buffer_span();
                ASSERT_GE(buf.size(), sizeof(payload));
                EXPECT_EQ(std::memcmp(buf.data(), payload, sizeof(payload)), 0);
                EXPECT_TRUE(ctx.slot().validate_read());
            });

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] with_write_transaction_success ok\n");
        },
        "with_write_transaction_success", logger_module(), crypto_module(), hub_module());
}

int with_write_transaction_timeout()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxAPITimeout");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
            config.shared_secret = 70002;
            config.ring_buffer_capacity = 1;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write and commit one slot so consumer has something to read
            with_write_transaction(*producer, 5000, [](WriteTransactionContext &ctx) {
                (void)ctx.slot().write("x", 1);
                (void)ctx.slot().commit(1);
            });
            // Consumer acquires and holds the slot (blocks producer from reusing it)
            // IMPORTANT: Scope read_handle to destroy it before consumer.reset() (Pitfall 10)
            {
                auto read_handle = consumer->acquire_consume_slot(5000);
                ASSERT_NE(read_handle, nullptr) << "Consumer must acquire slot after producer commit";
                // Writer tries with_write_transaction with short timeout -> should throw (slot busy)
                try
                {
                    with_write_transaction(*producer, 100, [](WriteTransactionContext &ctx) {
                        (void)ctx.slot().commit(1);
                    });
                    ADD_FAILURE() << "Expected with_write_transaction to throw on timeout";
                }
                catch (const std::exception &e)
                {
                    // Acquisition failed (timeout or slot busy); any exception is acceptable
                    (void)e;
                }
                // read_handle destroyed here - BEFORE producer.reset() and consumer.reset()
            }

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] with_write_transaction_timeout ok\n");
        },
        "with_write_transaction_timeout", logger_module(), crypto_module(), hub_module());
}

int WriteTransactionGuard_exception_releases_slot()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxGuardEx");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70003;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);

            bool exception_caught = false;
            try
            {
                WriteTransactionGuard guard(*producer, 5000);
                if (guard.slot())
                {
                    throw std::runtime_error(" intentional throw before commit");
                }
            }
            catch (const std::exception &)
            {
                exception_caught = true;
            }
            ASSERT_TRUE(exception_caught) << "Expected exception";

            // Slot must have been released by guard destructor. Acquire again should succeed.
            // IMPORTANT: Scope handle to destroy it before producer.reset() (Pitfall 10)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h.get(), nullptr) << "Slot should be available after guard released";
                EXPECT_TRUE(h->commit(0));
                EXPECT_TRUE(producer->release_write_slot(*h));
                // h destroyed here - BEFORE producer.reset()
            }

            producer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] WriteTransactionGuard_exception_releases_slot ok\n");
        },
        "WriteTransactionGuard_exception_releases_slot", logger_module(), crypto_module(),
        hub_module());
}

int ReadTransactionGuard_exception_releases_slot()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxReadGuardEx");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70004;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Produce one slot
            with_write_transaction(*producer, 5000, [](WriteTransactionContext &ctx) {
                uint64_t dummy = 42;
                (void)ctx.slot().write(&dummy, sizeof(dummy));
                EXPECT_TRUE(ctx.slot().commit(sizeof(dummy)));
            });
            uint64_t slot_id = producer->last_slot_id();
            ASSERT_NE(slot_id, std::numeric_limits<uint64_t>::max())
                << "No slot committed";

            bool exception_caught = false;
            try
            {
                ReadTransactionGuard guard(*consumer, slot_id, 5000);
                if (guard.slot())
                {
                    throw std::runtime_error(" intentional throw in read guard");
                }
            }
            catch (const std::exception &)
            {
                exception_caught = true;
            }
            ASSERT_TRUE(exception_caught) << "Expected exception";

            // Slot released by guard dtor; re-acquire same slot by ID (no-arg overload with
            // Latest_only would skip it because last_consumed_slot_id already equals commit_index).
            // IMPORTANT: Scope handle to destroy it before consumer.reset() (Pitfall 10)
            {
                auto h = consumer->acquire_consume_slot(slot_id, 1000);
                ASSERT_NE(h.get(), nullptr) << "Slot should be available after guard released";
                // h destroyed here - BEFORE producer.reset() and consumer.reset()
            }

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] ReadTransactionGuard_exception_releases_slot ok\n");
        },
        "ReadTransactionGuard_exception_releases_slot", logger_module(), crypto_module(),
        hub_module());
}

struct TypedPayload
{
    uint64_t seq;
    uint32_t value;
};
static_assert(std::is_trivially_copyable_v<TypedPayload>);

int with_typed_write_read_succeeds()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxTyped");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.shared_secret = 70005;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            TypedPayload written{12345, 999};
            with_typed_write<TypedPayload>(*producer, 5000, [&](TypedPayload &p) {
                p.seq = written.seq;
                p.value = written.value;
            });

            uint64_t slot_id = producer->last_slot_id();
            ASSERT_NE(slot_id, std::numeric_limits<uint64_t>::max())
                << "No slot committed";

            with_typed_read<TypedPayload>(*consumer, slot_id, 5000, [&](const TypedPayload &p) {
                EXPECT_EQ(p.seq, written.seq);
                EXPECT_EQ(p.value, written.value);
            });

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] with_typed_write_read_succeeds ok\n");
        },
        "with_typed_write_read_succeeds", logger_module(), crypto_module(), hub_module());
}

int with_next_slot_iterator()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("TxNextSlot");
            MessageHub &hub_ref = MessageHub::get_instance();
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
            config.shared_secret = 70006;
            config.ring_buffer_capacity = 4;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto producer = create_datablock_producer(hub_ref, channel,
                                                      DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
            ASSERT_NE(consumer, nullptr);

            // Write 3 slots (Single_reader yields oldest-first order)
            for (int i = 0; i < 3; ++i)
            {
                with_write_transaction(*producer, 5000, [i](WriteTransactionContext &ctx) {
                    uint64_t v = static_cast<uint64_t>(i);
                    std::memcpy(ctx.slot().buffer_span().data(), &v, sizeof(v));
                    EXPECT_TRUE(ctx.slot().commit(sizeof(v)));
                });
            }

            DataBlockSlotIterator iter = consumer->slot_iterator();
            std::vector<uint64_t> read_values;
            for (int i = 0; i < 3; ++i)
            {
                auto result = with_next_slot(iter, 2000, [&read_values](const SlotConsumeHandle &slot_handle) {
                    uint64_t v = 0;
                    EXPECT_TRUE(slot_handle.read(&v, sizeof(v)));
                    read_values.push_back(v);
                });
                ASSERT_TRUE(result.has_value()) << "with_next_slot iteration " << i << " failed";
            }
            EXPECT_EQ(read_values.size(), 3u);
            EXPECT_EQ(read_values[0], 0u) << "First slot should be 0";
            EXPECT_EQ(read_values[1], 1u) << "Second slot should be 1";
            EXPECT_EQ(read_values[2], 2u) << "Third slot should be 2";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
            fmt::print(stderr, "[transaction_api] with_next_slot_iterator ok\n");
        },
        "with_next_slot_iterator", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::transaction_api

namespace
{
struct TransactionAPIWorkerRegistrar
{
    TransactionAPIWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "transaction_api")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::transaction_api;
                if (scenario == "with_write_transaction_success")
                    return with_write_transaction_success();
                if (scenario == "with_write_transaction_timeout")
                    return with_write_transaction_timeout();
                if (scenario == "WriteTransactionGuard_exception_releases_slot")
                    return WriteTransactionGuard_exception_releases_slot();
                if (scenario == "ReadTransactionGuard_exception_releases_slot")
                    return ReadTransactionGuard_exception_releases_slot();
                if (scenario == "with_typed_write_read_succeeds")
                    return with_typed_write_read_succeeds();
                if (scenario == "with_next_slot_iterator")
                    return with_next_slot_iterator();
                fmt::print(stderr, "ERROR: Unknown transaction_api scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static TransactionAPIWorkerRegistrar g_transaction_api_registrar;
} // namespace
