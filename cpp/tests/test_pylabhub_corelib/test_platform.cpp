#include "platform.hpp"
#include "shared_test_helpers.h" // For StringCapture
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <regex>
#include <string>

using namespace pylabhub::platform;
using namespace ::testing;
using pylabhub::tests::helper::StringCapture;

// Helper to create a regex that looks for a file and line, handling backslashes.
static std::string GetLocationRegex(const char *file, int line)
{
    // In the output "in file at line X", escape the filename for regex matching.
    std::string escaped_file = std::regex_replace(file, std::regex(R"(\\)"), R"(\\\\)");
    return "in " + escaped_file + " at line " + std::to_string(line);
}

TEST(PlatformTest, DebugMsg)
{
    StringCapture stderr_capture(STDERR_FILENO);

    int line = __LINE__ + 2; // The line where PLH_DEBUG is called
    std::string test_message = "This is a test debug message with value 42.";
    PLH_DEBUG("This is a test debug message with value {}.", 42);

    std::string output = stderr_capture.GetOutput();

    EXPECT_THAT(output, HasSubstr("DEBUG MESSAGE:"));
    EXPECT_THAT(output, HasSubstr(test_message));
    EXPECT_THAT(output, MatchesRegex(".*" + GetLocationRegex(__FILE__, line) + ".*"));
}

TEST(PlatformTest, PrintStackTrace)
{
    StringCapture stderr_capture(STDERR_FILENO);

    print_stack_trace();

    std::string output = stderr_capture.GetOutput();

    EXPECT_THAT(output, HasSubstr("Stack Trace:"));
    // Check that there is *some* content after "Stack Trace:"
    EXPECT_THAT(output, Not(EndsWith("Stack Trace:\n")));
}

// A function that will be called by the panic test.
// The [[noreturn]] attribute is important for the compiler to understand it exits.
[[noreturn]] static void function_that_panics()
{
    PLH_PANIC("This is a panic test.");
}

TEST(PlatformTest, Panic)
{
    // EXPECT_DEATH runs the statement in a new process and checks its exit status and stderr.
    // We check that the stderr contains our fatal error message and the name of this file.
    // We cannot reliably check the line number from here, but checking the file is a good start.
    EXPECT_DEATH(function_that_panics(), AllOf(HasSubstr("FATAL ERROR: This is a panic test."),
                                               HasSubstr("test_platform.cpp")));
}
