// tests/test_pylabhub_corelib/test_recursionguard.cpp
/**
 * @file test_recursionguard.cpp
 * @brief Unit tests for the RecursionGuard class.
 *
 * This file contains a suite of tests for the `pylabhub::basics::RecursionGuard`,
 * a utility designed to detect and prevent unwanted recursion on a per-thread,
 * per-object basis. The tests cover single-threaded recursion, independence between
 * objects, non-LIFO destruction order, and thread safety.
 */
#include <array>
#include <future>
#include <memory>

#include "plh_base.hpp"
#include "gtest/gtest.h"

using pylabhub::basics::RecursionGuard;

namespace
{

// Test Functions and Objects used by the tests below.
int some_object = 0;
int another_object = 0;

/**
 * @brief A recursive function used to test the RecursionGuard.
 * @param depth The current recursion depth.
 * @param expect_recursing True if `is_recursing` should return true before the guard is created.
 */
void RecursiveFunction(int depth, bool expect_recursing)
{
    // Before creating the guard, the recursion state should match the expectation from the caller.
    ASSERT_EQ(RecursionGuard::is_recursing(&some_object), expect_recursing);

    RecursionGuard g(&some_object);
    // Immediately after creating a guard, recursion should always be detected for this object.
    ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));

    if (depth > 0)
    {
        // Any subsequent recursive calls must detect recursion.
        RecursiveFunction(depth - 1, /*expect_recursing=*/true);
    }
}

} // namespace

/**
 * @brief Tests the fundamental behavior of the RecursionGuard within a single thread.
 *
 * A function calls itself recursively. It verifies that the outermost call
 * initially sees no recursion, while all inner calls correctly detect that
 * they are recursing. It also verifies the state is clean after the top-level
 * call completes.
 */
TEST(RecursionGuardTest, SingleObjectRecursion)
{
    // Sanity check: ensure the object is not marked as recursing at the start.
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    // Start the recursion. The first call is not a recursion itself.
    RecursiveFunction(3, false);
    // After the entire call stack has unwound, ensure the state is clean.
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
}

/**
 * @brief Verifies that RecursionGuards for different objects are independent.
 *
 * It creates nested guards for two distinct objects and checks that the
 * recursion state for each object is tracked separately and correctly as the
 * guards are constructed and destructed.
 */
TEST(RecursionGuardTest, MultipleObjects)
{
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));

    {
        RecursionGuard g1(&some_object);
        ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));
        ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));

        {
            RecursionGuard g2(&another_object);
            ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));
            ASSERT_TRUE(RecursionGuard::is_recursing(&another_object));
        }

        // g2 is out of scope, 'another_object' should no longer be marked as recursing.
        ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));
        ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));
    }

    // g1 is out of scope, 'some_object' should no longer be marked.
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));
}

/**
 * @brief Tests that the recursion count is handled correctly even if guards
 * are destructed out of their creation order (non-LIFO).
 *
 * Two guards are created on the heap using unique_ptrs. The "outer" guard
 * (g1) is explicitly destroyed before the "inner" guard (g2). The test
 * verifies that the recursion state for each object is correctly updated
 * at each step.
 */
TEST(RecursionGuardTest, OutOfOrderDestruction)
{
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));

    auto g1 = std::make_unique<RecursionGuard>(&some_object);
    ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));

    auto g2 = std::make_unique<RecursionGuard>(&another_object);
    ASSERT_TRUE(RecursionGuard::is_recursing(&another_object));

    // Destroy g1 (the "outer" guard) before g2.
    g1.reset();
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    ASSERT_TRUE(RecursionGuard::is_recursing(&another_object));

    // Destroy g2.
    g2.reset();
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));
}

/**
 * @brief Tests the move constructor of RecursionGuard.
 *
 * Verifies that move construction correctly transfers ownership of the key,
 * making the source guard inert and ensuring the recursion stack is correctly
 * maintained.
 */
TEST(RecursionGuardTest, MoveConstructor)
{
    // A key for the guard.
    int obj_key = 1;

    // Test move constructor
    ASSERT_FALSE(RecursionGuard::is_recursing(&obj_key));
    {
        RecursionGuard g1(&obj_key);
        ASSERT_TRUE(RecursionGuard::is_recursing(&obj_key));

        RecursionGuard g2 = std::move(g1); // Move construction
        // g1 is now inert, g2 owns the key. The moved-from guard's destructor
        // will be a no-op.
        ASSERT_TRUE(RecursionGuard::is_recursing(&obj_key));
    }
    // g2 is out of scope, so it should have popped the key.
    ASSERT_FALSE(RecursionGuard::is_recursing(&obj_key));
}

/**
 * @brief Verifies that the RecursionGuard is thread-safe and its state
 * is correctly maintained on a per-thread basis (using thread_local storage).
 *
 * This test has two parts:
 * 1. Parallel Interference Test: Multiple threads perform recursion on their own
 *    private objects. It verifies that guards in one thread do not affect others.
 * 2. Thread-Local State Test: Two threads operate on the *same* shared object.
 *    Thread A acquires a guard and signals Thread B. Thread B then verifies that
 *    `is_recursing()` returns `false` for it, proving state is thread-local.
 */
TEST(RecursionGuardTest, ThreadSafety)
{
    // Part 1: Parallel recursion on distinct objects should not interfere.
    constexpr int NUM_THREADS = 8;
    std::atomic<bool> thread_failed{false};
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back(
            [&]()
            {
                int local_obj = 0; // Each thread uses its own stack-allocated object.
                std::function<void(int, bool)> recur = [&](int depth, bool expect_recursing)
                {
                    if (RecursionGuard::is_recursing(&local_obj) != expect_recursing)
                    {
                        thread_failed.store(true);
                        return;
                    }
                    RecursionGuard g(&local_obj);
                    if (!RecursionGuard::is_recursing(&local_obj))
                    {
                        thread_failed.store(true);
                        return;
                    }
                    if (depth > 0)
                    {
                        recur(depth - 1, true);
                    }
                };
                recur(3, false);
            });
    }

    for (auto &th : threads)
        th.join();
    ASSERT_FALSE(thread_failed.load())
        << "Part 1: One or more threads failed the per-thread recursion check.";

    // Part 2: Guard on a shared object in one thread is not visible to another.
    int shared_obj = 0;
    std::promise<void> ready_promise;
    std::future<void> ready_future = ready_promise.get_future();
    std::promise<void> done_promise;
    std::future<void> done_future = done_promise.get_future();
    std::atomic<bool> other_thread_observed_recursing{false};

    // Thread A: Acquires the guard and signals it's ready.
    std::thread threadA(
        [&]()
        {
            RecursionGuard guard(&shared_obj);
            ASSERT_TRUE(RecursionGuard::is_recursing(&shared_obj));
            ready_promise.set_value();
            done_future.wait(); // Wait until thread B is done checking.
        });

    // Thread B: Waits for Thread A, then checks the recursion state.
    std::thread threadB(
        [&]()
        {
            ready_future.wait();
            // CRITICAL CHECK: Thread B should NOT see the recursion guard held by Thread A
            // on the same object, because the state is thread-local.
            if (RecursionGuard::is_recursing(&shared_obj))
            {
                other_thread_observed_recursing.store(true);
            }
            done_promise.set_value();
        });

    threadA.join();
    threadB.join();

    ASSERT_FALSE(other_thread_observed_recursing.load())
        << "Part 2: Other thread incorrectly observed recursion on a shared object.";
}

/**
 * @brief Verifies that exceeding kMaxRecursionDepth panics (abort) instead of throwing.
 */
TEST(RecursionGuardTest, MaxDepthPanics)
{
    int key = 0;
    EXPECT_DEATH(
        {
            std::array<std::unique_ptr<RecursionGuard>, pylabhub::basics::kMaxRecursionDepth + 1>
                guards;
            for (size_t i = 0; i <= pylabhub::basics::kMaxRecursionDepth; ++i)
            {
                guards[i] = std::make_unique<RecursionGuard>(&key);
            }
        },
        "max recursion depth");
}
