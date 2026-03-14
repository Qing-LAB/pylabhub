// Layer 2: SlotRWCoordinator API tests
//
// Purpose:
//   Exercise the pure C SlotRWCoordinator API (utils/slot_rw_coordinator.h) against a
//   single in-process SlotRWState + SharedMemoryHeader, without DataBlock,
//   shared memory, or factories. This validates the core slot protocol and
//   metrics mapping in isolation so higher-level tests (DataBlock, factories,
//   MessageHub) can rely on a well-tested lower layer.

#include "shared_test_helpers.h"
#include "utils/data_block.hpp"
#include "utils/slot_rw_coordinator.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace pylabhub::hub;

namespace
{

// Helper that zero-initializes a header and one SlotRWState in plain memory.
struct SlotFixture
{
    SharedMemoryHeader header{};
    SlotRWState slot{};

    SlotFixture()
    {
        // Minimal sane initialization: capacity = 1, physical_page_size != 0.
        header.ring_buffer_capacity = 1;
        header.physical_page_size = static_cast<uint32_t>(to_bytes(DataBlockPageSize::Size4K));
        header.writer_timeout_count.store(0, std::memory_order_relaxed);
        header.writer_lock_timeout_count.store(0, std::memory_order_relaxed);
        header.writer_reader_timeout_count.store(0, std::memory_order_relaxed);
        header.reader_not_ready_count.store(0, std::memory_order_relaxed);
        header.reader_race_detected.store(0, std::memory_order_relaxed);
        header.reader_validation_failed.store(0, std::memory_order_relaxed);
        header.reader_peak_count.store(0, std::memory_order_relaxed);

        slot.write_lock.store(0, std::memory_order_relaxed);
        slot.reader_count.store(0, std::memory_order_relaxed);
        slot.slot_state.store(SlotRWState::SlotState::FREE, std::memory_order_relaxed);
        slot.writer_waiting.store(0, std::memory_order_relaxed);
        slot.write_generation.store(0, std::memory_order_relaxed);
    }
};

} // namespace

TEST(SlotRWCoordinatorTest, WriterAcquireCommitReleaseSingleThread)
{
    SlotFixture f;

    // Acquire write lock with no timeout via C API.
    SlotAcquireResult res = slot_rw_acquire_write(&f.slot, /*timeout_ms=*/0);
    EXPECT_EQ(res, SLOT_ACQUIRE_OK);

    // Commit via C API: this should bump generation and (in header-aware path)
    // would advance commit_index; we just check that calling it does not fail.
    slot_rw_commit(&f.slot);

    // Release write; slot returns to FREE state.
    slot_rw_release_write(&f.slot);

    EXPECT_EQ(f.slot.write_lock.load(std::memory_order_acquire), 0u);
}

TEST(SlotRWCoordinatorTest, ReaderAcquireValidateReleaseSingleThread)
{
    SlotFixture f;

    // Simulate a committed slot: state COMMITTED and generation 1.
    f.slot.slot_state.store(SlotRWState::SlotState::COMMITTED, std::memory_order_release);
    f.slot.write_generation.store(1u, std::memory_order_release);

    uint64_t gen = 0;
    SlotAcquireResult res = slot_rw_acquire_read(&f.slot, &gen);
    EXPECT_EQ(res, SLOT_ACQUIRE_OK);
    EXPECT_EQ(gen, 1u);

    // Validation should succeed when generation did not change.
    EXPECT_TRUE(slot_rw_validate_read(&f.slot, gen));

    // Release reader; reader_count should go back to zero.
    slot_rw_release_read(&f.slot);
    EXPECT_EQ(f.slot.reader_count.load(std::memory_order_acquire), 0u);
}

TEST(SlotRWCoordinatorTest, ReaderDetectsWrapAroundViaGenerationMismatch)
{
    SlotFixture f;

    // Committed state with generation 1 when acquired.
    f.slot.slot_state.store(SlotRWState::SlotState::COMMITTED, std::memory_order_release);
    f.slot.write_generation.store(1u, std::memory_order_release);

    uint64_t gen = 0;
    SlotAcquireResult res = slot_rw_acquire_read(&f.slot, &gen);
    ASSERT_EQ(res, SLOT_ACQUIRE_OK);
    ASSERT_EQ(gen, 1u);

    // Simulate wrap-around / reuse by bumping generation behind the reader's back.
    f.slot.write_generation.store(2u, std::memory_order_release);

    EXPECT_FALSE(slot_rw_validate_read(&f.slot, gen));
    slot_rw_release_read(&f.slot);
}

TEST(SlotRWCoordinatorTest, MetricsResetAndGetRoundTrip)
{
    SlotFixture f;

    // Manually bump a few counters as if timeouts/errors occurred.
    f.header.writer_timeout_count.store(3u, std::memory_order_relaxed);
    f.header.writer_lock_timeout_count.store(1u, std::memory_order_relaxed);
    f.header.writer_reader_timeout_count.store(2u, std::memory_order_relaxed);
    f.header.reader_race_detected.store(5u, std::memory_order_relaxed);

    DataBlockMetrics m{};
    ASSERT_EQ(slot_rw_get_metrics(&f.header, &m), 0);
    EXPECT_EQ(m.writer_timeout_count, 3u);
    EXPECT_EQ(m.writer_lock_timeout_count, 1u);
    EXPECT_EQ(m.writer_reader_timeout_count, 2u);
    EXPECT_EQ(m.reader_race_detected, 5u);

    // Reset and verify that metrics are cleared via the C API.
    ASSERT_EQ(slot_rw_reset_metrics(&f.header), 0);
    ASSERT_EQ(slot_rw_get_metrics(&f.header, &m), 0);
    EXPECT_EQ(m.writer_timeout_count, 0u);
    EXPECT_EQ(m.writer_lock_timeout_count, 0u);
    EXPECT_EQ(m.writer_reader_timeout_count, 0u);
    EXPECT_EQ(m.reader_race_detected, 0u);
}

TEST(SlotRWCoordinatorTest, HighContentionWritersAndReadersStress)
{
    using namespace pylabhub::tests::helper;
    using namespace std::chrono_literals;

    SlotFixture f;

    const int num_writers = get_stress_num_writers();
    const int num_readers = get_stress_num_readers();
    const int duration_sec = get_stress_duration_sec();

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writes_ok{0};
    std::atomic<uint64_t> reads_ok{0};

    auto writer = [&]() {
        while (!stop.load(std::memory_order_acquire))
        {
            SlotAcquireResult res = slot_rw_acquire_write(&f.slot, /*timeout_ms=*/5);
            if (res == SLOT_ACQUIRE_OK)
            {
                slot_rw_commit(&f.slot);
                slot_rw_release_write(&f.slot);
                writes_ok.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    auto reader = [&]() {
        while (!stop.load(std::memory_order_acquire))
        {
            uint64_t gen = 0;
            SlotAcquireResult res = slot_rw_acquire_read(&f.slot, &gen);
            if (res == SLOT_ACQUIRE_OK)
            {
                bool valid = slot_rw_validate_read(&f.slot, gen);
                slot_rw_release_read(&f.slot);
                if (valid)
                {
                    reads_ok.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::vector<std::thread> writers;
    writers.reserve(static_cast<size_t>(num_writers));
    for (int i = 0; i < num_writers; ++i)
        writers.emplace_back(writer);

    std::vector<std::thread> readers;
    readers.reserve(static_cast<size_t>(num_readers));
    for (int i = 0; i < num_readers; ++i)
        readers.emplace_back(reader);

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true, std::memory_order_release);

    for (auto &t : writers)
        t.join();
    for (auto &t : readers)
        t.join();

    EXPECT_GT(writes_ok.load(std::memory_order_relaxed), 0u);
    EXPECT_GT(reads_ok.load(std::memory_order_relaxed), 0u);
}

