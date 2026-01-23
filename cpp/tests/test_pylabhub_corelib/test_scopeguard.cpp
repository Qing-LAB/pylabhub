// tests/test_pylabhub_corelib/test_scopeguard.cpp
/**
 * @file test_scopeguard.cpp
 * @brief Unit tests for the ScopeGuard class.
 *
 * This file contains a suite of tests for the `pylabhub::basics::ScopeGuard`,
 * a utility designed to execute a function upon scope exit. The tests cover
 * basic execution, dismissal, move semantics, exception handling, and various
 * corner cases.
 */
#include "plh_base.hpp" // For plh_base types and std utilities
#include <gtest/gtest.h>
#include <atomic>
#include <stdexcept>
#include <functional>

using pylabhub::basics::make_scope_guard;
using pylabhub::basics::ScopeGuard;

// Test that the ScopeGuard executes its function on normal scope exit.
TEST(ScopeGuardTest, ExecutesOnScopeExit)
{
    bool executed = false;
    {
        auto guard = make_scope_guard([&]() { executed = true; });
        ASSERT_FALSE(executed);
    }
    ASSERT_TRUE(executed);
}

// Test that a guard created from an L-value lambda executes correctly.
TEST(ScopeGuardTest, ExecutesWithLvalueLambda)
{
    bool executed = false;
    auto my_lambda = [&]() { executed = true; };
    {
        auto guard = make_scope_guard(my_lambda);
        ASSERT_FALSE(executed);
    }
    ASSERT_TRUE(executed);
}

// Test that a mutable lambda with internal state works as expected.
TEST(ScopeGuardTest, StatefulMutableLambda)
{
    int counter = 0;
    {
        auto guard = make_scope_guard([i = 0, &counter]() mutable {
            i++;
            counter = i;
        });
    }
    ASSERT_EQ(counter, 1);
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

// Test that calling dismiss() multiple times is safe.
TEST(ScopeGuardTest, DismissIdempotency)
{
    bool executed = false;
    auto guard = make_scope_guard([&]() { executed = true; });
    guard.dismiss();
    guard.dismiss(); // Second call should have no effect.
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

        // To prove it was dismissed, we reset the flag. If the destructor runs
        // it again, the final assertion will fail.
        executed = false;
    }
    ASSERT_FALSE(executed);
}

// Test that calling invoke() multiple times is safe and executes only once.
TEST(ScopeGuardTest, InvokeIdempotency)
{
    int execution_count = 0;
    auto guard = make_scope_guard([&]() { execution_count++; });
    guard.invoke();
    ASSERT_EQ(execution_count, 1);

    guard.invoke(); // This call should do nothing.
    ASSERT_EQ(execution_count, 1);
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
        } // guard2 goes out of scope here and should execute.
        ASSERT_TRUE(executed);
    }
}

// Test that a moved-from ScopeGuard does not execute. This is critical for
// ensuring the cleanup action is not performed twice.
TEST(ScopeGuardTest, MovedFromGuardIsInactive)
{
    std::atomic<int> execution_count = 0;
    {
        auto guard1 = make_scope_guard([&]() { execution_count++; });
        ScopeGuard guard2(std::move(guard1));
        // guard1 is now moved-from and should be inactive.
        // Its destructor at the end of this scope should do nothing.
    }
    // Only guard2, which was moved to and then went out of scope, should have
    // executed the callable.
    ASSERT_EQ(execution_count.load(), 1);
}

// Test that exceptions from the guarded function are swallowed in the destructor.
// This is required to prevent std::terminate during stack unwinding.
TEST(ScopeGuardTest, ExceptionInDestructorIsSwallowed)
{
    auto make_and_destroy_guard = []()
    {
        auto guard = make_scope_guard([]() { throw std::runtime_error("Test"); });
        // The guard's destructor runs here and must not propagate the exception.
    };
    EXPECT_NO_THROW(make_and_destroy_guard());
}

// Test that exceptions from invoke() are also swallowed.
TEST(ScopeGuardTest, ExceptionInInvokeIsSwallowed)
{
    auto guard = make_scope_guard([]() { throw std::runtime_error("Test"); });
    EXPECT_NO_THROW(guard.invoke());
}

// Test that the guard works with std::function.
TEST(ScopeGuardTest, CreateFromStdFunction)
{
    bool executed = false;
    std::function<void()> my_func = [&]() { executed = true; };
    {
        auto guard = make_scope_guard(my_func);
        ASSERT_FALSE(executed);
    }
    ASSERT_TRUE(executed);
}

// Statically verify the noexcept contract of the move constructor.
TEST(ScopeGuardTest, NoexceptCorrectness)
{
    auto f = []() {};
    using GuardType = decltype(make_scope_guard(f));
    static_assert(std::is_nothrow_move_constructible_v<GuardType>,
                  "ScopeGuard should be nothrow move constructible.");

    auto nf = []() noexcept {};
    using NoexceptGuardType = decltype(make_scope_guard(nf));
    static_assert(std::is_nothrow_move_constructible_v<NoexceptGuardType>,
                  "ScopeGuard with noexcept lambda should be nothrow move constructible.");
}

// Test the explicit operator bool() for an active guard.
TEST(ScopeGuardTest, OperatorBoolActive)
{
    auto guard = make_scope_guard([]() {});
    EXPECT_TRUE(static_cast<bool>(guard));
}

// Test the explicit operator bool() for a dismissed guard.
TEST(ScopeGuardTest, OperatorBoolDismissed)
{
    auto guard = make_scope_guard([]() {});
    guard.dismiss();
    EXPECT_FALSE(static_cast<bool>(guard));
}

// Test the explicit operator bool() for a moved-from guard.
TEST(ScopeGuardTest, OperatorBoolMovedFrom)
{
    auto guard1 = make_scope_guard([]() {});
    EXPECT_TRUE(static_cast<bool>(guard1));
    auto guard2 = std::move(guard1);
    EXPECT_FALSE(static_cast<bool>(guard1)); // guard1 should be inactive
    EXPECT_TRUE(static_cast<bool>(guard2));  // guard2 should be active
}

// Test that release() is an effective alias for dismiss().
TEST(ScopeGuardTest, ReleaseAlias)
{
    bool executed = false;
    {
        auto guard = make_scope_guard([&]() { executed = true; });
        guard.release(); // Should behave like dismiss()
        ASSERT_FALSE(executed);
    }
    ASSERT_FALSE(executed);
}

// Test that invoke_and_rethrow() executes and dismisses.
TEST(ScopeGuardTest, InvokeAndRethrowExecutesAndDismisses)
{
    int count = 0;
    {
        auto guard = make_scope_guard([&]() { count++; });
        guard.invoke_and_rethrow();
        ASSERT_EQ(count, 1);
        count = 0; // Reset to check if destructor re-runs it
    }
    ASSERT_EQ(count, 0); // Should not have executed again
}

// Test that invoke_and_rethrow() propagates exceptions.
TEST(ScopeGuardTest, InvokeAndRethrowPropagatesException)
{
    auto guard = make_scope_guard([]() { throw std::runtime_error("Test Propagate"); });
    EXPECT_THROW(guard.invoke_and_rethrow(), std::runtime_error);
}

// Test to verify the bug fix in invoke(): if the callable throws,
// the guard is still dismissed and does not re-execute on scope exit.
TEST(ScopeGuardTest, InvokeIsDismissedOnException)
{
    int execution_count = 0;
    try
    {
        auto guard = make_scope_guard([&]()
                                      {
                                          execution_count++;
                                          throw std::runtime_error("Test");
                                      });
        EXPECT_NO_THROW(guard.invoke()); // Exception is swallowed
    }
    catch (...)
    {
        // Should not be reached.
        FAIL() << "Exception was not swallowed by invoke()";
    }

    // If the guard was correctly dismissed inside invoke() before throwing,
    // the count should remain 1. If not, the destructor would have run it again.
    ASSERT_EQ(execution_count, 1);
}

// A compile-time test to verify static_asserts.
// This test doesn't run, but serves as documentation. It is expected to fail compilation
// if uncommented, proving the static_asserts work.
/*
TEST(ScopeGuardTest, StaticAsserts)
{
    // 1. Should fail: instantiating with a reference
    // auto f = [](){};
    // ScopeGuard<decltype(f)&> ref_guard(f);
    // 2. Should fail: using a type that is not copyable or movable
    struct NonConstructible {
        NonConstructible() = default;
        NonConstructible(const NonConstructible&) = delete;
        NonConstructible(NonConstructible&&) = delete;
        void operator()() {}
    };
    // auto guard = make_scope_guard(NonConstructible{});
}
*/