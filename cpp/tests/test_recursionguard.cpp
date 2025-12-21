// tests/test_recursionguard.cpp
//
// Unit test for pylabhub::utils::RecursionGuard, converted to GoogleTest.

#include <gtest/gtest.h>
#include "utils/RecursionGuard.hpp"
#include <functional>
#include <memory>

#include "test_main.h"


using namespace pylabhub::utils;

namespace {

// Test Functions and Objects
int some_object;
int another_object;

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

TEST(RecursionGuardTest, SingleObjectRecursion)
{
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
    RecursiveFunction(3, false);
    ASSERT_FALSE(RecursionGuard::is_recursing(&some_object));
}

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

TEST(RecursionGuardTest, OutOfOrderDestruction)
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