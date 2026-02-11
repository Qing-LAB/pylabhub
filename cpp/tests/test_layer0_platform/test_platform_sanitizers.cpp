/**
 * @file test_platform_sanitizers.cpp
 * @brief Layer 0 tests for sanitizer detection (TSan, ASan, UBSan)
 *
 * These tests verify that sanitizers are working correctly by intentionally
 * triggering detectable errors. Tests are conditionally compiled based on
 * which sanitizer is active.
 *
 * Note: These tests use EXPECT_DEATH which spawns a subprocess, so the
 * sanitizer errors don't crash the main test process.
 */
#include "plh_platform.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <limits>

using namespace ::testing;

// ============================================================================
// ThreadSanitizer (TSan) Tests
// ============================================================================

#ifdef PYLABHUB_SANITIZER_IS_THREAD

/**
 * Test TSan detects data races
 *
 * This test intentionally creates a data race by having two threads
 * increment a shared non-atomic variable without synchronization.
 */
TEST(SanitizerTest, TSan_DetectsDataRace)
{
    auto data_race_func = []()
    {
        long shared_value = 0;

        std::thread t1(
            [&]()
            {
                for (int i = 0; i < 1000; ++i)
                    shared_value++;
            });

        std::thread t2(
            [&]()
            {
                for (int i = 0; i < 1000; ++i)
                    shared_value++;
            });

        t1.join();
        t2.join();
    };

    EXPECT_DEATH(data_race_func(), ".*ThreadSanitizer: data race.*\n");
}

#endif // PYLABHUB_SANITIZER_IS_THREAD

// ============================================================================
// AddressSanitizer (ASan) Tests
// ============================================================================

#ifdef PYLABHUB_SANITIZER_IS_ADDRESS

/**
 * Test ASan detects heap buffer overflow (write)
 */
TEST(SanitizerTest, ASan_DetectsHeapBufferOverflowWrite)
{
    auto overflow_func = []()
    {
        int *array = new int[10];
        // Use volatile to prevent optimization
        *(volatile int *)&array[100] = 0;
        delete[] array;
    };

    EXPECT_DEATH(overflow_func(), ".*AddressSanitizer: heap-buffer-overflow.*\n");
}

/**
 * Test ASan detects heap buffer overflow (read)
 */
TEST(SanitizerTest, ASan_DetectsHeapBufferOverflowRead)
{
    auto overflow_func = []()
    {
        int *array = new int[10];
        int volatile x = array[100]; // Trigger overflow
        (void)x;                     // Avoid unused variable warning
        delete[] array;
    };

    EXPECT_DEATH(overflow_func(), ".*AddressSanitizer: heap-buffer-overflow.*\n");
}

/**
 * Test ASan detects heap-use-after-free
 */
TEST(SanitizerTest, ASan_DetectsHeapUseAfterFree)
{
    auto use_after_free_func = []()
    {
        int *array = new int[10];
        delete[] array;
        int volatile x = array[5]; // Use after free
        (void)x;
    };

    EXPECT_DEATH(use_after_free_func(), ".*AddressSanitizer: heap-use-after-free.*\n");
}

// Cross-platform no-inline attribute
#if defined(_MSC_VER)
#define PYLABHUB_NOINLINE __declspec(noinline)
#else
#define PYLABHUB_NOINLINE __attribute__((noinline))
#endif

/**
 * Helper function to trigger stack buffer overflow
 * Must be no-inline to ensure distinct stack frame
 */
PYLABHUB_NOINLINE
static void trigger_stack_overflow()
{
    volatile char buf[256];
    buf[0] = 1; // Use buffer to prevent optimization

    // Write one byte past the end (most likely to hit ASan redzone)
    volatile char *p = buf;
    p[256] = 0;

    asm volatile("" ::: "memory"); // Compiler barrier
}

/**
 * Test ASan detects stack buffer overflow
 */
TEST(SanitizerTest, ASan_DetectsStackBufferOverflow)
{
    EXPECT_DEATH(trigger_stack_overflow(), ".*AddressSanitizer: stack-buffer-overflow.*\n");
}

#endif // PYLABHUB_SANITIZER_IS_ADDRESS

// ============================================================================
// UndefinedBehaviorSanitizer (UBSan) Tests
// ============================================================================

#ifdef PYLABHUB_SANITIZER_IS_UNDEFINED

/**
 * Test UBSan detects signed integer overflow
 */
TEST(SanitizerTest, UBSan_DetectsSignedIntegerOverflow)
{
    auto overflow_func = []()
    {
        // Use volatile to prevent compiler optimization
        volatile int value = std::numeric_limits<int>::max();
        value++;
    };

    EXPECT_DEATH(overflow_func(), ".*runtime error: signed integer overflow.*\n");
}

/**
 * Test UBSan detects division by zero
 */
TEST(SanitizerTest, UBSan_DetectsDivisionByZero)
{
    auto div_by_zero_func = []()
    {
        volatile int x = 42;
        volatile int y = 0;
        volatile int z = x / y;
        (void)z;
    };

    EXPECT_DEATH(div_by_zero_func(), ".*runtime error: division by zero.*\n");
}

/**
 * Test UBSan detects null pointer dereference
 */
TEST(SanitizerTest, UBSan_DetectsNullPointerDereference)
{
    auto null_deref_func = []()
    {
        volatile int *ptr = nullptr;
        volatile int x = *ptr;
        (void)x;
    };

    // UBSan detects this as a null pointer access
    // Note: On some platforms, this might trigger a segfault before UBSan catches it
    EXPECT_DEATH(null_deref_func(), ".*");
}

#endif // PYLABHUB_SANITIZER_IS_UNDEFINED

// ============================================================================
// No Sanitizer Tests (Smoke Tests)
// ============================================================================

#if !defined(PYLABHUB_SANITIZER_IS_THREAD) && !defined(PYLABHUB_SANITIZER_IS_ADDRESS) &&           \
    !defined(PYLABHUB_SANITIZER_IS_UNDEFINED)

/**
 * When no sanitizer is active, provide a placeholder test
 */
TEST(SanitizerTest, NoSanitizer_PlaceholderTest)
{
    SUCCEED() << "No sanitizer active, skipping sanitizer detection tests";
}

#endif
