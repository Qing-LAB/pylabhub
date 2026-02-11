/**
 * @file test_platform_debug.cpp
 * @brief Layer 0 tests for debug utilities (PLH_DEBUG, PLH_PANIC, stack traces)
 *
 * Tests cover:
 * - Debug message output and formatting
 * - Panic/abort behavior
 * - Stack trace generation
 * - Source location macros
 * - Format error handling
 */
#include "plh_base.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cstring>

#if defined(PYLABHUB_PLATFORM_POSIX)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace pylabhub::platform;
using namespace pylabhub::debug;
using namespace pylabhub::format_tools;
using namespace ::testing;
using pylabhub::tests::helper::StringCapture;

// ============================================================================
// Debug Message Tests
// ============================================================================

/**
 * Test PLH_DEBUG outputs correct message format
 * Note: PLH_DEBUG intentionally does NOT include source location to avoid
 * issues with variadic templates and incorrect location capture when wrapped.
 */
TEST(PlatformDebugTest, DebugMsg_BasicOutput)
{
    StringCapture capture(STDERR_FILENO);

    PLH_DEBUG("Test message with value {}", 42);

    std::string output = capture.GetOutput();

    // Verify message content (PLH_DEBUG outputs: "[DBG]  <message>\n")
    EXPECT_THAT(output, HasSubstr("[DBG]  Test message with value 42"));

    // Verify trailing newline
    EXPECT_THAT(output, EndsWith("\n"));

    // Verify format: should be exactly "[DBG]  Test message with value 42\n"
    EXPECT_EQ(output, "[DBG]  Test message with value 42\n");
}

/**
 * Test PLH_DEBUG with multiple arguments
 */
TEST(PlatformDebugTest, DebugMsg_MultipleArgs)
{
    StringCapture capture(STDERR_FILENO);

    PLH_DEBUG("Values: {}, {}, {}", 1, "test", 3.14);

    std::string output = capture.GetOutput();
    EXPECT_THAT(output, HasSubstr("Values: 1, test, 3.14"));
}

/**
 * Test debug_msg_rt with runtime format strings
 */
TEST(PlatformDebugTest, DebugMsg_RuntimeFormat)
{
    StringCapture capture(STDERR_FILENO);

    std::string runtime_fmt = "Runtime message: {}";
    debug_msg_rt(runtime_fmt, "dynamic");

    std::string output = capture.GetOutput();
    EXPECT_THAT(output, HasSubstr("Runtime message: dynamic"));
}

/**
 * Test debug_msg_rt handles format errors gracefully
 */
TEST(PlatformDebugTest, DebugMsg_FormatError)
{
    StringCapture capture(STDERR_FILENO);

    // Provide mismatched format string (expects 2 args, provide 1)
    std::string runtime_fmt = "Value: {} {}";
    debug_msg_rt(runtime_fmt, 123);

    std::string output = capture.GetOutput();

    // Should output format error message
    EXPECT_THAT(output, HasSubstr("FATAL FORMAT ERROR DURING DEBUG_MSG_RT"));
    EXPECT_THAT(output, HasSubstr("fmt_str['Value: {} {}']"));
    EXPECT_THAT(output, HasSubstr("Exception: 'argument not found'"));
}

// ============================================================================
// Source Location Macro Tests
// ============================================================================

/**
 * Test SRCLOC_TO_STR macro produces correct format
 */
TEST(PlatformDebugTest, SourceLocation_ToStringFormat)
{
    std::source_location loc = std::source_location::current();
    std::string result = SRCLOC_TO_STR(loc);

    // Expected format: filename:line:function_name
    std::string expected_filename{filename_only(loc.file_name())};

    EXPECT_THAT(result, StartsWith(expected_filename + ":"));
    EXPECT_THAT(result, HasSubstr(std::to_string(loc.line())));
    EXPECT_THAT(result, EndsWith(std::string(":") + loc.function_name()));
}

/**
 * Test PLH_LOC_HERE_STR captures correct location
 */
TEST(PlatformDebugTest, SourceLocation_HereString)
{
    std::string loc_str = PLH_LOC_HERE_STR;

    EXPECT_THAT(loc_str, HasSubstr(filename_only(__FILE__)));
    EXPECT_THAT(loc_str, HasSubstr(__func__));
    EXPECT_FALSE(loc_str.empty());
}

// ============================================================================
// Stack Trace Tests
// ============================================================================

/**
 * Test print_stack_trace() generates stack trace output
 *
 * Note: We redirect to a file instead of StringCapture to avoid deadlock
 * on Windows (DbgHelp library initialization can write to stderr).
 */
TEST(PlatformDebugTest, StackTrace_GeneratesOutput)
{
    const auto temp_path = std::filesystem::temp_directory_path() / "stack_trace.log";
    const std::string temp_path_str = temp_path.string();

#if defined(PYLABHUB_PLATFORM_WIN64)
    FILE *log_file = freopen(temp_path_str.c_str(), "w", stderr);
    ASSERT_NE(log_file, nullptr);
#else
    int stderr_copy = dup(fileno(stderr));
    int log_fd = ::open(temp_path_str.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_NE(log_fd, -1);
    dup2(log_fd, fileno(stderr));
    close(log_fd);
#endif

    print_stack_trace();
    fflush(stderr);

#if defined(PYLABHUB_PLATFORM_WIN64)
    freopen("NUL", "w", stderr);
#else
    dup2(stderr_copy, fileno(stderr));
    close(stderr_copy);
#endif

    // Read and verify output
    std::string output;
    ASSERT_TRUE(pylabhub::tests::helper::read_file_contents(temp_path_str, output));
    std::filesystem::remove(temp_path);

    EXPECT_THAT(output, HasSubstr("Stack Trace (most recent call first):"));
    EXPECT_FALSE(output.empty());
}

// ============================================================================
// Panic Tests
// ============================================================================

/**
 * Helper function that panics (must be separate function for EXPECT_DEATH)
 */
[[noreturn]] static void function_that_panics()
{
    PLH_PANIC("This is a test panic message");
}

/**
 * Test PLH_PANIC aborts with correct error message
 */
TEST(PlatformDebugTest, Panic_AbortsWithMessage)
{
    EXPECT_DEATH(function_that_panics(),
                 AllOf(HasSubstr("This is a test panic message"), HasSubstr("PANIC"),
                       HasSubstr("Stack Trace (most recent call first):")));
}

/**
 * Helper with formatted panic message
 */
[[noreturn]] static void function_with_formatted_panic()
{
    PLH_PANIC("Panic with value: {}", 42);
}

/**
 * Test PLH_PANIC supports formatted messages
 */
TEST(PlatformDebugTest, Panic_SupportsFormatting)
{
    EXPECT_DEATH(function_with_formatted_panic(), HasSubstr("Panic with value: 42"));
}
