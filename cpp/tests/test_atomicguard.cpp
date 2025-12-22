// tests/test_atomicguard.cpp
//
// Unit tests for pylabhub::utils::AtomicGuard.

#include <gtest/gtest.h>

#include <cstdlib>
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

// For death tests, ensure PANIC calls std::abort() to be detectable by the test framework.
#ifdef PANIC
#undef PANIC
#endif
#define PANIC(msg) std::abort()

#include "utils/AtomicGuard.hpp"

using namespace pylabhub::utils;
using namespace std::chrono_literals;

// What: Tests the fundamental explicit acquire and release operations.
// How: It creates a guard, acquires ownership of an owner, verifies the owner's
//      state reflects the guard's token, and then explicitly releases ownership,
//      verifying the owner becomes free again.
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

// What: Verifies the RAII (Resource Acquisition Is Initialization) behavior of the guard.
// How: A guard is constructed with the `acquire_now` flag set to true. The test
//      verifies that the guard is active and owns the lock immediately upon
//      construction, and that the lock is automatically released when the guard
//      goes out of scope.
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
    } // destructor releases here
    ASSERT_TRUE(owner.is_free());
}

// What: Ensures a guard can be explicitly released before its destruction without error.
// How: An active guard is explicitly released. The test verifies it becomes inactive.
//      When the guard is then destructed at the end of the scope, it should be a
//      no-op, which is confirmed by checking that the owner remains free.
TEST(AtomicGuardTest, ExplicitReleaseAndDestruction)
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner);
        ASSERT_TRUE(g.acquire());
        ASSERT_TRUE(g.active());
        ASSERT_TRUE(g.release());
        ASSERT_FALSE(g.active());
    } // Destructor is a no-op for an inactive guard.
    ASSERT_TRUE(owner.is_free());
}

// What: Tests the case where RAII acquisition fails because the lock is already held.
// How: The owner is manually pre-locked with an arbitrary token. A guard is then
//      constructed with `acquire_now` set to true. The test asserts that this
//      guard correctly reports itself as inactive.
TEST(AtomicGuardTest, RaiiAcquireFailure)
{
    AtomicOwner owner;
    owner.store(123u); // Locked by "someone else"
    {
        AtomicGuard g(&owner, true); // tryAcquire will fail.
        ASSERT_FALSE(g.active());
    }
    ASSERT_EQ(owner.load(), 123u); // Verify the original lock was not disturbed.
    owner.store(0u); // cleanup
}

// What: A stress test to verify that only one thread can acquire the lock at a time.
// How: Many threads are spawned, and each repeatedly attempts to acquire the lock
//      in a loop. A shared atomic counter tracks the number of successful acquisitions
//      over a period of time. After all threads complete, the test asserts that
//      the total success count is greater than zero (i.e., work was done) and that
//      the lock is properly free at the end.
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
            std::mt19937_64 rng((uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id()) + (uint64_t)i);
            std::uniform_int_distribution<int> jitter(0, 200);
            std::this_thread::sleep_for(std::chrono::microseconds(jitter(rng)));

            auto until = std::chrono::steady_clock::now() + 1000ms;
            while (std::chrono::steady_clock::now() < until)
            {
                AtomicGuard g(&owner);
                if (g.acquire())
                {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                    if ((rng() % 5) == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
                    }
                }
            }
        });
    }

    for (auto &t : threads) t.join();

    ASSERT_GT(success_count.load(), 0);
    ASSERT_TRUE(owner.is_free());
}

// What: Tests the single-threaded transfer of ownership between two guards.
// How: Guard 'a' acquires the lock. It then transfers ownership to guard 'b'. The
//      test verifies that 'a' becomes inactive, 'b' becomes active, and the owner's
//      token matches 'b's token. Finally, it verifies 'b' can release the lock.
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

// What: A stress test for the `transfer_to` mechanism under high concurrency.
// How: A pool of guards is created. One guard initially acquires the lock. Many
//      threads are then spawned, each repeatedly attempting to transfer ownership
//      between two randomly chosen guards from the pool. This chaos tests the
//      atomicity of the transfer logic. At the end, the test verifies that
//      exactly one guard still holds the lock.
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
            unsigned int seed = static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
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

    for (auto &g : guards) {
        if (g.active()) {
            ASSERT_TRUE(g.release());
        }
    }
    ASSERT_TRUE(owner.is_free());
}

// What: Tests transferring ownership of a lock from a guard in one thread to a
//       guard in another thread.
// How: Thread A acquires a lock with `guard_A`. Thread B is spawned and attaches
//      `guard_B` to the same owner. Thread A then calls `transfer_to(guard_B)`.
//      Thread B verifies it becomes active and is able to release the lock.
//      Futures/promises are used for synchronization.
TEST(AtomicGuardTest, TransferBetweenThreads)
{
    AtomicOwner owner;
    AtomicGuard guard_A(&owner);
    AtomicGuard guard_B;
    std::atomic<bool> thread_failure{false};

    ASSERT_TRUE(guard_A.acquire());

    std::thread t2([&]() {
        guard_B.attach(&owner);
        auto until = std::chrono::steady_clock::now() + 2000ms;
        while (!guard_B.active() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(1ms);
        }

        if (!guard_B.active() || owner.load() != guard_B.token() || !guard_B.release()) {
            thread_failure.store(true);
        }
    });

    std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(guard_A.transfer_to(guard_B));

    t2.join();

    ASSERT_FALSE(thread_failure.load());
    ASSERT_TRUE(owner.is_free());
}

// What: Ensures that transferring ownership between guards attached to different
//       owners is safely rejected.
// How: Two separate owners are created. A guard is attached to each. An attempt
//      to transfer ownership from a guard on owner 1 to a guard on owner 2 is
//      made. The test asserts that this transfer fails and the original guard
//      remains active.
TEST(AtomicGuardTest, TransferRejectsDifferentOwners)
{
    AtomicOwner o1, o2;
    AtomicGuard a(&o1);
    AtomicGuard b(&o2);

    ASSERT_TRUE(a.acquire());
    ASSERT_FALSE(a.transfer_to(b)); // The critical check
    ASSERT_TRUE(a.active());
    ASSERT_TRUE(a.release());
}

// What: Verifies correct RAII behavior when a transfer has occurred.
// How: Guard 'a' acquires a lock and transfers it to 'b'. Scope is then exited.
//      The test verifies that 'a's destructor is a no-op and 'b's destructor
//      correctly releases the lock, leaving the owner free.
TEST(AtomicGuardTest, DestructorWithTransfer)
{
    AtomicOwner owner;
    {
        AtomicGuard a(&owner, true);
        ASSERT_TRUE(a.active());
        AtomicGuard b(&owner);
        ASSERT_TRUE(a.transfer_to(b));
        // On scope exit: 'a' is destroyed first (no-op), 'b' is destroyed and releases.
    }
    ASSERT_TRUE(owner.is_free());
}

// What: Tests the dynamic attach/detach functionality of a guard.
// How: A guard is created without an owner and verified to be non-functional.
//      It is then attached to an owner and verified to work correctly. Finally,
//      it is detached and confirmed to be non-functional again.
TEST(AtomicGuardTest, AttachAndDetach)
{
    AtomicOwner owner;
    AtomicGuard guard;

    ASSERT_FALSE(guard.active());
    ASSERT_FALSE(guard.acquire()); // Cannot acquire without an owner.

    ASSERT_TRUE(guard.attach_and_acquire(&owner));
    ASSERT_TRUE(guard.active());
    ASSERT_TRUE(guard.release());

    guard.detach_no_release();
    ASSERT_FALSE(guard.acquire()); // Cannot acquire after detaching.
}

// What: Verifies that detaching a guard while it is active correctly "leaks"
//       the lock, leaving the owner in a locked state.
// How: A guard acquires a lock. `detach_no_release()` is called. When the guard
//      is destructed, the test verifies that the owner remains locked because
//      the guard abdicated its responsibility for releasing the lock.
TEST(AtomicGuardTest, DetachWhileActiveAndDestruction)
{
    AtomicOwner owner;
    uint64_t leaked_token = 0;
    {
        AtomicGuard g(&owner, true);
        ASSERT_TRUE(g.active());
        leaked_token = g.token();
        g.detach_no_release(); // Detach while active.
    } // Destructor should be a no-op.
    ASSERT_EQ(owner.load(), leaked_token); // Owner remains locked.
    owner.store(0u); // Manual cleanup for test.
}

// What: Ensures that creating and destructing guards in various inactive states
//       is safe and has no side effects.
// How: A default-constructed guard and a guard attached to an owner (but not
//      acquired) are created and destroyed. The test verifies the owner remains
//      free, confirming the destructors were no-ops.
TEST(AtomicGuardTest, NoopDestructorScenarios)
{
    {
        AtomicGuard g; // Default constructed
        ASSERT_FALSE(g.active());
    }
    AtomicOwner owner;
    {
        AtomicGuard g(&owner); // Attached but not active
        ASSERT_FALSE(g.active());
    }
    ASSERT_TRUE(owner.is_free());
}

// What: Verifies that AtomicOwner supports move semantics correctly.
// How: It tests both move construction and move assignment, ensuring that the
//      state (the token value) is correctly transferred from the source owner
//      to the destination owner.
TEST(AtomicGuardTest, AtomicOwnerMoveSemantics)
{
    uint64_t initial_state = 999u;
    // Move construction
    {
        AtomicOwner o1(initial_state);
        ASSERT_EQ(o1.load(), initial_state);
        AtomicOwner o2(std::move(o1));
        ASSERT_EQ(o2.load(), initial_state);
    }
    // Move assignment
    {
        AtomicOwner o3(initial_state);
        ASSERT_EQ(o3.load(), initial_state);
        AtomicOwner o4;
        o4 = std::move(o3);
        ASSERT_EQ(o4.load(), initial_state);
    }
}

// What: Verifies that AtomicGuard supports move semantics correctly.
// How: It tests both move construction and move assignment for an *active* guard.
//      It verifies that the moved-to guard becomes active, holds the correct
//      token, and properly releases the lock upon its destruction.
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
    } // `b`'s destructor releases the lock.
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
    } // `d`'s destructor releases the lock.
    ASSERT_TRUE(owner.is_free());
}

// This helper function creates a scenario where the AtomicGuard destructor's
// safety invariant is violated, which should trigger PANIC -> std::abort().
static void TriggerAbortLogic()
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner, true);
        if (!g.active()) {
            std::exit(5); // Should not happen in a single-threaded test.
        }
        // Simulate another thread or bug "stealing" the lock from under the guard.
        owner.store(12345u);
    }
    // `g`'s destructor will now see that owner.load() != g.token(), and must abort.
}

// What: A death test to verify the critical safety feature that the guard's
//       destructor will abort the program if it holds a lock token that doesn't
//       match what's in the owner.
// How: It runs a helper function `TriggerAbortLogic` in a forked process. This
//      function simulates a "stolen" lock. The test asserts that this forked
//      process terminates with an abort signal, as expected.
TEST(AtomicGuardDeathTest, DestructorAbortsOnInvariantViolation)
{
    // `ASSERT_DEATH` runs the provided statement in a separate process and
    // checks that it terminates with a non-zero exit status. The regex ".*"
    // is a simple way to match any output message, which is sufficient here.
    ASSERT_DEATH({ TriggerAbortLogic(); }, ".*");
}
