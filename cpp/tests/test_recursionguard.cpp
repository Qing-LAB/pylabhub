// tests/test_recursionguard.cpp
//
// Unit test for pylabhub::utils::RecursionGuard, converted to GoogleTest.
//
// Notes:
//  - Uses a unique test-suite prefix (RecursionGuardUnit) to avoid duplicate
//    symbol/linker collisions with other test translation units.

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

// Test Functions and Objects (initialized)
int some_object = 0;
int another_object = 0;

void RecursiveFunction(int depth, bool expect_recursing)
{
    // At the start of the *outermost* call expect_recursing == false.
    // For inner recursive calls expect_recursing == true because an outer guard exists.
    ASSERT_EQ(RecursionGuard::is_recursing(&some_object), expect_recursing);

    RecursionGuard g(&some_object);
    // after constructing the guard, it should always be true
    ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));

    if (depth > 0)
    {
        // inner calls will see the outer guard -> expect_recursing == true
        RecursiveFunction(depth - 1, /*expect_recursing=*/true);
    }
}

} // namespace

// Use a unique suite name 'RecursionGuardUnit' to avoid duplicate symbol issues.
TEST(RecursionGuardUnit, SingleObjectRecursion)
{
    // sanity: ensure starting state is not recursing
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    RecursiveFunction(3, false);
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
}

TEST(RecursionGuardUnit, MultipleObjects)
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

TEST(RecursionGuardUnit, OutOfOrderDestruction)
{
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));

    auto g1 = std::make_unique<RecursionGuard>(&some_object);
    ASSERT_TRUE(RecursionGuard::is_recursing(&some_object));

    auto g2 = std::make_unique<RecursionGuard>(&another_object);
    ASSERT_TRUE(RecursionGuard::is_recursing(&another_object));

    // Destroy g1 (the "outer" guard) before g2 (the "inner" guard).
    g1.reset();
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    ASSERT_TRUE(RecursionGuard::is_recursing(&another_object));

    g2.reset();
    ASSERT_FALSE(RecursionGuard::is_recursing(&another_object));
}

// Thread-safety test for RecursionGuard.
//
// 1) Each thread runs a small recursive routine on its own local object address,
//    verifying the guard works per-thread (no GTest assertions inside threads).
// 2) Two-thread check: thread A acquires a guard on a shared object and signals
//    readiness; thread B then checks that it does NOT see is_recursing(...) true
//    for that same object (verifying thread-local behavior).
TEST(RecursionGuardUnit, ThreadSafety)
{
    // Part 1: parallel recursion on distinct objects should not interfere.
    constexpr int NUM_THREADS = 8;
    std::atomic<bool> thread_failed{false};
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&]() {
            // Each thread uses its own stack-allocated object.
            int local_obj = 0;

            // inner lambda: recursive check using runtime boolean failures
            std::function<void(int, bool)> recur = [&](int depth, bool expect_recursing) {
                // Precondition: before creating guard, expect_recursing indicates what we expect.
                if (RecursionGuard::is_recursing(&local_obj) != expect_recursing)
                {
                    thread_failed.store(true, std::memory_order_relaxed);
                    return;
                }

                RecursionGuard g(&local_obj);
                if (!RecursionGuard::is_recursing(&local_obj))
                {
                    thread_failed.store(true, std::memory_order_relaxed);
                    return;
                }

                if (depth > 0)
                {
                    recur(depth - 1, true);
                }
            };

            // Run a small recursion depth.
            recur(3, false);
        });
    }

    for (auto &th : threads) th.join();
    ASSERT_FALSE(thread_failed.load()) << "One or more threads failed the per-thread recursion check";

    // Part 2: shared-object check (thread A holds guard, thread B should NOT see is_recursing == true)
    int shared_obj = 0;
    std::promise<void> ready_promise;
    std::future<void> ready_future = ready_promise.get_future();
    std::promise<void> done_promise;
    std::future<void> done_future = done_promise.get_future();

    std::atomic<bool> other_thread_observed_recursing{false};

    // Thread A: construct guard and signal ready, then wait for done.
    std::thread threadA([&]() {
        RecursionGuard guard(&shared_obj); // register recursion for this thread only
        // signal ready to thread B
        ready_promise.set_value();
        // Wait for thread B to finish its check
        done_future.wait();
        // guard goes out of scope and unregisters here
    });

    // Thread B: wait until A signals it's holding the guard, then check is_recursing
    std::thread threadB([&]() {
        // wait for A to construct guard
        ready_future.wait();
        // thread B should NOT observe recursion for shared_obj while A holds it,
        // because recursion tracking should be thread-local.
        if (RecursionGuard::is_recursing(&shared_obj))
        {
            other_thread_observed_recursing.store(true, std::memory_order_relaxed);
        }
        // signal done so A can release
        done_promise.set_value();
    });

    threadA.join();
    threadB.join();

    ASSERT_FALSE(other_thread_observed_recursing.load()) << "Other thread observed recursion on shared object (should be thread-local)";
}
