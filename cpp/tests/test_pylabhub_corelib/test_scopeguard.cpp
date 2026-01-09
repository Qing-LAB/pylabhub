// tests/test_pylabhub_corelib/test_scopeguard.cpp
/**
 * @file test_scopeguard.cpp
 * @brief Unit tests for the ScopeGuard class.
 *
 * This file contains a suite of tests for the `pylabhub::basics::ScopeGuard`,
 * a utility designed to execute a function upon scope exit.
 */
#include <gtest/gtest.h>
#include <utility>

#include "scope_guard.hpp"

using pylabhub::basics::ScopeGuard;
using pylabhub::basics::make_scope_guard;

// Test that the ScopeGuard executes its function on scope exit.
TEST(ScopeGuardTest, ExecutesOnScopeExit)
{
    bool executed = false;
    {
        auto guard = make_scope_guard([&]() { executed = true; });
        ASSERT_FALSE(executed);
    }
    ASSERT_TRUE(executed);
}

// Test that a dismissed ScopeGuard does not execute its function.
TEST(ScopeGuardTest, Dismiss)
{
    bool executed = false;
    {
        auto guard = make_scope_guard([&]() { executed = true; });
        guard.dismiss();
        ASSERT_FALSE(executed);
    }
    ASSERT_FALSE(executed);
}

// Test that invoke() executes the function immediately and dismisses the guard.
TEST(ScopeGuardTest, Invoke)
{
    bool executed = false;
    {
        auto guard = make_scope_guard([&]() { executed = true; });
        ASSERT_FALSE(executed);
        guard.invoke();
        ASSERT_TRUE(executed);
    }
    // To prove it was dismissed, we reset the flag. If the destructor runs it,
    // the assertion will fail.
    executed = false;
    ASSERT_FALSE(executed);
}

// Test that moving a ScopeGuard transfers ownership of the function call.
TEST(ScopeGuardTest, MoveConstruction)
{
    bool executed = false;
    {
        auto guard1 = make_scope_guard([&]() { executed = true; });
        {
            ScopeGuard guard2(std::move(guard1));
            ASSERT_FALSE(executed);
        } // guard2 goes out of scope here
        ASSERT_TRUE(executed);
    }
}

// Test that a moved-from ScopeGuard does not execute.
TEST(ScopeGuardTest, MovedFromGuardIsInactive)
{
    bool executed = false;
    {
        auto guard1 = make_scope_guard([&]() { executed = true; });
        ScopeGuard guard2(std::move(guard1));
        // guard1 is now moved-from and should be inactive.
        // Its destructor should do nothing.
    }
    // If guard1 executed, this would be true. We expect it to be false.
    ASSERT_FALSE(executed);
}

// Test that exceptions from the guarded function are swallowed in the destructor.
TEST(ScopeGuardTest, ExceptionInDestructorIsSwallowed)
{
    auto make_and_destroy_guard = []()
    {
        auto guard = make_scope_guard([]() { throw std::runtime_error("Test"); });
    };

    // The destructor should not propagate the exception.
    EXPECT_NO_THROW(make_and_destroy_guard());
}

// Test that exceptions from invoke() are swallowed.
TEST(ScopeGuardTest, ExceptionInInvokeIsSwallowed)
{
    auto guard = make_scope_guard([]() { throw std::runtime_error("Test"); });

    // invoke() should not propagate the exception.
    EXPECT_NO_THROW(guard.invoke());
}
