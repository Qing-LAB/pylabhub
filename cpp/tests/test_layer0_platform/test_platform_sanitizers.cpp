/**
 * @file test_platform_sanitizers.cpp
 * @brief Layer 0 tests for sanitizer detection (TSan, ASan, UBSan)
 *
 * Run trigger code in a death-test subprocess via EXPECT_EXIT; pass iff
 * captured stderr contains the expected sanitizer report. Exit code is ignored.
 *
 * We use plain regex strings (not HasSubstr) so GTest's death test applies
 * ContainsRegex to the captured output. Set --gtest_death_test_style=threadsafe
 * so the child is re-exec'd and its stderr is captured reliably (avoids
 * unflushed buffers in the "fast" fork-only style).
 */
#include "plh_platform.hpp"
#include <cassert>
#include <gtest/gtest.h>
#include <limits>
#include <thread>

// Accept any exit code; we only care about stderr content
static bool any_exit(int) { return true; }

// ============================================================================
// ThreadSanitizer (TSan) Tests
// ============================================================================

#ifdef PYLABHUB_SANITIZER_IS_THREAD

TEST(SanitizerTest, TSan_DetectsDataRace)
{
    auto trigger = []() {
        long v = 0;
        std::thread t1([&] { for (int i = 0; i < 1000; ++i) v++; });
        std::thread t2([&] { for (int i = 0; i < 1000; ++i) v++; });
        t1.join();
        t2.join();
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    };
    EXPECT_EXIT(trigger(), any_exit, "ThreadSanitizer: data race");
}

#endif // PYLABHUB_SANITIZER_IS_THREAD

// ============================================================================
// AddressSanitizer (ASan) Tests
// ============================================================================

#ifdef PYLABHUB_SANITIZER_IS_ADDRESS

TEST(SanitizerTest, ASan_DetectsHeapBufferOverflowWrite)
{
    auto trigger = []() {
        int *a = new int[10];
        *(volatile int *)&a[100] = 0;
        delete[] a;
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    };
    EXPECT_EXIT(trigger(), any_exit, "AddressSanitizer: heap-buffer-overflow");
}

TEST(SanitizerTest, ASan_DetectsHeapBufferOverflowRead)
{
    auto trigger = []() {
        int *a = new int[10];
        (void)(volatile int)a[100];
        delete[] a;
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    };
    EXPECT_EXIT(trigger(), any_exit, "AddressSanitizer: heap-buffer-overflow");
}

TEST(SanitizerTest, ASan_DetectsHeapUseAfterFree)
{
    auto trigger = []() {
        int *a = new int[10];
        delete[] a;
        (void)(volatile int)a[5];
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    };
    EXPECT_EXIT(trigger(), any_exit, "AddressSanitizer: heap-use-after-free");
}

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

TEST(SanitizerTest, ASan_DetectsStackBufferOverflow)
{
    NOINLINE static void trigger() {
        volatile char buf[256];
        buf[0] = 1;
        volatile char *p = buf;
        p[256] = 0;
        (void)p;
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    }
    EXPECT_EXIT(trigger(), any_exit, "AddressSanitizer: stack-buffer-overflow");
}

#endif // PYLABHUB_SANITIZER_IS_ADDRESS

// ============================================================================
// UndefinedBehaviorSanitizer (UBSan) Tests
// ============================================================================

#ifdef PYLABHUB_SANITIZER_IS_UNDEFINED

TEST(SanitizerTest, UBSan_DetectsSignedIntegerOverflow)
{
    auto trigger = []() {
        volatile int v = std::numeric_limits<int>::max();
        v++;
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    };
    EXPECT_EXIT(trigger(), any_exit, "runtime error: signed integer overflow");
}

TEST(SanitizerTest, UBSan_DetectsDivisionByZero)
{
    auto trigger = []() {
        volatile int x = 42, y = 0;
        (void)(x / y);
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    };
    EXPECT_EXIT(trigger(), any_exit, "runtime error: division by zero");
}

TEST(SanitizerTest, UBSan_DetectsNullPointerDereference)
{
    auto trigger = []() {
        volatile int *p = nullptr;
        (void)*p;
        assert(false);  // force abnormal exit so GTest evaluates the stderr matcher
    };
    EXPECT_EXIT(trigger(), any_exit, "runtime error|null|SIGSEGV|signal");
}

#endif // PYLABHUB_SANITIZER_IS_UNDEFINED

// ============================================================================
// No Sanitizer Tests (Smoke Tests)
// ============================================================================

#if !defined(PYLABHUB_SANITIZER_IS_THREAD) && !defined(PYLABHUB_SANITIZER_IS_ADDRESS) &&           \
    !defined(PYLABHUB_SANITIZER_IS_UNDEFINED)

TEST(SanitizerTest, NoSanitizer_PlaceholderTest)
{
    SUCCEED() << "No sanitizer active, skipping sanitizer detection tests";
}

#endif
