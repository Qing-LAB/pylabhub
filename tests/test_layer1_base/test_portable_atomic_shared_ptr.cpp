/**
 * @file test_portable_atomic_shared_ptr.cpp
 * @brief Direct unit tests for PortableAtomicSharedPtr.
 */
#include "shared_test_helpers.h"
#include "portable_atomic_shared_ptr.hpp"

#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using pylabhub::utils::detail::PortableAtomicSharedPtr;

namespace
{
struct Payload
{
    int seq;
    int doubled;
};
} // namespace

TEST(PortableAtomicSharedPtrTest, DefaultNullAndStoreLoadRoundTrip)
{
    PortableAtomicSharedPtr<int> ptr;

    EXPECT_EQ(ptr.load(), nullptr);

    auto value = std::make_shared<int>(42);
    ptr.store(value);

    auto loaded = ptr.load();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(*loaded, 42);

    ptr.store(nullptr);
    EXPECT_EQ(ptr.load(), nullptr);
}

TEST(PortableAtomicSharedPtrTest, LoadedSnapshotKeepsPreviousObjectAlive)
{
    PortableAtomicSharedPtr<int> ptr;

    ptr.store(std::make_shared<int>(7));
    auto snapshot = ptr.load();

    ptr.store(std::make_shared<int>(11));
    ptr.store(nullptr);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(*snapshot, 7);
    EXPECT_EQ(ptr.load(), nullptr);
}

TEST(PortableAtomicSharedPtrTest, ConcurrentLoadStoreObservesConsistentPayloads)
{
    PortableAtomicSharedPtr<Payload> ptr;

    constexpr int iterations = 20000;
    const int     reader_count = std::max(2, pylabhub::tests::helper::get_stress_num_readers());

    // Barrier: writer waits until all readers are ready before starting.
    // This prevents the writer from completing all iterations before any
    // reader thread is scheduled (which caused non_null_reads == 0 under load).
    std::atomic<int>  readers_ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> writer_done{false};
    std::atomic<bool> invariant_ok{true};
    std::atomic<int>  non_null_reads{0};

    std::vector<std::thread> readers;
    readers.reserve(static_cast<size_t>(reader_count));
    for (int t = 0; t < reader_count; ++t)
    {
        readers.emplace_back(
            [&]()
            {
                // Signal ready and wait for go.
                readers_ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();

                while (!writer_done.load(std::memory_order_acquire))
                {
                    auto snapshot = ptr.load(std::memory_order_acquire);
                    if (!snapshot)
                        continue;

                    ++non_null_reads;
                    if (snapshot->doubled != snapshot->seq * 2 || snapshot->seq <= 0)
                    {
                        invariant_ok.store(false, std::memory_order_relaxed);
                    }
                }

                // Final read after writer_done — verify last value is consistent.
                auto snapshot = ptr.load(std::memory_order_acquire);
                if (!snapshot || snapshot->doubled != snapshot->seq * 2 || snapshot->seq < 1)
                {
                    invariant_ok.store(false, std::memory_order_relaxed);
                }
            });
    }

    // Wait until all readers are in their spin-wait, then start.
    while (readers_ready.load(std::memory_order_acquire) < reader_count)
        std::this_thread::yield();

    std::thread writer(
        [&]()
        {
            // Wait for the go signal so readers are spinning before we start.
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (int i = 1; i <= iterations; ++i)
            {
                ptr.store(std::make_shared<Payload>(Payload{i, i * 2}),
                          std::memory_order_release);
            }
            writer_done.store(true, std::memory_order_release);
        });

    // Release the barrier — all readers + writer run concurrently.
    go.store(true, std::memory_order_release);

    writer.join();
    for (auto &reader : readers)
        reader.join();

    EXPECT_GT(non_null_reads.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(invariant_ok.load(std::memory_order_relaxed));
}
