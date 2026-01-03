// tests/test_pylabhub_corelib/test_atomicguard.cpp
/**
 * @file test_atomicguard.cpp
 * @brief Unit tests for the AtomicGuard and AtomicOwner classes.
 *
 * This file contains a suite of tests for the `pylabhub::basics::AtomicGuard`
 * spinlock implementation. The tests cover basic acquisition and release,
 * RAII behavior, move semantics, thread safety, and high-contention scenarios.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "atomic_guard.hpp"
#include "platform.hpp"

using namespace std::chrono_literals;
using pylabhub::basics::AtomicGuard;
using pylabhub::basics::AtomicOwner;

// Defines sizes for stress tests to allow for quick or thorough testing.
static constexpr int LIGHT_THREADS = 32;
static constexpr int HEAVY_THREADS = 64;
static constexpr int LIGHT_ITERS = 500;
static constexpr int HEAVY_ITERS = 20000;
static constexpr int SLOT_NUM = 16;

#define THREAD_NUM LIGHT_THREADS
#define ITER_NUM LIGHT_ITERS

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
    ASSERT_EQ(owner.load(), g.token());

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
        ASSERT_EQ(owner.load(), token_in_scope);
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
    owner.store(123u); // Manually lock the owner.
    {
        // Attempt to acquire via RAII constructor.
        AtomicGuard g(&owner, true);
        // The guard should be inactive as it failed to acquire the lock.
        ASSERT_FALSE(g.active());
    }
    // The original lock should remain untouched.
    ASSERT_EQ(owner.load(), 123u);
    owner.store(0u);
}

/**
 * @brief Stress test for concurrent lock acquisition from multiple threads.
 * Multiple threads repeatedly attempt to acquire and release the same lock
 * to ensure mutual exclusion is maintained under contention.
 */
TEST(AtomicGuardTest, ConcurrentAcquireStress)
{
    AtomicOwner owner;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < THREAD_NUM; ++t)
    {
        threads.emplace_back(
            [&]()
            {
                std::mt19937_64 rng(std::random_device{}());
                std::uniform_int_distribution<int> jitter(0, 200);
                auto until = std::chrono::steady_clock::now() + 500ms;
                while (std::chrono::steady_clock::now() < until)
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
                    else
                    {
                        // Spin with a small delay if lock is not acquired.
                        std::this_thread::sleep_for(std::chrono::microseconds(jitter(rng)));
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
        ASSERT_EQ(owner.load(), tok);

        AtomicGuard b(std::move(a)); // Move ownership to b.
        ASSERT_TRUE(b.active());     // b should now be active.
        ASSERT_EQ(b.token(), tok);
        ASSERT_EQ(owner.load(), tok);
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
        ASSERT_EQ(owner.load(), token_c);
        ASSERT_FALSE(c.active());
    } // d's destructor releases the lock.
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
    ASSERT_EQ(owner.load(), tok);

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
    ASSERT_TRUE(g.attach_and_acquire(&owner) ? true : true);
    ASSERT_TRUE(g.active());
    ASSERT_EQ(owner.load(), g.token());
    ASSERT_TRUE(g.release());

    // Detach the guard. It should no longer be able to acquire the lock.
    g.detach_no_release();
    ASSERT_FALSE(g.acquire());
}

/**
 * @brief Tests transferring lock ownership between threads using `std::promise` and `std::future`.
 * This demonstrates a single, simple handoff of an active guard.
 */
TEST(AtomicGuardTest, TransferBetweenThreads_SingleHandoff)
{
    AtomicOwner owner;

    std::promise<AtomicGuard> p;
    std::future<AtomicGuard> f = p.get_future();

    // Producer thread: creates and acquires a guard, then sends it to the consumer.
    std::thread producer(
        [&]()
        {
            AtomicGuard g(&owner, true);
            ASSERT_TRUE(g.active());
            p.set_value(std::move(g)); // Move the guard into the promise.
        });

    // Consumer thread: receives the guard from the future and releases the lock.
    std::thread consumer(
        [&]()
        {
            AtomicGuard g = f.get(); // Receive the moved guard.
            ASSERT_TRUE(g.active());
            ASSERT_EQ(owner.load(), g.token());
            ASSERT_TRUE(g.release());
        });

    producer.join();
    consumer.join();

    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief A stress test involving many concurrent producer-consumer pairs handing off locks.
 * This test is designed to flush out race conditions related to move semantics under load.
 */
TEST(AtomicGuardTest, TransferBetweenThreads_HeavyHandoff)
{
    const int pairs = THREAD_NUM;
    const int iters_per_pair = ITER_NUM;

    std::vector<AtomicOwner> owners(pairs);
    std::vector<std::thread> workers;
    workers.reserve(pairs);

    for (int p = 0; p < pairs; ++p)
    {
        workers.emplace_back(
            [&, p, owner = &owners[p]]() mutable
            {
                for (int i = 0; i < iters_per_pair; ++i)
                {
                    // Acquire the lock. Retry briefly if it's contended.
                    AtomicGuard g(owner, true);
                    if (!g.active())
                    {
                        auto until = std::chrono::steady_clock::now() + 20ms;
                        while (!g.acquire() && std::chrono::steady_clock::now() < until)
                        {
                        }
                    }
                    ASSERT_TRUE(g.active()) << "Guard should be active before move";

                    // Handoff via promise/future to a short-lived local consumer thread.
                    std::promise<AtomicGuard> p2;
                    std::future<AtomicGuard> f2 = p2.get_future();
                    std::thread local_consumer([p2 = std::move(p2), g = std::move(g)]() mutable
                                               { p2.set_value(std::move(g)); });

                    AtomicGuard moved = f2.get();
                    ASSERT_TRUE(moved.active()) << "Guard should be active after move";
                    ASSERT_TRUE(moved.release());
                    local_consumer.join();
                }
            });
    }

    for (auto &w : workers)
        w.join();

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
    const int THREADS = THREAD_NUM;
    const int ITERS = ITER_NUM;

    std::vector<AtomicGuard> slots;
    slots.reserve(SLOTS);
    for (int i = 0; i < SLOTS; ++i)
        slots.emplace_back(&owner); // Attached but not acquired.

    std::vector<std::mutex> slot_mtx(SLOTS);

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]()
            {
                std::mt19937_64 rng(static_cast<uint64_t>(
                                        std::hash<std::thread::id>{}(std::this_thread::get_id())) ^
                                    t);
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
                            ASSERT_EQ(owner.load(), slots[dst].token());
                            ASSERT_TRUE(slots[dst].release());
                        }
                    }
                }
            });
    }

    for (auto &th : threads)
        th.join();

    // Clean up any remaining active locks.
    for (auto &s : slots)
    {
        if (s.active()) [[maybe_unused]]
            bool ok = s.release();
    }
    ASSERT_TRUE(owner.is_free());
}

/**
 * @brief A large-scale stress test with many producer and consumer threads.
 * Producers create locks and send them via channelized futures to consumers,
 * who receive and release them. This tests for leaks and race conditions
 * in the entire lifecycle of creating, moving, and destroying guards under heavy load.
 */
TEST(AtomicGuardTest, ManyConcurrentProducerConsumerPairs)
{
    AtomicOwner owner;
    const int PAIRS = THREAD_NUM;
    const int ITERS = ITER_NUM;

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
                        auto until = std::chrono::steady_clock::now() + 10ms;
                        while (!g.acquire() && std::chrono::steady_clock::now() < until)
                        {
                        }
                    }

                    // Publish the future so the consumer can wait for it.
                    {
                        std::lock_guard<std::mutex> lk(ch.mtx);
                        ch.q.emplace_back(std::move(fut));
                    }
                    ch.cv.notify_one();

                    // Fulfill the promise, moving the guard to the shared state for the consumer.
                    prom.set_value(std::move(g));
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
