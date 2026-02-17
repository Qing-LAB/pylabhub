// tests/test_layer3_datahub/workers/datahub_c_api_draining_workers.cpp
//
// SlotState::DRAINING protocol tests.
//
// These tests verify that DRAINING is entered when the writer wraps around a COMMITTED slot
// (and a reader is holding it), that new readers are rejected while DRAINING, that the slot
// correctly resolves to WRITING/COMMITTED after drain completes, and that a drain timeout
// restores the slot to COMMITTED.
//
// Test strategy:
// - Tests that require concurrent write + read use std::thread within the worker process.
// - Writer thread calls acquire_write_slot(5000) and blocks on drain. Main thread holds or
//   releases the read slot as needed, then joins the writer thread.
// - DiagnosticHandle is used to inspect raw SlotRWState during the drain window.
// - Secret numbers start at 72001 to avoid conflicts with other test suites.
//
// Test list:
//   1. draining_state_entered_on_wraparound   — 1-slot ring; reader held; writer wraps → DRAINING
//   2. draining_rejects_new_readers           — while DRAINING, acquire_consume_slot returns nullptr
//   3. draining_resolves_after_reader_release — reader release → drain completes → COMMITTED; consumer reads ok
//   4. draining_timeout_restores_committed    — short writer timeout; slot restored to COMMITTED; data still readable
//   5. no_reader_races_on_clean_wraparound    — N full write+read cycles; reader_race_detected == 0

#include "datahub_c_api_draining_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::c_api_draining
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// Helper: build a 1-slot Latest_only config (forces wraparound on second write)
static DataBlockConfig make_one_slot_config(uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 1;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

// Helper: spin-poll until slot_state == expected or timeout_ms elapses.
// Returns true if expected state was observed before timeout.
static bool wait_for_slot_state(SlotRWState *rw, SlotRWState::SlotState expected,
                                int timeout_ms = 2000)
{
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (rw->slot_state.load(std::memory_order_acquire) == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// ============================================================================
// 1. draining_state_entered_on_wraparound
// 1-slot ring: write+commit, consumer holds the slot, producer wraps around →
// slot_state should transition to DRAINING while reader_count > 0.
// ============================================================================

int draining_state_entered_on_wraparound()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("DrainState");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_one_slot_config(72001);

            auto producer = create_datablock_producer_impl(hub, channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                         &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            // Open diagnostic handle to inspect raw slot state
            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            SlotRWState *rw = diag->slot_rw_state(0);
            ASSERT_NE(rw, nullptr);

            // First write: FREE → WRITING → COMMITTED
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                uint64_t val = 0xAA;
                std::memcpy(h->buffer_span().data(), &val, sizeof(val));
                EXPECT_TRUE(h->commit(sizeof(val)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Consumer acquires and holds the slot (reader_count = 1)
            auto rh = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh, nullptr);

            // Writer thread: wrap around → should enter DRAINING (5 s timeout)
            std::thread writer([&]()
            {
                auto h = producer->acquire_write_slot(5000);
                if (h)
                {
                    (void)h->commit(0);
                    (void)producer->release_write_slot(*h);
                }
            });

            // Poll until DRAINING (or timeout)
            bool saw_draining = wait_for_slot_state(rw, SlotRWState::SlotState::DRAINING);
            EXPECT_TRUE(saw_draining)
                << "slot_state should be DRAINING while writer waits for reader to drain";

            // Release reader → drain completes → writer proceeds
            (void)consumer->release_consume_slot(*rh);
            writer.join();

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "draining_state_entered_on_wraparound", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. draining_rejects_new_readers
// While a slot is DRAINING (writer waiting), a second acquire_consume_slot must
// return nullptr (NOT_READY) because slot_state != COMMITTED.
// ============================================================================

int draining_rejects_new_readers()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("DrainReject");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_one_slot_config(72002);

            auto producer = create_datablock_producer_impl(hub, channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                         &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            SlotRWState *rw = diag->slot_rw_state(0);
            ASSERT_NE(rw, nullptr);

            // First write
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                uint64_t val = 0xBB;
                std::memcpy(h->buffer_span().data(), &val, sizeof(val));
                EXPECT_TRUE(h->commit(sizeof(val)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Consumer 1 holds the slot
            auto rh1 = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh1, nullptr);

            // Writer thread: wrap around, enter DRAINING
            std::thread writer([&]()
            {
                auto h = producer->acquire_write_slot(5000);
                if (h)
                {
                    (void)h->commit(0);
                    (void)producer->release_write_slot(*h);
                }
            });

            // Wait for DRAINING
            bool saw_draining = wait_for_slot_state(rw, SlotRWState::SlotState::DRAINING);
            ASSERT_TRUE(saw_draining) << "Expected DRAINING state before testing reader rejection";

            // While DRAINING, a new acquire_consume_slot must return nullptr (not COMMITTED)
            auto rh2 = consumer->acquire_consume_slot(0);
            EXPECT_EQ(rh2, nullptr)
                << "acquire_consume_slot must return nullptr when slot_state == DRAINING";

            // Release first reader → writer can proceed
            (void)consumer->release_consume_slot(*rh1);
            writer.join();

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "draining_rejects_new_readers", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. draining_resolves_after_reader_release
// After the draining reader releases, DRAINING → WRITING → COMMITTED, and a
// subsequent consumer acquire succeeds and reads the new data written by the writer.
// ============================================================================

int draining_resolves_after_reader_release()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("DrainResolve");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_one_slot_config(72003);

            auto producer = create_datablock_producer_impl(hub, channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                         &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            SlotRWState *rw = diag->slot_rw_state(0);
            ASSERT_NE(rw, nullptr);

            // First write: value=111
            const uint64_t kOldValue = 111;
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                std::memcpy(h->buffer_span().data(), &kOldValue, sizeof(kOldValue));
                EXPECT_TRUE(h->commit(sizeof(kOldValue)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Consumer holds slot (reader_count = 1)
            auto rh = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh, nullptr);

            // Writer thread: wrap around, write new value=222 after drain
            const uint64_t kNewValue = 222;
            std::thread writer([&]()
            {
                auto h = producer->acquire_write_slot(5000);
                if (h)
                {
                    std::memcpy(h->buffer_span().data(), &kNewValue, sizeof(kNewValue));
                    (void)h->commit(sizeof(kNewValue));
                    (void)producer->release_write_slot(*h);
                }
            });

            // Wait for DRAINING
            bool saw_draining = wait_for_slot_state(rw, SlotRWState::SlotState::DRAINING);
            ASSERT_TRUE(saw_draining) << "Expected DRAINING before releasing reader";

            // Release reader → drain completes → writer writes value=222 and commits
            (void)consumer->release_consume_slot(*rh);
            writer.join();

            // Slot is now COMMITTED with new data; consumer reads value=222
            auto rh2 = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh2, nullptr) << "Consumer should see COMMITTED slot after drain resolved";
            uint64_t read_val = 0;
            std::memcpy(&read_val, rh2->buffer_span().data(), sizeof(read_val));
            EXPECT_EQ(read_val, kNewValue) << "Consumer should read new value written after drain";
            (void)consumer->release_consume_slot(*rh2);

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "draining_resolves_after_reader_release", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 4. draining_timeout_restores_committed
// If the writer times out while draining (reader holds slot too long), the slot
// must be restored to COMMITTED (not left in DRAINING) and write_lock must be
// cleared. Verified via DiagnosticHandle raw state inspection.
// ============================================================================

int draining_timeout_restores_committed()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("DrainTimeout");
            MessageHub &hub = MessageHub::get_instance();
            auto cfg = make_one_slot_config(72004);

            auto producer = create_datablock_producer_impl(hub, channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                         &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            SlotRWState *rw = diag->slot_rw_state(0);
            ASSERT_NE(rw, nullptr);

            const uint64_t kValue = 0xCAFEBABE;

            // Write and commit original value
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr);
                std::memcpy(h->buffer_span().data(), &kValue, sizeof(kValue));
                EXPECT_TRUE(h->commit(sizeof(kValue)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            ASSERT_EQ(rw->slot_state.load(std::memory_order_acquire),
                      SlotRWState::SlotState::COMMITTED)
                << "Slot must be COMMITTED before consumer acquires";

            // Consumer acquires and holds the slot (reader_count = 1)
            auto rh = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh, nullptr);

            // Producer tries to acquire with a very short timeout (10 ms) while reader holds
            // the slot. This forces the drain to time out.
            auto wh = producer->acquire_write_slot(10);
            EXPECT_EQ(wh, nullptr)
                << "acquire_write_slot must return nullptr when drain times out";

            // After timeout: slot_state must be COMMITTED (restored) and write_lock must be 0.
            EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                      SlotRWState::SlotState::COMMITTED)
                << "slot_state must be COMMITTED after drain timeout (not DRAINING)";
            EXPECT_EQ(rw->write_lock.load(std::memory_order_acquire), 0u)
                << "write_lock must be released (0) after drain timeout";

            // Release the reader
            (void)consumer->release_consume_slot(*rh);

            // Slot is still COMMITTED with the original data; write_lock still 0.
            EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                      SlotRWState::SlotState::COMMITTED)
                << "slot_state must remain COMMITTED after reader release";
            EXPECT_EQ(rw->write_lock.load(std::memory_order_acquire), 0u)
                << "write_lock must remain 0 after reader release";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "draining_timeout_restores_committed", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 5. no_reader_races_on_clean_wraparound
// Fill+read a 2-slot ring N times. Because DRAINING is set before any new reader
// can observe a half-overwritten slot, reader_race_detected must remain zero for
// clean (single-threaded) wraparounds.
// ============================================================================

int no_reader_races_on_clean_wraparound()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("DrainNoRace");
            MessageHub &hub = MessageHub::get_instance();

            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
            cfg.shared_secret = 72005;
            cfg.ring_buffer_capacity = 2; // 2 slots: forces wrap every 2 writes
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;

            auto producer = create_datablock_producer_impl(hub, channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                         &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            constexpr int kIterations = 20;
            for (int i = 0; i < kIterations; ++i)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr) << "Write acquire failed at iteration " << i;
                uint64_t val = static_cast<uint64_t>(i);
                std::memcpy(h->buffer_span().data(), &val, sizeof(val));
                EXPECT_TRUE(h->commit(sizeof(val)));
                EXPECT_TRUE(producer->release_write_slot(*h));

                auto rh = consumer->acquire_consume_slot(1000);
                ASSERT_NE(rh, nullptr) << "Read acquire failed at iteration " << i;
                (void)consumer->release_consume_slot(*rh);
            }

            // No reader races should have occurred on clean single-threaded wraparounds
            DataBlockMetrics metrics{};
            ASSERT_EQ(producer->get_metrics(metrics), 0);
            EXPECT_EQ(metrics.reader_race_detected, 0u)
                << "reader_race_detected must be 0 for clean single-threaded wraparounds";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "no_reader_races_on_clean_wraparound", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 6. single_reader_ring_full_blocks_not_draining
// For Single_reader, the ring-full check (write_index - read_index < capacity)
// fires BEFORE write_index is advanced, so the writer is blocked by ring-full
// and never reaches the slot a reader is holding. DRAINING is never triggered.
//
// Proof check (code path, src/utils/data_block.cpp):
//   ring-full spin → {write_index.load(), read_index.load()} checked first
//   → write_index.fetch_add(1) only after check passes
//   → acquire_write() only called after fetch_add
//   → DRAINING only entered inside acquire_write() if slot_state==COMMITTED
//   With Single_reader: fetch_add cannot reach the held slot while read_index
//   hasn't advanced (reader's release is what advances read_index).
// ============================================================================

int single_reader_ring_full_blocks_not_draining()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SRDrainNever");
            MessageHub &hub = MessageHub::get_instance();

            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
            cfg.shared_secret = 72006;
            cfg.ring_buffer_capacity = 2;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;

            auto producer = create_datablock_producer_impl(hub, channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                         &cfg, nullptr, nullptr);
            ASSERT_NE(consumer, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);
            SlotRWState *rw0 = diag->slot_rw_state(0);
            SlotRWState *rw1 = diag->slot_rw_state(1);
            ASSERT_NE(rw0, nullptr);
            ASSERT_NE(rw1, nullptr);

            // Fill the ring: write+commit slot 0 and slot 1
            // After this: write_index=2, read_index=0 → ring full (2-0=2=capacity)
            for (int i = 0; i < 2; ++i)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr) << "fill write " << i;
                uint64_t v = static_cast<uint64_t>(i);
                std::memcpy(h->buffer_span().data(), &v, sizeof(v));
                EXPECT_TRUE(h->commit(sizeof(v)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Consumer acquires slot 0 (read_index stays 0 — only advances on release)
            auto rh = consumer->acquire_consume_slot(1000);
            ASSERT_NE(rh, nullptr);

            // Reset metrics so we get a clean baseline
            ASSERT_EQ(producer->reset_metrics(), 0);

            // Producer tries to write with short timeout:
            // ring-full check sees write_index(2) - read_index(0) = 2 = capacity → blocked.
            // Should timeout on ring-full, NOT on drain.
            auto wh = producer->acquire_write_slot(50);
            EXPECT_EQ(wh, nullptr)
                << "Producer must time out (ring full) while consumer holds slot 0";

            // Verify the timeout was ring-full, NOT drain:
            // writer_reader_timeout_count is only incremented in the drain spin timeout path
            // (inside acquire_write()). Ring-full timeout only increments writer_timeout_count.
            DataBlockMetrics m{};
            ASSERT_EQ(producer->get_metrics(m), 0);
            EXPECT_GT(m.writer_timeout_count, 0u)
                << "writer_timeout_count must be > 0 (ring-full timeout occurred)";
            EXPECT_EQ(m.writer_reader_timeout_count, 0u)
                << "writer_reader_timeout_count must be 0 — ring-full blocked before any drain attempt";

            // Verify no slot entered DRAINING state — writer never reached them
            EXPECT_EQ(rw0->slot_state.load(std::memory_order_acquire),
                      SlotRWState::SlotState::COMMITTED)
                << "Slot 0 must remain COMMITTED; DRAINING was never entered";
            EXPECT_EQ(rw1->slot_state.load(std::memory_order_acquire),
                      SlotRWState::SlotState::COMMITTED)
                << "Slot 1 must remain COMMITTED; DRAINING was never entered";

            (void)consumer->release_consume_slot(*rh);
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(channel);
        },
        "single_reader_ring_full_blocks_not_draining", logger_module(), crypto_module(),
        hub_module());
}

// ============================================================================
// 7. sync_reader_ring_full_blocks_not_draining
// For Sync_reader, read_index = min(all consumer positions). Even with multiple
// consumers at different positions, the ring-full check blocks the writer before
// it can reach any slot currently held by any consumer. DRAINING is never triggered.
// ============================================================================

int sync_reader_ring_full_blocks_not_draining()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("SyncDrainNever");
            MessageHub &hub = MessageHub::get_instance();

            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Sync_reader;
            cfg.shared_secret = 72007;
            cfg.ring_buffer_capacity = 3;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.checksum_policy = ChecksumPolicy::None;

            auto producer = create_datablock_producer_impl(hub, channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);
            // Two independent consumers (each registers its own heartbeat/position slot)
            auto consumer1 = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                          &cfg, nullptr, nullptr);
            ASSERT_NE(consumer1, nullptr);
            auto consumer2 = find_datablock_consumer_impl(hub, channel, cfg.shared_secret,
                                                          &cfg, nullptr, nullptr);
            ASSERT_NE(consumer2, nullptr);

            auto diag = open_datablock_for_diagnostic(channel);
            ASSERT_NE(diag, nullptr);

            // Fill the ring: 3 slots committed
            // write_index=3, commit_index=2, read_index=0 → ring full (3-0=3=capacity)
            for (int i = 0; i < 3; ++i)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_NE(h, nullptr) << "fill write " << i;
                uint64_t v = static_cast<uint64_t>(i);
                std::memcpy(h->buffer_span().data(), &v, sizeof(v));
                EXPECT_TRUE(h->commit(sizeof(v)));
                EXPECT_TRUE(producer->release_write_slot(*h));
            }

            // Consumer 1 acquires slot 0 (next_pos stays 0 until release)
            auto rh1 = consumer1->acquire_consume_slot(1000);
            ASSERT_NE(rh1, nullptr);

            // Consumer 2 acquires slot 0 (same slot — Sync_reader each consumer is independent)
            auto rh2 = consumer2->acquire_consume_slot(1000);
            ASSERT_NE(rh2, nullptr);

            // read_index = min(next_pos1=0, next_pos2=0) = 0 → ring still full

            ASSERT_EQ(producer->reset_metrics(), 0);

            // Producer tries to write with short timeout → ring-full, NOT drain
            auto wh = producer->acquire_write_slot(50);
            EXPECT_EQ(wh, nullptr)
                << "Producer must time out (ring full) while consumers hold slots";

            DataBlockMetrics m{};
            ASSERT_EQ(producer->get_metrics(m), 0);
            EXPECT_GT(m.writer_timeout_count, 0u)
                << "writer_timeout_count must be > 0 (ring-full timeout occurred)";
            EXPECT_EQ(m.writer_reader_timeout_count, 0u)
                << "writer_reader_timeout_count must be 0 — ring-full blocked before any drain";

            // No slot should have entered DRAINING
            for (uint32_t si = 0; si < 3; ++si)
            {
                SlotRWState *rw = diag->slot_rw_state(si);
                ASSERT_NE(rw, nullptr);
                EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                          SlotRWState::SlotState::COMMITTED)
                    << "Slot " << si << " must remain COMMITTED; DRAINING was never entered";
            }

            (void)consumer1->release_consume_slot(*rh1);
            (void)consumer2->release_consume_slot(*rh2);
            producer.reset();
            consumer1.reset();
            consumer2.reset();
            cleanup_test_datablock(channel);
        },
        "sync_reader_ring_full_blocks_not_draining", logger_module(), crypto_module(),
        hub_module());
}

} // namespace pylabhub::tests::worker::c_api_draining

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct CApiDrainingWorkerRegistrar
{
    CApiDrainingWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "c_api_draining")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::c_api_draining;
                if (scenario == "draining_state_entered_on_wraparound")
                    return draining_state_entered_on_wraparound();
                if (scenario == "draining_rejects_new_readers")
                    return draining_rejects_new_readers();
                if (scenario == "draining_resolves_after_reader_release")
                    return draining_resolves_after_reader_release();
                if (scenario == "draining_timeout_restores_committed")
                    return draining_timeout_restores_committed();
                if (scenario == "no_reader_races_on_clean_wraparound")
                    return no_reader_races_on_clean_wraparound();
                if (scenario == "single_reader_ring_full_blocks_not_draining")
                    return single_reader_ring_full_blocks_not_draining();
                if (scenario == "sync_reader_ring_full_blocks_not_draining")
                    return sync_reader_ring_full_blocks_not_draining();
                fmt::print(stderr, "ERROR: Unknown c_api_draining scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static CApiDrainingWorkerRegistrar s_c_api_draining_registrar;
} // namespace
