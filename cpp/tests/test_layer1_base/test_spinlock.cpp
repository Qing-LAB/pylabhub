/**
 * @file test_spinlock.cpp
 * @brief Unit tests for in-process spin state + guard (token mode).
 *
 * State owner: InProcessSpinState. Locking is done by SpinGuard.
 * Same 32-byte layout as SharedSpinLock; token semantics only.
 */
#include "shared_test_helpers.h"
#include "utils/in_process_spin_state.hpp"

#include <deque>
#include <future>
#include <mutex>
#include <random>
#include <thread>
#include <gtest/gtest.h>

using namespace std::chrono_literals;
using pylabhub::hub::InProcessSpinState;
using pylabhub::hub::InProcessSpinStateGuard;
using pylabhub::hub::make_in_process_spin_state;
using pylabhub::hub::SpinGuard;
using pylabhub::tests::helper::get_stress_iterations;
using pylabhub::tests::helper::get_stress_num_threads;

namespace
{
uint64_t get_seed()
{
    if (const char *seed_str = std::getenv("SPINLOCK_TEST_SEED"))
    {
        try
        {
            return std::stoull(seed_str);
        }
        catch (const std::exception &) {}
    }
    return std::random_device{}();
}
} // namespace

static constexpr int SLOT_NUM = 16;

// -----------------------------------------------------------------------------
// Basic acquire / release
// -----------------------------------------------------------------------------
TEST(InProcessSpinStateTest, BasicAcquireRelease)
{
    auto state = make_in_process_spin_state();
    SpinGuard g(state);
    ASSERT_TRUE(g.holds_lock());
    ASSERT_NE(g.token(), 0u);
    ASSERT_TRUE(state.is_locked());

    ASSERT_TRUE(g.release());
    ASSERT_FALSE(g.holds_lock());
    ASSERT_FALSE(state.is_locked());
}

TEST(InProcessSpinStateTest, SpinGuardAlias_BehavesIdentically)
{
    auto state = make_in_process_spin_state();
    SpinGuard g(state);
    ASSERT_TRUE(g.holds_lock());
    ASSERT_NE(g.token(), 0u);
    ASSERT_TRUE(state.is_locked());
}

TEST(InProcessSpinStateTest, RaiiAndTokenPersistence)
{
    auto state = make_in_process_spin_state();
    uint64_t token_in_scope = 0;
    {
        InProcessSpinStateGuard g(state);
        token_in_scope = g.token();
        ASSERT_TRUE(g.holds_lock());
        ASSERT_TRUE(state.is_locked());
    }
    ASSERT_FALSE(state.is_locked());
}

TEST(InProcessSpinStateTest, ExplicitReleaseAndDestruction)
{
    auto state = make_in_process_spin_state();
    {
        InProcessSpinStateGuard g(state);
        ASSERT_TRUE(g.holds_lock());
        ASSERT_TRUE(g.release());
        ASSERT_FALSE(g.holds_lock());
    }
    ASSERT_FALSE(state.is_locked());
}

TEST(InProcessSpinStateTest, RaiiAcquireFailure)
{
    auto state = make_in_process_spin_state();
    InProcessSpinStateGuard g_locker(state);
    ASSERT_TRUE(g_locker.holds_lock());
    {
        InProcessSpinStateGuard g;
        ASSERT_FALSE(g.try_lock(state, 1)); // short timeout
    }
    ASSERT_TRUE(state.is_locked());
    g_locker.release();
    ASSERT_FALSE(state.is_locked());
}

// -----------------------------------------------------------------------------
// Concurrent stress
// -----------------------------------------------------------------------------
TEST(InProcessSpinStateTest, ConcurrentAcquireStress)
{
    const int thread_num = get_stress_num_threads();
    const int iter_num = get_stress_iterations(20000, 500);

    auto state = make_in_process_spin_state();
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    uint64_t base_seed = get_seed();

    for (int t = 0; t < thread_num; ++t)
    {
        threads.emplace_back(
            [&state, &success_count, t, base_seed, iter_num]()
            {
                std::mt19937_64 rng(base_seed + t);
                for (int i = 0; i < iter_num; ++i)
                {
                    InProcessSpinStateGuard g;
                    if (g.try_lock(state, 5))
                    {
                        success_count.fetch_add(1, std::memory_order_relaxed);
                        if ((rng() & 0xF) == 0)
                            std::this_thread::sleep_for(std::chrono::microseconds(rng() & 0xFF));
                        g.release();
                    }
                }
            });
    }

    for (auto &th : threads)
        th.join();

    ASSERT_GT(success_count.load(std::memory_order_relaxed), 0);
    ASSERT_FALSE(state.is_locked());
}

// -----------------------------------------------------------------------------
// Move semantics
// -----------------------------------------------------------------------------
TEST(InProcessSpinStateTest, MoveSemanticsSingleThread)
{
    auto state = make_in_process_spin_state();

    {
        InProcessSpinStateGuard a(state);
        ASSERT_TRUE(a.holds_lock());
        uint64_t tok = a.token();
        ASSERT_TRUE(state.is_locked());

        InProcessSpinStateGuard b(std::move(a));
        ASSERT_TRUE(b.holds_lock());
        ASSERT_EQ(b.token(), tok);
        ASSERT_TRUE(state.is_locked());
        ASSERT_FALSE(a.holds_lock());
    }
    ASSERT_FALSE(state.is_locked());

    {
        InProcessSpinStateGuard c(state);
        ASSERT_TRUE(c.holds_lock());
        uint64_t token_c = c.token();

        InProcessSpinStateGuard d;
        d = std::move(c);
        ASSERT_TRUE(d.holds_lock());
        ASSERT_EQ(d.token(), token_c);
        ASSERT_FALSE(c.holds_lock());
    }
    ASSERT_FALSE(state.is_locked());
}

TEST(InProcessSpinStateTest, MoveActiveGuardBehavior)
{
    auto state = make_in_process_spin_state();
    InProcessSpinStateGuard a(state);
    ASSERT_TRUE(a.holds_lock());
    uint64_t tok = a.token();

    InProcessSpinStateGuard b(std::move(a));
    ASSERT_TRUE(b.holds_lock());
    ASSERT_EQ(b.token(), tok);
    ASSERT_TRUE(state.is_locked());
    ASSERT_FALSE(a.holds_lock());

    ASSERT_TRUE(b.release());
    ASSERT_FALSE(state.is_locked());
}

TEST(InProcessSpinStateTest, SelfMoveAssignmentAndDetachedMove)
{
    auto state = make_in_process_spin_state();

    // Self-move-assignment (active guard): implementation-defined; we require no crash and lock still held
    {
        InProcessSpinStateGuard e(state);
        ASSERT_TRUE(e.holds_lock());
        uint64_t token_e = e.token();
        e = std::move(e); // self-move
        ASSERT_TRUE(e.holds_lock());
        ASSERT_EQ(e.token(), token_e);
        ASSERT_TRUE(state.is_locked());
    }
    ASSERT_FALSE(state.is_locked());

    // Self-move-assignment (detached guard): no-op, token unchanged
    {
        InProcessSpinStateGuard f;
        ASSERT_FALSE(f.holds_lock());
        uint64_t token_f = f.token();
        f = std::move(f); // self-move
        ASSERT_FALSE(f.holds_lock());
        ASSERT_EQ(f.token(), token_f);
    }
    ASSERT_FALSE(state.is_locked());
}

TEST(InProcessSpinStateTest, DetachThenTryLockReuse)
{
    auto state = make_in_process_spin_state();
    InProcessSpinStateGuard g;
    ASSERT_FALSE(g.holds_lock());

    // Attach and acquire via try_lock (like AtomicGuard attach_and_acquire)
    ASSERT_TRUE(g.try_lock(state, 10));
    ASSERT_TRUE(g.holds_lock());
    ASSERT_TRUE(state.is_locked());
    ASSERT_TRUE(g.release());

    // Detach; guard no longer holds, state is free
    g.detach();
    ASSERT_FALSE(g.holds_lock());
    ASSERT_FALSE(state.is_locked());

    // Reuse: try_lock again (same or another state) works
    ASSERT_TRUE(g.try_lock(state, 10));
    ASSERT_TRUE(g.holds_lock());
    g.release();
    ASSERT_FALSE(state.is_locked());
}

TEST(InProcessSpinStateTest, ConcurrentMoveAssignmentStress)
{
    auto state = make_in_process_spin_state();
    const int SLOTS = SLOT_NUM;
    const int THREADS = get_stress_num_threads();
    const int ITERS = get_stress_iterations(20000, 500);

    std::vector<InProcessSpinStateGuard> slots;
    slots.reserve(SLOTS);
    for (int i = 0; i < SLOTS; ++i)
        slots.emplace_back(); // default-constructed, not holding

    std::vector<std::mutex> slot_mtx(SLOTS);
    std::atomic<bool> thread_failure{false};
    uint64_t base_seed = get_seed();

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back(
            [&, t]()
            {
                std::mt19937_64 rng(base_seed + t);
                std::uniform_int_distribution<int> idxdist(0, SLOTS - 1);
                for (int it = 0; it < ITERS; ++it)
                {
                    int src = idxdist(rng);
                    int dst = idxdist(rng);
                    if (src == dst)
                        continue;

                    std::unique_lock<std::mutex> lk_src(slot_mtx[src], std::defer_lock);
                    std::unique_lock<std::mutex> lk_dst(slot_mtx[dst], std::defer_lock);
                    std::lock(lk_src, lk_dst);

                    InProcessSpinStateGuard tmp = std::move(slots[src]);
                    slots[dst] = std::move(tmp);

                    if (!slots[dst].holds_lock() && slots[dst].try_lock(state, 1))
                    {
                        if (!slots[dst].release())
                            thread_failure.store(true, std::memory_order_relaxed);
                    }
                }
            });
    }

    for (auto &th : threads)
        th.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));

    for (auto &s : slots)
    {
        if (s.holds_lock())
            s.release();
    }
    ASSERT_FALSE(state.is_locked());
}

// -----------------------------------------------------------------------------
// Handoff between threads
// -----------------------------------------------------------------------------
TEST(InProcessSpinStateTest, TransferBetweenThreads_SingleHandoff)
{
    auto state = make_in_process_spin_state();
    std::atomic<bool> thread_failure{false};

    std::promise<InProcessSpinStateGuard> p;
    std::future<InProcessSpinStateGuard> f = p.get_future();

    std::thread producer(
        [&]()
        {
            try
            {
                InProcessSpinStateGuard g(state);
                if (!g.holds_lock())
                    throw std::runtime_error("Producer failed to acquire.");
                p.set_value(std::move(g));
            }
            catch (...)
            {
                thread_failure.store(true, std::memory_order_relaxed);
                try { p.set_exception(std::current_exception()); }
                catch (const std::future_error &) {}
            }
        });

    std::thread consumer(
        [&]()
        {
            try
            {
                InProcessSpinStateGuard g = f.get();
                if (!g.holds_lock())
                    thread_failure.store(true, std::memory_order_relaxed);
                else
                    g.release();
            }
            catch (...)
            {
                thread_failure.store(true, std::memory_order_relaxed);
            }
        });

    producer.join();
    consumer.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    ASSERT_FALSE(state.is_locked());
}

// -----------------------------------------------------------------------------
// Heavy handoff stress
// -----------------------------------------------------------------------------
TEST(InProcessSpinStateTest, TransferBetweenThreads_HeavyHandoff)
{
    const int pairs = get_stress_num_threads();
    const int iters_per_pair = get_stress_iterations(20000, 500);

    auto state = make_in_process_spin_state();
    std::vector<std::thread> workers;
    workers.reserve(pairs);
    std::atomic<bool> thread_failure{false};
    uint64_t base_seed = get_seed();

    for (int p = 0; p < pairs; ++p)
    {
        workers.emplace_back(
            [&, p]()
            {
                std::mt19937_64 rng(base_seed + p);
                for (int i = 0; i < iters_per_pair; ++i)
                {
                    InProcessSpinStateGuard g(state);
                    if (!g.holds_lock())
                    {
                        int retries = 5;
                        while (!g.try_lock(state, 10) && retries > 0)
                        {
                            std::this_thread::yield();
                            --retries;
                        }
                    }
                    if (!g.holds_lock())
                    {
                        thread_failure.store(true, std::memory_order_relaxed);
                        continue;
                    }

                    std::promise<InProcessSpinStateGuard> p2;
                    std::future<InProcessSpinStateGuard> f2 = p2.get_future();
                    std::thread local_consumer(
                        [p2 = std::move(p2), g = std::move(g)]() mutable
                        {
                            try { p2.set_value(std::move(g)); }
                            catch (...) {}
                        });

                    InProcessSpinStateGuard moved = f2.get();
                    if (!moved.holds_lock())
                        thread_failure.store(true, std::memory_order_relaxed);
                    else
                        moved.release();
                    local_consumer.join();
                }
            });
    }

    for (auto &w : workers)
        w.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    ASSERT_FALSE(state.is_locked());
}

// -----------------------------------------------------------------------------
// Many producer-consumer pairs (channel of guards)
// -----------------------------------------------------------------------------
TEST(InProcessSpinStateTest, ManyConcurrentProducerConsumerPairs)
{
    const int pairs = get_stress_num_threads();
    const int iters = get_stress_iterations(20000, 500);

    auto state = make_in_process_spin_state();
    std::deque<std::future<InProcessSpinStateGuard>> q;
    std::mutex q_mutex;
    std::atomic<bool> done{false};
    std::atomic<bool> thread_failure{false};

    std::thread consumer(
        [&]()
        {
            while (true)
            {
                std::future<InProcessSpinStateGuard> fut;
                {
                    std::lock_guard<std::mutex> lk(q_mutex);
                    if (q.empty() && done.load(std::memory_order_acquire))
                        break;
                    if (q.empty())
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        continue;
                    }
                    fut = std::move(q.front());
                    q.pop_front();
                }
                if (!fut.valid())
                    continue;
                try
                {
                    InProcessSpinStateGuard g = fut.get();
                    if (g.holds_lock())
                        g.release();
                }
                catch (...)
                {
                    thread_failure.store(true, std::memory_order_relaxed);
                }
            }
        });

    std::vector<std::thread> producers;
    for (int t = 0; t < pairs; ++t)
    {
        producers.emplace_back(
            [&, t]()
            {
                for (int i = 0; i < iters; ++i)
                {
                    std::promise<InProcessSpinStateGuard> prom;
                    std::future<InProcessSpinStateGuard> fut = prom.get_future();
                    InProcessSpinStateGuard g(state);
                    if (!g.holds_lock())
                        continue;
                    {
                        std::lock_guard<std::mutex> lk(q_mutex);
                        q.push_back(std::move(fut));
                    }
                    prom.set_value(std::move(g));
                }
            });
    }

    for (auto &p : producers)
        p.join();
    done.store(true, std::memory_order_release);
    consumer.join();

    ASSERT_FALSE(thread_failure.load(std::memory_order_relaxed));
    ASSERT_FALSE(state.is_locked());
}
