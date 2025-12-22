// tests/test_atomicguard.cpp
//
// Unit tests for pylabhub::utils::AtomicGuard, converted to GoogleTest with fixes.
//
// - PANIC is forced to std::abort() for the death test.
// - Worker/thread bodies do not call GTest assertions directly; they update
//   atomics which are checked on the main thread after join().

#include <gtest/gtest.h>

#include <cstdlib>    // std::abort, std::exit
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <functional>
#include <memory>
#include <future>
#include <sstream>
#include <random>

#include "platform.hpp"

// Ensure PANIC calls std::abort() for tests that rely on abort behavior.
#ifdef PANIC
#undef PANIC
#endif
#define PANIC(msg) std::abort()

#include "utils/AtomicGuard.hpp"

using namespace pylabhub::utils;
using namespace std::chrono_literals;

//
// ---------------------- AtomicGuard tests ----------------------
//

TEST(AtomicGuardTest, BasicAcquireRelease)
{
    AtomicOwner owner;
    AtomicGuard guard(&owner);
    ASSERT_NE(guard.token(), 0u);
    ASSERT_FALSE(guard.active());

    ASSERT_TRUE(guard.acquire());
    ASSERT_TRUE(guard.active());
    ASSERT_EQ(owner.load(), guard.token());

    ASSERT_TRUE(guard.release());
    ASSERT_FALSE(guard.active());
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, RaiiAndTokenPersistence)
{
    AtomicOwner owner;
    uint64_t token_in_scope = 0;
    {
        AtomicGuard g(&owner, true); // acquire on construction
        ASSERT_NE(g.token(), 0u);
        token_in_scope = g.token();
        ASSERT_TRUE(g.active());
        ASSERT_EQ(owner.load(), token_in_scope);
    } // destructor releases
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, ExplicitReleaseAndDestruction)
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner);
        ASSERT_TRUE(g.acquire());
        ASSERT_TRUE(g.active());
        ASSERT_TRUE(g.release());
        ASSERT_FALSE(g.active());
    }
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, RaiiAcquireFailure)
{
    AtomicOwner owner;
    owner.store(123u); // Locked by "someone else"
    {
        AtomicGuard g(&owner, true); // tryAcquire will fail.
        ASSERT_FALSE(g.active());
    }
    ASSERT_EQ(owner.load(), 123u);
    owner.store(0u); // cleanup
}

TEST(AtomicGuardTest, ConcurrentAcquire)
{
    AtomicOwner owner;
    constexpr int THREADS = 64;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back([&, i]() {
            // Seed thread-local randomness
            std::mt19937_64 rng((uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id()) + (uint64_t)i);
            std::uniform_int_distribution<int> jitter(0, 200);

            // Small random start jitter
            std::this_thread::sleep_for(std::chrono::microseconds(jitter(rng)));

            auto until = std::chrono::steady_clock::now() + 1000ms;
            while (std::chrono::steady_clock::now() < until)
            {
                AtomicGuard g(&owner);
                if (g.acquire())
                {
                    success_count.fetch_add(1, std::memory_order_relaxed);

                    // Simulate occasional work
                    if ((rng() % 5) == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
                    }
                }
                // destructor releases
            }
        });
    }

    for (auto &t : threads) t.join();

    ASSERT_GT(success_count.load(), 0);
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, TransferSingleThread)
{
    AtomicOwner owner;
    AtomicGuard a(&owner);
    AtomicGuard b(&owner);

    ASSERT_TRUE(a.acquire());
    ASSERT_TRUE(a.active());

    ASSERT_TRUE(a.transfer_to(b));
    ASSERT_FALSE(a.active());
    ASSERT_TRUE(b.active());
    ASSERT_EQ(owner.load(), b.token());

    ASSERT_TRUE(b.release());
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, ConcurrentTransfers)
{
    AtomicOwner owner;
    constexpr int NUM_GUARDS = 16;
    std::vector<AtomicGuard> guards;
    guards.reserve(NUM_GUARDS);
    for (int i = 0; i < NUM_GUARDS; ++i) guards.emplace_back(&owner);

    ASSERT_TRUE(guards[0].acquire());

    constexpr int NUM_THREADS = 32;
    constexpr int TRANSFERS_PER_THREAD = 2000;
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back([&]() {
            unsigned int seed = static_cast<unsigned int>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
            for (int j = 0; j < TRANSFERS_PER_THREAD; ++j)
            {
                seed = seed * 1103515245 + 12345;
                int src_idx = (seed / 65536) % NUM_GUARDS;
                seed = seed * 1103515245 + 12345;
                int dest_idx = (seed / 65536) % NUM_GUARDS;
                if (src_idx == dest_idx) continue;
                (void)guards[src_idx].transfer_to(guards[dest_idx]);
            }
        });
    }

    for (auto &t : threads) t.join();

    int active_count = 0;
    for (const auto &g : guards) if (g.active()) ++active_count;
    ASSERT_EQ(active_count, 1);
    ASSERT_NE(owner.load(), 0u);

    // Release the final owner
    for (auto &g : guards) if (g.active()) ASSERT_TRUE(g.release());
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, TransferBetweenThreads)
{
    AtomicOwner owner;
    AtomicGuard guard_A(&owner);
    AtomicGuard guard_B;
    std::atomic<bool> transfer_done{false};
    std::atomic<bool> thread_failure{false};

    ASSERT_TRUE(guard_A.acquire());

    std::thread t2([&]() {
        // Attach this thread's guard to the owner
        guard_B.attach(&owner);

        // Wait until guard_B becomes active (i.e., receives the transfer)
        auto until = std::chrono::steady_clock::now() + 2000ms;
        while (!guard_B.active() && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(1ms);
        }

        if (!guard_B.active()) {
            thread_failure.store(true, std::memory_order_relaxed);
            return;
        }

        // Verify it owns the token and release.
        if (owner.load() != guard_B.token() || !guard_B.release()) {
            thread_failure.store(true, std::memory_order_relaxed);
            return;
        }
        transfer_done.store(true, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(guard_A.transfer_to(guard_B));

    t2.join();

    ASSERT_FALSE(thread_failure.load());
    ASSERT_TRUE(transfer_done.load());
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, TransferRejectsDifferentOwners)
{
    AtomicOwner o1, o2;
    AtomicGuard a(&o1);
    AtomicGuard b(&o2);

    ASSERT_TRUE(a.acquire());
    ASSERT_FALSE(a.transfer_to(b));
    ASSERT_TRUE(a.active());
    ASSERT_TRUE(a.release());
}

TEST(AtomicGuardTest, DestructorWithTransfer)
{
    AtomicOwner owner;
    {
        AtomicGuard a(&owner, true);
        ASSERT_TRUE(a.active());
        AtomicGuard b(&owner);
        ASSERT_TRUE(a.transfer_to(b));
        // On scope exit: a destroyed first (no-op), b destroyed and releases
    }
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, AttachAndDetach)
{
    AtomicOwner owner;
    AtomicGuard guard;

    ASSERT_FALSE(guard.active());
    ASSERT_FALSE(guard.acquire());

    ASSERT_TRUE(guard.attach_and_acquire(&owner));
    ASSERT_TRUE(guard.active());
    ASSERT_TRUE(guard.release());

    guard.detach_no_release();
    ASSERT_FALSE(guard.acquire());
}

TEST(AtomicGuardTest, DetachWhileActiveAndDestruction)
{
    AtomicOwner owner;
    uint64_t leaked_token = 0;
    {
        AtomicGuard g(&owner, true);
        ASSERT_TRUE(g.active());
        leaked_token = g.token();
        g.detach_no_release();
    }
    ASSERT_EQ(owner.load(), leaked_token);
    owner.store(0u);
}

TEST(AtomicGuardTest, NoopDestructorScenarios)
{
    {
        AtomicGuard g;
        ASSERT_FALSE(g.active());
    }
    AtomicOwner owner;
    {
        AtomicGuard g(&owner);
        ASSERT_FALSE(g.active());
    }
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, AtomicOwnerMoveSemantics)
{
    uint64_t initial_state = 999u;
    {
        AtomicOwner o1(initial_state);
        ASSERT_EQ(o1.load(), initial_state);
        AtomicOwner o2(std::move(o1));
        ASSERT_EQ(o2.load(), initial_state);
    }
    {
        AtomicOwner o3(initial_state);
        ASSERT_EQ(o3.load(), initial_state);
        AtomicOwner o4;
        o4 = std::move(o3);
        ASSERT_EQ(o4.load(), initial_state);
    }
}

TEST(AtomicGuardTest, AtomicGuardMoveSemantics)
{
    AtomicOwner owner;
    uint64_t token_a = 0;

    // Move construction
    {
        AtomicGuard a(&owner, true);
        ASSERT_TRUE(a.active());
        token_a = a.token();
        ASSERT_EQ(owner.load(), token_a);

        AtomicGuard b(std::move(a));
        ASSERT_TRUE(b.active());
        ASSERT_EQ(b.token(), token_a);
        ASSERT_EQ(owner.load(), token_a);
    }
    ASSERT_TRUE(owner.is_free());

    // Move assignment
    {
        AtomicGuard c(&owner, true);
        ASSERT_TRUE(c.active());
        uint64_t token_c = c.token();

        AtomicGuard d;
        d = std::move(c);
        ASSERT_TRUE(d.active());
        ASSERT_EQ(d.token(), token_c);
        ASSERT_EQ(owner.load(), token_c);
    }
    ASSERT_TRUE(owner.is_free());
}

// Trigger logic that is expected to cause abort in destructor path.
static void TriggerAbortLogic()
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner, true);
        if (!g.active()) {
            std::exit(5); // distinct code for failure to acquire in single-threaded test
        }
        // Simulate corruption/stealing of the token
        owner.store(12345u);
    }
    // g's destructor should detect mismatch and call PANIC() -> abort()
}

// Death test: ensure destructor aborts on invariant violation.
// Using a simple ".*" regex to be robust across platforms.
TEST(AtomicGuardDeathTest, DestructorAbortsOnInvariantViolation)
{
    ASSERT_DEATH({ TriggerAbortLogic(); }, ".*");
}
