// test_recursionguard.cpp
// Unit test for pylabhub::utils::RecursionGuard

#include "utils/RecursionGuard.hpp"
#include <fmt/core.h>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace pylabhub::utils;

// --- Minimal Test Harness ---
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fmt::print(stderr, "  CHECK FAILED: {} at {}:{}\n", #condition, __FILE__, __LINE__);   \
            throw std::runtime_error("Test case failed");                                          \
        }                                                                                          \
    } while (0)

void TEST_CASE(const std::string &name, std::function<void()> test_func)
{
    fmt::print("\n=== {} ===\n", name);
    try
    {
        test_func();
        tests_passed++;
        fmt::print("  --- PASSED ---\n");
    }
    catch (const std::exception &e)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED: {} ---\n", e.what());
    }
    catch (...)
    {
        tests_failed++;
        fmt::print(stderr, "  --- FAILED with unknown exception ---\n");
    }
}

// --- Test Functions and Objects ---

int some_object;
int another_object;

// implementation:
void recursive_function(int depth, bool expect_recursing)
{
    // At the start of the *outermost* call expect_recursing == false.
    // For inner recursive calls expect_recursing == true because an outer guard exists.
    CHECK(RecursionGuard::is_recursing(&some_object) == expect_recursing);

    RecursionGuard g(&some_object);
    // after constructing the guard, it should always be true
    CHECK(RecursionGuard::is_recursing(&some_object));

    if (depth > 0)
    {
        // inner calls will see the outer guard -> expect_recursing == true
        recursive_function(depth - 1, /*expect_recursing=*/true);
    }
}

//
// [Purpose]
// Tests the primary use case of the `RecursionGuard`: detecting direct re-entrant
// calls for a single object.
//
// [Method]
// - A helper function, `recursive_function`, creates a `RecursionGuard` on a
//   global object (`some_object`).
// - The test first checks that, before any calls, `is_recursing()` is false.
// - It then calls `recursive_function(3)`, which will recursively call itself
//   three times.
// - Inside `recursive_function`, it's verified that `is_recursing()` is `false`
//   at the beginning of the *outermost* call, but `true` for all subsequent
//   *inner* (recursive) calls. This confirms the guard correctly identifies
//   re-entrance.
// - After the top-level call returns, the test verifies that `is_recursing()`
//   is once again false.
void test_single_object_recursion()
{
    CHECK(!RecursionGuard::is_recursing(&some_object));
    recursive_function(3, false);
    CHECK(!RecursionGuard::is_recursing(&some_object));
}

//
// [Purpose]
// Verifies that `RecursionGuard`s for different object instances are tracked
// independently. A guard on one object should not affect the recursion state
// of another.
//
// [Method]
// 1. Creates a guard `g1` for `some_object`. Verifies `is_recursing()` is true
//    for `some_object` but false for `another_object`.
// 2. Within the scope of `g1`, creates a nested guard `g2` for `another_object`.
//    Verifies `is_recursing()` is now true for *both* objects.
// 3. `g2` is destructed. Verifies the state for `another_object` returns to
//    false, while `some_object` remains true.
// 4. `g1` is destructed. Verifies the state for `some_object` also returns to
//    false.
void test_multiple_objects()
{
    CHECK(!RecursionGuard::is_recursing(&some_object));
    CHECK(!RecursionGuard::is_recursing(&another_object));

    {
        RecursionGuard g1(&some_object);
        CHECK(RecursionGuard::is_recursing(&some_object));
        CHECK(!RecursionGuard::is_recursing(&another_object));

        {
            RecursionGuard g2(&another_object);
            CHECK(RecursionGuard::is_recursing(&some_object));
            CHECK(RecursionGuard::is_recursing(&another_object));
        }

        CHECK(RecursionGuard::is_recursing(&some_object));
        CHECK(!RecursionGuard::is_recursing(&another_object));
    }

    CHECK(!RecursionGuard::is_recursing(&some_object));
    CHECK(!RecursionGuard::is_recursing(&another_object));
}

//
// [Purpose]
// Tests robustness against an edge case where nested guards are destructed out of
// LIFO (Last-In, First-Out) order. A naive stack-based implementation might fail
// here, but a robust implementation should handle it gracefully.
//
// [Method]
// 1. Creates guard `g1` for `some_object` on the heap.
// 2. Creates guard `g2` for `another_object` on the heap.
// 3. Destroys `g1` (the "outer" guard) *before* `g2` (the "inner" guard).
// 4. Verifies that the recursion state for `some_object` is correctly cleared,
//    while the state for `another_object` remains unaffected.
// 5. Destroys `g2` and verifies its state is also cleared. This proves the
//    internal tracking mechanism is not a simple stack and can handle out-of-order
//    cleanup.
void test_out_of_order_destruction()
{
    CHECK(!RecursionGuard::is_recursing(&some_object));
    CHECK(!RecursionGuard::is_recursing(&another_object));

    auto g1 = std::make_unique<RecursionGuard>(&some_object);
    CHECK(RecursionGuard::is_recursing(&some_object));

    auto g2 = std::make_unique<RecursionGuard>(&another_object);
    CHECK(RecursionGuard::is_recursing(&another_object));

    // Destroy g1 (the outer guard) before g2 (the inner guard).
    // The defensive logic in the destructor should handle this.
    g1.reset();
    CHECK(!RecursionGuard::is_recursing(&some_object));
    CHECK(RecursionGuard::is_recursing(&another_object));

    g2.reset();
    CHECK(!RecursionGuard::is_recursing(&another_object));
}

// --- Main Test Runner ---
int main()
{
    fmt::print("--- RecursionGuard Test Suite ---\n");

    some_object = 0;
    another_object = 0;

    TEST_CASE("Single Object Direct Recursion", test_single_object_recursion);
    TEST_CASE("Multiple Objects Interleaved", test_multiple_objects);
    TEST_CASE("Out-of-Order Destruction", test_out_of_order_destruction);

    fmt::print("\n--- Test Summary ---\n");
    fmt::print("Passed: {}, Failed: {}\n", tests_passed, tests_failed);

    return tests_failed == 0 ? 0 : 1;
}
