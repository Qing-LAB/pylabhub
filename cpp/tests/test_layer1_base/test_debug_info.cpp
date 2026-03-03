/**
 * @file test_debug_info.cpp
 * @brief Tests for debug_info.hpp — stack trace, panic, debug messaging.
 *
 * PLH_PANIC is tested with EXPECT_DEATH since it calls std::abort().
 * debug_msg_rt is tested directly (not via PLH_DEBUG_RT macro, which
 * is a no-op unless PYLABHUB_ENABLE_DEBUG_MESSAGES is defined).
 */
#include <plh_base.hpp>

#include <gtest/gtest.h>

#include <source_location>
#include <string>

using namespace pylabhub::debug;

// ============================================================================
// print_stack_trace
// ============================================================================

TEST(DebugInfoTest, PrintStackTrace_NoExternalTools)
{
    // Should not crash; output goes to stderr
    EXPECT_NO_FATAL_FAILURE(print_stack_trace(false));
}

TEST(DebugInfoTest, PrintStackTrace_WithExternalTools)
{
    // May invoke addr2line on POSIX; should not crash regardless
    EXPECT_NO_FATAL_FAILURE(print_stack_trace(true));
}

// ============================================================================
// PLH_PANIC
// ============================================================================

TEST(DebugInfoTest, Panic_Aborts)
{
    EXPECT_DEATH(PLH_PANIC("test {}", 42), "PANIC.*test 42");
}

TEST(DebugInfoTest, Panic_IncludesSourceLocation)
{
    EXPECT_DEATH(PLH_PANIC("msg"), "test_debug_info\\.cpp");
}

// ============================================================================
// debug_msg_rt (runtime format string)
// ============================================================================

TEST(DebugInfoTest, DebugMsgRt_DoesNotCrash)
{
    // Writes to stderr; verify no crash
    EXPECT_NO_FATAL_FAILURE(debug_msg_rt("hello {}", 42));
}

TEST(DebugInfoTest, DebugMsgRt_FormatError_Swallowed)
{
    // Too few args for format string — should not crash (error swallowed internally)
    EXPECT_NO_FATAL_FAILURE(debug_msg_rt("{} {} {}", 1));
}

// ============================================================================
// SRCLOC_TO_STR
// ============================================================================

TEST(DebugInfoTest, SrclocToStr_Format)
{
    auto loc = std::source_location::current();
    std::string s = SRCLOC_TO_STR(loc);
    // Should contain ":" separators and function name
    EXPECT_NE(s.find(':'), std::string::npos);
    // Should contain this test function name
    EXPECT_NE(s.find("SrclocToStr_Format"), std::string::npos);
}
