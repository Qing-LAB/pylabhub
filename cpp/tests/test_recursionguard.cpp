// tests/test_recursionguard.cpp
//
// Unit test for pylabhub::utils::RecursionGuard, converted to GoogleTest.

#include <gtest/gtest.h>

#include "utils/RecursionGuard.hpp"

#include <functional>
#include <memory>
#include <vector>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>

using namespace pylabhub::utils;

namespace {

// Test Functions and Objects used by the tests below.
int some_object = 0;
int another_object = 0;

void RecursiveFunction(int depth, bool expect_recursing)
{
    // Before creating a guard, we expect the recursion state to match the context
    // from the calling function.
    ASSERT_EQ(RecursionGuard::is_recursing(&some_object), expect_recursing);

    RecursionGuard g(&some_object);
    // Immediately after creating a guard, recursion should always be detected for this object.
    ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));

    if (depth > 0)
    {
        // For any recursive calls, we now expect them to detect recursion.
        RecursiveFunction(depth - 1, /*expect_recursing=*/true);
    }
}

} // namespace

// What: Tests the fundamental behavior of the RecursionGuard within a single thread.
// How: A function calls itself recursively. It verifies that the outermost call
//      initially sees no recursion, while all inner calls correctly detect that
//      they are recursing. It also verifies the state is clean after the top-level
//      call completes.
TEST(RecursionGuardTest, SingleObjectRecursion)
{
    // Sanity check: ensure the object is not marked as recursing at the start.
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    // Start the recursion, expecting the first call to not be a recursion itself.
    RecursiveFunction(3, false);
    // After the entire call stack has unwound, ensure the state is clean.
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
}

// What: Verifies that RecursionGuards for different objects are independent.
// How: It creates nested guards for two distinct objects and checks that the
//      recursion state for each object is tracked separately and correctly as the
//      guards are constructed and destructed.
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

        ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));
        ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));
    }

    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));
}

// What: Tests that the recursion count is handled correctly even if guards
//       are destructed out of their creation order.
// How: Two guards are created on the heap using unique_ptrs. The "outer" guard
//      (g1) is explicitly destroyed before the "inner" guard (g2). The test
//      verifies that the recursion state for each object is correctly updated
//      at each step, ensuring the system doesn't rely on strict LIFO stack ordering.
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

    g2.reset();
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));
}

// What: Verifies that the RecursionGuard is thread-safe and that its state
//       is correctly maintained on a per-thread basis.
// How: This test has two parts:
//      1. Parallel Interference Test: Multiple threads are spawned, each performing
//         recursion on its own private, stack-allocated object. It verifies that
//         the guards in one thread do not affect the state seen by any other thread.
//      2. Thread-Local State Test: Two threads operate on the *same* shared object
//         address. Thread A acquires a guard and signals Thread B. Thread B then
//         verifies that `is_recursing()` returns `false` for it, proving that the
//         guard's state is truly thread-local.
TEST(RecursionGuardTest, ThreadSafety)
{
    // --- Part 1: Parallel recursion on distinct objects should not interfere. ---
    constexpr int NUM_THREADS = 8;
    std::atomic<bool> thread_failed{false};
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&]() {
            int local_obj = 0; // Each thread uses its own stack-allocated object.
            std::function<void(int, bool)> recur = [&](int depth, bool expect_recursing) {
                if (RecursionGuard::is_recursing(&local_obj) != expect_recursing) {
                    thread_failed.store(true);
                    return;
                }
                RecursionGuard g(&local_obj);
                if (!RecursionGuard::is_recursing(&local_obj)) {
                    thread_failed.store(true);
                    return;
                }
                if (depth > 0) {
                    recur(depth - 1, true);
                }
            };
            recur(3, false);
        });
    }

    for (auto &th : threads) th.join();
    ASSERT_FALSE(thread_failed.load()) << "Part 1: One or more threads failed the per-thread recursion check.";

    // --- Part 2: Guard on a shared object in one thread is not visible to another. ---
    int shared_obj = 0;
    std::promise<void> ready_promise;
    std::future<void> ready_future = ready_promise.get_future();
    std::promise<void> done_promise;
    std::future<void> done_future = done_promise.get_future();
    std::atomic<bool> other_thread_observed_recursing{false};

    // Thread A: Acquires the guard and signals it's ready.
    std::thread threadA([&]() {
        RecursionGuard guard(&shared_obj);
        ready_promise.set_value();
        done_future.wait();
    });

    // Thread B: Waits for Thread A, then checks the recursion state.
    std::thread threadB([&]() {
        ready_future.wait();
        // This is the critical check: Thread B should NOT see the recursion
        // guard held by Thread A on the same object.
        if (RecursionGuard::is_recursing(&shared_obj)) {
            other_thread_observed_recursing.store(true);
        }
        done_promise.set_value();
    });

    threadA.join();
    threadB.join();

    ASSERT_FALSE(other_thread_observed_recursing.load()) << "Part 2: Other thread incorrectly observed recursion on a shared object.";
}

