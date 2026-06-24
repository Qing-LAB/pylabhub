// tests/test_layer3_datahub/workers/error_handling_workers.cpp
// DataBlock/slot error paths: timeout, wrong secret, invalid handle, bounds checks.
// Ensures recoverable errors return false/nullptr/empty instead of undefined behavior.
#include "datahub_producer_consumer_workers.h"
#include "datahub_fd_test_helper.h"  // #275-S2: fd-source pair + producer-only helpers
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include "utils/security/shm_capability_channel.hpp"  // template-site direct transport
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>
#include <unistd.h>  // ::dup, ::close for template-site rx-fd duplication
#include <vector>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

// #275-S2 migration: producer/consumer pairs use the fd-source factories
// (`create_datablock_producer_from_fd_impl` / `find_datablock_consumer_from_fd_impl`)
// wrapped by `make_fd_backed_pair` + `make_fd_backed_producer` in
// `datahub_fd_test_helper.h`.  HEP-CORE-0041 capability transport
// authenticates via fd possession, not header-stored secret —
// `cfg.shared_secret` is ignored on these paths and retires entirely
// in #275-S5.

namespace pylabhub::tests::worker::error_handling
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetDataBlockModule(); }

int acquire_consume_slot_timeout_returns_null()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrTimeout");
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto p = make_fd_backed_pair(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            ASSERT_NE(p.consumer, nullptr);
            auto& consumer = p.consumer;
            // Producer is established (p.producer) but never writes/commits →
            // consumer must get nullptr on short timeout.
            std::unique_ptr<SlotConsumeHandle> h = consumer->acquire_consume_slot(50);
            EXPECT_EQ(h.get(), nullptr);

            cleanup_test_datablock(channel);
        },
        "acquire_consume_slot_timeout_returns_null", logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// find_consumer_wrong_secret_returns_null — RETIRED 2026-06-23 (HEP-CORE-0041
// 1i-cleanup / #275-S2)
// ----------------------------------------------------------------------------
//
// Original intent: pin the C API's `find_datablock_consumer_impl(name,
// secret, ...)` rejection when the supplied secret didn't match the
// header-stored secret.  Created producer with secret S; attempted attach
// with secret S+1; expected nullptr.  This exercised the
// `memcmp(stored_secret, supplied_secret, 64) != 0` gate in
// `find_datablock_consumer_impl`.
//
// Why retired:
//   1. The gate IS the surface that HEP-CORE-0041 1i-cleanup deletes.
//      Under the new capability transport, possession of the SHM fd
//      (received via SCM_RIGHTS after a `crypto_box` challenge-response
//      handshake) IS the auth — there is no per-attach secret to mismatch.
//      `SharedMemoryHeader::shared_secret[64]` itself retires in #275-S5
//      (Core Structure Change Protocol).
//   2. The "consumer can't reach this segment" failure mode the test
//      composed at the C API now lives at the L2 capability transport:
//        * `test_layer2_service/test_shm_capability_channel.cpp::
//          ConsumerThrowsOnNonexistentEndpoint` — endpoint discovery fails
//        * `test_layer2_service/test_attach_protocol.cpp::
//          RejectsConsumerWithWrongSeckey` — wrong consumer keypair → MAC fail
//      Both are stronger than the old "wrong secret" gate (cryptographic
//      vs memcmp) and live at the layer where the gate now exists.
//   3. Constructing a synthetic-failure capability-path equivalent in this
//      L3 test would either re-test the L2 mechanism or manufacture a
//      production-unreachable failure — neither serves the test bar in
//      `docs/README/README_testing.md` §1.2.
//
// Future maintainers: do NOT reintroduce a "wrong secret" test at the
// DataBlock C API level — there is no secret in the new model.

int release_write_slot_invalid_handle_returns_false()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ErrReleaseWrite");
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto p = make_fd_backed_producer(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            auto& producer = p.producer;
            SlotWriteHandle invalid_handle; // default-constructed
            EXPECT_FALSE(producer->release_write_slot(invalid_handle));

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
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto p = make_fd_backed_pair(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            ASSERT_NE(p.consumer, nullptr);
            auto& consumer = p.consumer;
            SlotConsumeHandle invalid_handle;
            EXPECT_FALSE(consumer->release_consume_slot(invalid_handle));

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
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto p = make_fd_backed_producer(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            auto& producer = p.producer;
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            size_t slot_size = write_handle->buffer_span().size();
            ASSERT_GT(slot_size, 0u);

            char buf[1] = {'x'};
            EXPECT_FALSE(write_handle->write(buf, 0));
            EXPECT_FALSE(write_handle->write(buf, slot_size + 1));
            EXPECT_FALSE(write_handle->write(buf, 1, slot_size));

            EXPECT_TRUE(producer->release_write_slot(*write_handle));
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
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto p = make_fd_backed_producer(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            auto& producer = p.producer;
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            size_t slot_size = write_handle->buffer_span().size();
            EXPECT_FALSE(write_handle->commit(slot_size + 1));

            EXPECT_TRUE(producer->release_write_slot(*write_handle));
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
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto p = make_fd_backed_pair(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            ASSERT_NE(p.consumer, nullptr);
            auto& producer = p.producer;
            auto& consumer = p.consumer;

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
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            auto p = make_fd_backed_producer(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            auto& producer = p.producer;
            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);
            EXPECT_TRUE(write_handle->commit(0));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            cleanup_test_datablock(channel);
        },
        "double_release_write_slot_idempotent", logger_module(), crypto_module(), hub_module());
}

int slot_acquire_timeout_returns_error()
{
    return run_gtest_worker(
        []()
        {
            using namespace pylabhub::tests;
            namespace sec = pylabhub::utils::security;
            std::string channel = make_test_channel_name("ErrSlotTimeout");
            DataBlockConfig config{};
            config.policy = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 2;
            config.physical_page_size = DataBlockPageSize::Size4K;

            config.flex_zone_size = sizeof(EmptyFlexZone); // rounded up to PAGE_ALIGNMENT at creation

            // Template-typed fd-source pair: mint transport, build producer
            // + consumer over a duplicated fd (in-process substitute for
            // SCM_RIGHTS).  The non-template helper can't carry F/D type
            // tags through, so the templates are called directly.
            const size_t total = datablock_layout_total_size(config);
            auto transport = sec::create_shm_capability_producer(total);
            ASSERT_NE(transport, nullptr);
            auto producer = create_datablock_producer_from_fd<EmptyFlexZone, TestDataBlock>(
                channel, transport->borrow_fd(), DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(producer, nullptr);
            const int rx_fd = ::dup(transport->borrow_fd());
            ASSERT_GE(rx_fd, 0);
            auto consumer = find_datablock_consumer_from_fd<EmptyFlexZone, TestDataBlock>(
                channel, rx_fd, config);
            ::close(rx_fd);
            ASSERT_NE(consumer, nullptr);

            // No data written — acquiring a slot must time out.
            bool got_timeout = false;
            consumer->with_transaction<EmptyFlexZone, TestDataBlock>(
                50ms,
                [&got_timeout](ReadTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(50ms))
                    {
                        EXPECT_FALSE(result.is_ok());
                        EXPECT_EQ(result.error(), SlotAcquireError::Timeout);
                        got_timeout = true;
                        break;
                    }
                });
            EXPECT_TRUE(got_timeout);

            // Tear down in reverse: consumer → producer → transport.
            consumer.reset();
            producer.reset();
            transport.reset();
            cleanup_test_datablock(channel);
        },
        "slot_acquire_timeout_returns_error", logger_module(), crypto_module(), hub_module());
}

int sub_page_logical_size_round_trip()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SubPageRoundTrip");
            DataBlockConfig config{};
            config.policy               = DataBlockPolicy::RingBuffer;
            config.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            config.ring_buffer_capacity = 128;
            config.physical_page_size   = DataBlockPageSize::Size4K;
            config.logical_unit_size    = 64; // sub-page: 64 B slots, 4 K pages
            config.checksum_policy      = ChecksumPolicy::None;

            auto p = make_fd_backed_pair(channel, DataBlockPolicy::RingBuffer, config);
            ASSERT_NE(p.producer, nullptr);
            ASSERT_NE(p.consumer, nullptr);
            auto& producer = p.producer;
            auto& consumer = p.consumer;

            // Write a 32-byte sentinel pattern into the 64-byte slot.
            const char kPayload[32] = {
                0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};

            auto write_handle = producer->acquire_write_slot(5000);
            ASSERT_NE(write_handle, nullptr);

            auto wspan = write_handle->buffer_span();
            ASSERT_GE(wspan.size_bytes(), sizeof(kPayload));
            EXPECT_EQ(wspan.size_bytes(), 64u) << "slot buffer must equal logical_unit_size";

            std::memcpy(wspan.data(), kPayload, sizeof(kPayload));
            EXPECT_TRUE(write_handle->commit(sizeof(kPayload)));
            EXPECT_TRUE(producer->release_write_slot(*write_handle));

            auto read_handle = consumer->acquire_consume_slot(5000);
            ASSERT_NE(read_handle, nullptr);

            auto rspan = read_handle->buffer_span();
            ASSERT_GE(rspan.size_bytes(), sizeof(kPayload));
            EXPECT_EQ(0, std::memcmp(rspan.data(), kPayload, sizeof(kPayload)))
                << "data read back must match data written";

            EXPECT_TRUE(consumer->release_consume_slot(*read_handle));

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "sub_page_logical_size_round_trip", logger_module(), crypto_module(), hub_module());
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
                if (scenario == "slot_acquire_timeout_returns_error")
                    return slot_acquire_timeout_returns_error();
                if (scenario == "sub_page_logical_size_round_trip")
                    return sub_page_logical_size_round_trip();
                fmt::print(stderr, "ERROR: Unknown error_handling scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static ErrorHandlingWorkerRegistrar g_error_handling_registrar;
} // namespace
