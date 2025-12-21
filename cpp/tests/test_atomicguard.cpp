// tests/test_atomicguard.cpp
//
// Unit test for pylabhub::utils::AtomicGuard, converted to GoogleTest.

#include <gtest/gtest.h>

// Ensure that for this test, PANIC() results in a standard abort.
// This is critical for the correctness of the death test.
#ifdef PANIC
#undef PANIC
#endif
#define PANIC(msg) std::abort()

#include "utils/AtomicGuard.hpp"
#include "platform.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace pylabhub::utils;
using namespace std::chrono_literals;

// --- Test Cases ---

TEST(AtomicGuardTest, BasicAcquireRelease)
{
    AtomicOwner owner;
    AtomicGuard guard(&owner);
    ASSERT_NE(guard.token(), 0);
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
        ASSERT_NE(g.token(), 0);
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
    owner.store(123); // Lock is already held by someone else.
    {
        AtomicGuard g(&owner, true); // tryAcquire will fail.
        ASSERT_FALSE(g.active());
    }
    ASSERT_EQ(owner.load(), 123);
    owner.store(0); // Clean up.
}

TEST(AtomicGuardTest, ConcurrentAcquire)
{
    AtomicOwner owner;
    constexpr int THREADS = 64;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
    {
        threads.emplace_back(
            [&, i]()
            {
                std::srand(static_cast<unsigned int>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()) + i));
                std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 200));

                auto until = std::chrono::steady_clock::now() + 1000ms;
                while (std::chrono::steady_clock::now() < until)
                {
                    AtomicGuard g(&owner);
                    if (g.acquire())
                    {
                        success_count++;
                        ASSERT_TRUE(g.active());
                        if (std::rand() % 5 == 0)
                        {
                            std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 100));
                        }
                    }
                }
            });
    }

    for (auto &t : threads)
        t.join();

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
    for (int i = 0; i < NUM_GUARDS; ++i)
    {
        guards.emplace_back(&owner);
    }

    ASSERT_TRUE(guards[0].acquire());

    constexpr int NUM_THREADS = 32;
    constexpr int TRANSFERS_PER_THREAD = 2000;
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back(
            [&]()
            {
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

    for (auto &t : threads)
        t.join();

    int active_count = 0;
    for (const auto &g : guards)
    {
        if (g.active())
        {
            active_count++;
        }
    }
    ASSERT_EQ(active_count, 1);
    ASSERT_NE(owner.load(), 0);

    for (auto &g : guards)
    {
        if (g.active())
        {
            ASSERT_TRUE(g.release());
        }
    }
    ASSERT_TRUE(owner.is_free());
}

TEST(AtomicGuardTest, TransferBetweenThreads)
{
    AtomicOwner owner;
    AtomicGuard guard_A(&owner);
    AtomicGuard guard_B;

    std::atomic<bool> transfer_done{false};
    ASSERT_TRUE(guard_A.acquire());

    std::thread t2(
        [&]()
        {
            guard_B.attach(&owner);
            while (!guard_B.active())
            {
                std::this_thread::sleep_for(1ms);
            }
            ASSERT_EQ(owner.load(), guard_B.token());
            ASSERT_TRUE(guard_B.release());
            transfer_done = true;
        });

    std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(guard_A.transfer_to(guard_B));

    t2.join();
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
    owner.store(0);
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
    uint64_t initial_state = 999;
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

void TriggerAbortLogic()
{
    AtomicOwner owner;
    {
        AtomicGuard g(&owner, true);
        if (!g.active()) {
            exit(5); 
        }
        owner.store(12345);
    }
}

TEST(AtomicGuardDeathTest, DestructorAbortsOnInvariantViolation)
{
    ASSERT_DEATH({ TriggerAbortLogic(); }, "");
}
