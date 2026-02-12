// tests/test_pylabhub_corelib/test_atomicguard.cpp
/**
 * @file test_atomicguard.cpp
 * @brief Unit tests for the AtomicGuard and AtomicOwner classes.
 *
 * This file contains a suite of tests for the `pylabhub::basics::AtomicGuard`
 * spinlock implementation. The tests cover basic acquisition and release,
 * RAII behavior, move semantics, thread safety, and high-contention scenarios.
 */
#include "plh_base.hpp"
#include "shared_test_helpers.h"
#include <deque>
#include <future>
#include <random>

#include "gtest/gtest.h"

using namespace std::chrono_literals;
using pylabhub::basics::AtomicGuard;
using pylabhub::basics::AtomicOwner;
using pylabhub::tests::helper::get_stress_iterations;
using pylabhub::tests::helper::get_stress_num_threads;

namespace
{
// Helper to get a reproducible seed from an environment variable or fall back to random_device.
uint64_t get_seed()
{
    if (const char *seed_str = std::getenv("ATOMICGUARD_TEST_SEED"))
    {
        try
        {
            return std::stoull(seed_str);
        }
        catch (const std::exception &)
        {
            // Fall through to random_device if conversion fails
        }
    }
    return std::random_device{}();
}
} // namespace

static constexpr int SLOT_NUM = 16;

/**
 * @brief Tests the fundamental manual acquire and release behavior.
 * It verifies that a guard can successfully acquire a lock on a free owner,
 * making the owner non-free, and can subsequently release it, making the owner free again.
 */
TEST(AtomicGuardTest, BasicAcquireRelease)
{
    AtomicOwner owner;
    AtomicGuard g(&owner);
    ASSERT_NE(g.token(), 0u);
    ASSERT_FALSE(g.active());

    // Acquire the lock and check state
    ASSERT_TRUE(g.acquire());
    ASSERT_TRUE(g.active());
    ASSERT_FALSE(owner.is_free());

    // Release the lock and check state
    ASSERT_TRUE(g.release());
    ASSERT_FALSE(g.active());
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief Tests the RAII (Resource Acquisition Is Initialization) functionality.
 * The guard should acquire the lock upon construction when requested and
 * automatically release it upon destruction (when it goes out of scope).
 */
TEST(AtomicGuardTest, RaiiAndTokenPersistence)
{
    AtomicOwner owner;
    uint64_t token_in_scope = 0;
    {
        // Construct guard to acquire lock immediately.
        AtomicGuard g(&owner, true);
        ASSERT_NE(g.token(), 0u);
        token_in_scope = g.token();
        ASSERT_TRUE(g.active());
        ASSERT_FALSE(owner.is_free());
    } // Lock is automatically released here by the destructor.
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief Ensures that an explicit `release()` call works correctly even with RAII.
 * If a guard's lock is released manually before destruction, the destructor
 * should not cause a double-release or error.
 */
TEST(AtomicGuardTest, ExplicitReleaseAndDestruction)
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner);
        ASSERT_TRUE(g.acquire());
        ASSERT_TRUE(g.active());
        // Manually release before destructor is called.
        ASSERT_TRUE(g.release());
        ASSERT_FALSE(g.active());
    } // Destructor runs on an inactive guard, which should be a no-op.
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief Tests that RAII construction fails to acquire a lock if it's already taken.
 */
TEST(AtomicGuardTest, RaiiAcquireFailure)
{
    AtomicOwner owner;
    // Lock the owner with a separate guard to simulate it being taken.
    AtomicGuard g_locker(&owner, true);
    ASSERT_TRUE(g_locker.active());
    {
        // Attempt to acquire via RAII constructor.
        AtomicGuard g(&owner, true);
        // The guard should be inactive as it failed to acquire the lock.
        ASSERT_FALSE(g.active());
    }
    // The original lock should remain untouched.
    ASSERT_FALSE(owner.is_free());
    ASSERT_TRUE(g_locker.release());
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief Stress test for concurrent lock acquisition from multiple threads.
 * Multiple threads repeatedly attempt to acquire and release the same lock
 * to ensure mutual exclusion is maintained under contention.
 */
TEST(AtomicGuardTest, ConcurrentAcquireStress)
{
    const int thread_num = get_stress_num_threads();
    const int iter_num = get_stress_iterations(20000, 500);

    AtomicOwner owner;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    uint64_t base_seed = get_seed();

    for (int t = 0; t < thread_num; ++t)
    {
        threads.emplace_back(
            [&owner, &success_count, t, base_seed, iter_num]()
            {
                std::mt19937_64 rng(base_seed + t); // Add 't' to vary seed slightly per thread
                for (int i = 0; i < iter_num; ++i)
                {
                    AtomicGuard g(&owner);
                    if (g.acquire())
                    {
                        success_count.fetch_add(1, std::memory_order_relaxed);
                        // Simulate variable work inside the critical section.
                        if ((rng() & 0xF) == 0)
                            std::this_thread::sleep_for(std::chrono::microseconds(rng() & 0xFF));
                        [[maybe_unused]] bool ok = g.release();
                    }
                }
            });
    }

    for (auto &th : threads)
        th.join();

    // At least some acquisitions should have succeeded.
    ASSERT_GT(success_count.load(std::memory_order_relaxed), 0);
    // The lock must be free at the end.
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief Verifies single-threaded move constructor and move assignment semantics.
 * Ensures that ownership of an active lock can be correctly transferred from one
 * guard to another, and that the source guard becomes inactive.
 */
TEST(AtomicGuardTest, MoveSemanticsSingleThread)
{
    AtomicOwner owner;
    uint64_t tok;

    // Test move construction.
    {
        AtomicGuard a(&owner, true);
        ASSERT_TRUE(a.active());
        tok = a.token();
        ASSERT_FALSE(owner.is_free());

        AtomicGuard b(std::move(a)); // Move ownership to b.
        ASSERT_TRUE(b.active());     // b should now be active.
        ASSERT_EQ(b.token(), tok);
        ASSERT_FALSE(owner.is_free());
        ASSERT_FALSE(a.active()); // a should be inactive.
    } // b's destructor releases the lock.
    ASSERT_TRUE(owner.is_free());

    // Test move assignment.
    {
        AtomicGuard c(&owner, true);
        ASSERT_TRUE(c.active());
        uint64_t token_c = c.token();

        AtomicGuard d;
        d = std::move(c); // Move assign ownership to d.
        ASSERT_TRUE(d.active());
        ASSERT_EQ(d.token(), token_c);
        ASSERT_FALSE(owner.is_free());
        ASSERT_FALSE(c.active());
    } // d's destructor releases the lock.
    ASSERT_TRUE(owner.is_free());

    // Test self-move-assignment.
    {
        AtomicGuard e(&owner, true);
        ASSERT_TRUE(e.active());
        uint64_t token_e = e.token();
        e = std::move(e);        // Self-move.
        ASSERT_TRUE(e.active()); // Should remain active.
        ASSERT_EQ(e.token(), token_e);
        ASSERT_FALSE(owner.is_free());
    } // e's destructor releases the lock.
    ASSERT_TRUE(owner.is_free());

    // Test self-move-assignment for a detached guard.
    {
        AtomicGuard f; // Detached guard.
        ASSERT_FALSE(f.active());
        uint64_t token_f = f.token();
        f = std::move(f); // Self-move.
        ASSERT_FALSE(f.active());
        ASSERT_EQ(f.token(), token_f); // Token should remain unchanged.
        // There's no owner to check as it was never attached.
    } // f's destructor runs.
    // Ensure the owner is still free (no interaction).
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief Specifically tests that moving an active guard correctly transfers ownership.
 */
TEST(AtomicGuardTest, MoveActiveGuardBehavior)
{
    AtomicOwner owner;
    AtomicGuard a(&owner, true);
    ASSERT_TRUE(a.active());
    uint64_t tok = a.token();

    AtomicGuard b(std::move(a)); // Move while active.
    ASSERT_TRUE(b.active());
    ASSERT_EQ(b.token(), tok);
    ASSERT_FALSE(owner.is_free());

    // The source guard `a` should now be inactive and detached.
    // Calling methods on it should be safe and reflect its inactive state.
    ASSERT_FALSE(a.active());
    ASSERT_FALSE(a.release()); // Cannot release an inactive guard.

    ASSERT_TRUE(b.release());
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief Tests the ability to attach a guard to an owner after construction and detach it.
 */
TEST(AtomicGuardTest, AttachDetach)
{
    AtomicOwner owner;
    AtomicGuard g; // Create a detached guard.
    ASSERT_FALSE(g.active());
    ASSERT_FALSE(g.acquire()); // Cannot acquire while detached.

    // Attach the guard to an owner and acquire the lock.
    ASSERT_TRUE(g.attach_and_acquire(&owner));
    ASSERT_TRUE(g.active());
    ASSERT_FALSE(owner.is_free());
    ASSERT_TRUE(g.release());

    // Detach the guard. It should no longer be able to acquire the lock.
    g.detach();
    ASSERT_FALSE(g.active());
    ASSERT_FALSE(g.acquire());
}

/**
 * @brief Tests transferring lock ownership between threads using `std::promise` and `std::future`.
 * This demonstrates a single, simple handoff of an active guard.
 */
TEST(AtomicGuardTest, TransferBetweenThreads_SingleHandoff)
{
    AtomicOwner owner;
    std::atomic<bool> thread_failure{false};

    // Note: Some older STL implementations had issues with move-only types (like AtomicGuard)
    // in std::promise/std::future. If encountering issues on such platforms, consider
    // using std::promise<std::unique_ptr<AtomicGuard>> as a workaround.
    std::promise<AtomicGuard> p;
    std::future<AtomicGuard> f = p.get_future();

    // Producer thread: creates and acquires a guard, then sends it to the consumer.
    std::thread producer(
        [&]()
        {
            try
            {
                AtomicGuard g(&owner, true);
                if (!g.active())
                {
                    // In this test, acquisition should not fail.
                    throw std::runtime_error("Producer failed to acquire lock.");
                }
                p.set_value(std::move(g)); // Move the guard into the promise.
            }
            catch (...)
            {
                thread_failure.store(true, std::memory_order_relaxed);
                try
                {
                    p.set_exception(std::current_exception());
                }
                catch (const std::future_error &)
                {
                    // Ignore if future is already satisfied or no state.
                }
            }
        });

    // Consumer thread: receives the guard from the future and releases the lock.
    std::thread consumer(
        [&]()
        {
            try
            {
                AtomicGuard g = f.get(); // Receive the moved guard.
                if (!g.active())
                {
                    // If we receive an inactive guard, the producer must have failed.
                    thread_failure.store(true, std::memory_order_relaxed);
                    return;
                }

                if (!g.release())
                {
                    thread_failure.store(true, std::memory_order_relaxed);
                }
            }
            catch (...)
            {
                thread_failure.store(true, std::memory_order_relaxed);
            }
        });

    producer.join();
    consumer.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief A stress test involving many concurrent producer-consumer pairs handing off locks.
 * This test is designed to flush out race conditions related to move semantics under load.
 */
TEST(AtomicGuardTest, TransferBetweenThreads_HeavyHandoff)
{
    const int pairs = get_stress_num_threads();
    const int iters_per_pair = get_stress_iterations(20000, 500);

    std::vector<AtomicOwner> owners(pairs);
    std::vector<std::thread> workers;
    workers.reserve(pairs);
    std::atomic<bool> thread_failure{false};

    uint64_t base_seed = get_seed(); // Get base seed once

    for (int p = 0; p < pairs; ++p)
    {
        workers.emplace_back(
            [&, p, owner = &owners[p]]() mutable
            {
                std::mt19937_64 rng(base_seed + p); // Seed per worker
                for (int i = 0; i < iters_per_pair; ++i)
                {
                    // Acquire the lock. Retry a few times if it's contended.
                    AtomicGuard g(owner, true);
                    if (!g.active())
                    {
                        int retries = 5; // A small, fixed number of retries
                        while (!g.acquire() && retries > 0)
                        {
                            std::this_thread::yield(); // Yield to other threads
                            retries--;
                        }
                    }
                    if (!g.active())
                    {
                        thread_failure.store(true, std::memory_order_relaxed);
                        continue; // Skip this iteration if acquire failed.
                    }

                    // Handoff via promise/future to a short-lived local consumer thread.
                    std::promise<AtomicGuard> p2;
                    std::future<AtomicGuard> f2 = p2.get_future();
                    std::thread local_consumer(
                        [p2 = std::move(p2), g = std::move(g)]() mutable
                        {
                            try
                            {
                                p2.set_value(std::move(g));
                            }
                            catch (...)
                            {
                                try
                                {
                                    p2.set_exception(std::current_exception());
                                }
                                catch (...)
                                {
                                }
                            }
                        });

                    AtomicGuard moved = f2.get();
                    if (!moved.active())
                    {
                        thread_failure.store(true, std::memory_order_relaxed);
                    }
                    if (!moved.release())
                    {
                        thread_failure.store(true, std::memory_order_relaxed);
                    }
                    local_consumer.join();
                }
            });
    }

    for (auto &w : workers)
        w.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    for (const auto &owner : owners)
    {
        ASSERT_TRUE(owner.is_free());
    }
}

/**
 * @brief Stress tests concurrent move assignments into a shared vector of guards.
 * Many threads randomly pick two guard slots and move one to the other, while also
 * attempting to acquire/release locks, to stress the move-assignment operator.
 */
TEST(AtomicGuardTest, ConcurrentMoveAssignmentStress)
{
    AtomicOwner owner;
    const int SLOTS = SLOT_NUM;
    const int THREADS = get_stress_num_threads();
    const int ITERS = get_stress_iterations(20000, 500);

    std::vector<AtomicGuard> slots;
    slots.reserve(SLOTS);
    for (int i = 0; i < SLOTS; ++i)
        slots.emplace_back(&owner); // Attached but not acquired.

    std::vector<std::mutex> slot_mtx(SLOTS);
    std::atomic<bool> thread_failure{false};

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    uint64_t base_seed = get_seed(); // Get base seed once

    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]()
            {
                // The original code used a hash of thread ID, which is good for uniqueness but not
                // reproducibility. Replace with a reproducible seed.
                std::mt19937_64 rng(base_seed + t);
                std::uniform_int_distribution<int> idxdist(0, SLOTS - 1);
                for (int it = 0; it < ITERS; ++it)
                {
                    int src = idxdist(rng);
                    int dst = idxdist(rng);
                    if (src == dst)
                        continue;

                    // Lock both mutexes to safely access the slots vector.
                    std::unique_lock<std::mutex> lk_src(slot_mtx[src], std::defer_lock);
                    std::unique_lock<std::mutex> lk_dst(slot_mtx[dst], std::defer_lock);
                    std::lock(lk_src, lk_dst);

                    // Move source into a temporary, then move assign into destination.
                    AtomicGuard tmp = std::move(slots[src]);
                    slots[dst] = std::move(tmp);

                    // To add more pressure, opportunistically try to use the guard.
                    if (!slots[dst].active())
                    {
                        if (slots[dst].acquire())
                        {
                            // If acquire succeeds, the guard MUST be active.
                            // A check against owner state is redundant.
                            if (!slots[dst].release())
                            {
                                thread_failure.store(true, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            });
    }

    for (auto &th : threads)
        th.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));

    // Clean up any remaining active locks.
    for (auto &s : slots)
    {
        if (s.active())
        {
            if (!s.release())
            {
                thread_failure.store(true, std::memory_order_relaxed);
            }
        }
    }
    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    ASSERT_TRUE(owner.is_free());
}

#if !defined(NDEBUG) && GTEST_HAS_DEATH_TEST
/**
 * @brief Death tests to ensure invariant violations cause a panic in debug builds.
 */
TEST(AtomicGuardDeathTest, InvariantViolationsPanic)
{
    // Test that calling attach() on an active guard panics.
    // The new destructor never panics, so that death test has been removed.
    EXPECT_DEATH(
        []()
        {
            AtomicOwner owner1;
            AtomicOwner owner2;
            AtomicGuard g(&owner1, true);
            g.attach(&owner2); // Should panic.
        }(),
        "The original lock is now leaked");
}
#endif

/**
 * @brief A large-scale stress test with many producer and consumer threads.
 * Producers create locks and send them via channelized futures to consumers,
 * who receive and release them. This tests for leaks and race conditions
 * in the entire lifecycle of creating, moving, and destroying guards under heavy load.
 */
TEST(AtomicGuardTest, ManyConcurrentProducerConsumerPairs)
{
    AtomicOwner owner;
    const int PAIRS = get_stress_num_threads();
    const int ITERS = get_stress_iterations(20000, 500);

    // A simple channel for passing futures between a producer and a consumer.
    struct Channel
    {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::future<AtomicGuard>> q;
    };

    std::vector<std::unique_ptr<Channel>> channels;
    for (int p = 0; p < PAIRS; ++p)
        channels.emplace_back(std::make_unique<Channel>());

    std::atomic<bool> thread_failure{false};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(PAIRS);
    consumers.reserve(PAIRS);

    for (int p = 0; p < PAIRS; ++p)
    {
        Channel &ch = *channels[p];

        // Consumer thread: waits for futures, gets the guard, and releases the lock.
        consumers.emplace_back(
            [&ch, &thread_failure, ITERS]()
            {
                for (int i = 0; i < ITERS; ++i)
                {
                    std::future<AtomicGuard> fut;
                    {
                        std::unique_lock<std::mutex> lk(ch.mtx);
                        ch.cv.wait(lk, [&] { return !ch.q.empty(); });
                        fut = std::move(ch.q.front());
                        ch.q.pop_front();
                    }

                    try
                    {
                        AtomicGuard g = fut.get();
                        if (g.active())
                        {
                            bool ok = g.release();
                            if (!ok)
                                thread_failure.store(true, std::memory_order_relaxed);
                        }
                    }
                    catch (...)
                    {
                        thread_failure.store(true, std::memory_order_relaxed);
                    }
                }
            });

        // Producer thread: creates guards, acquires locks, and sends futures to the consumer.
        producers.emplace_back(
            [&ch, &owner, ITERS]()
            {
                for (int i = 0; i < ITERS; ++i)
                {
                    std::promise<AtomicGuard> prom;
                    std::future<AtomicGuard> fut = prom.get_future();

                    // Create and acquire a guard, with a small retry loop for contention.
                    AtomicGuard g(&owner, true);
                    if (!g.active())
                    {
                        int retries = 5; // A small, fixed number of retries
                        while (!g.acquire() && retries > 0)
                        {
                            std::this_thread::yield(); // Yield to other threads
                            retries--;
                        }
                    }

                    // Publish the future so the consumer can wait for it.
                    {
                        std::lock_guard<std::mutex> lk(ch.mtx);
                        ch.q.emplace_back(std::move(fut));
                    }
                    ch.cv.notify_one();

                    // Fulfill the promise, moving the guard to the shared state for the consumer.
                    try
                    {
                        prom.set_value(std::move(g));
                    }
                    catch (...)
                    {
                        try
                        {
                            prom.set_exception(std::current_exception());
                        }
                        catch (...)
                        {
                            // Ignore if promise is already satisfied
                        }
                    }
                }
            });
    }

    for (auto &t : producers)
        t.join();
    for (auto &t : consumers)
        t.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    ASSERT_TRUE(owner.is_free());
}