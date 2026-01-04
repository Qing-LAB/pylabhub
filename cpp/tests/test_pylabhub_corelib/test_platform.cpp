#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>

#if PYLABHUB_IS_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h> // for open()
#endif

#include "platform.hpp"
#include "shared_test_helpers.h" // For StringCapture & read_file_contents
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace pylabhub::platform;
using namespace pylabhub::debug;
using namespace ::testing;
using pylabhub::tests::helper::StringCapture;

// Utility: escape any regex metacharacters in `s`
static std::string RegexEscape(const std::string &s)
{
    static const std::string meta = R"(\.^$|()[]*+?{})"; // set of regex metachars
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s)
    {
        if (meta.find(c) != std::string::npos)
        {
            out.push_back('\\'); // escape it
        }
        out.push_back(c);
    }
    return out;
}

static std::string GetLocationRegex(const char *file, int line)
{
    // Create a regex that matches the file and line number in the format used by PLH_DEBUG
    // Example: "at tests/test_pylabhub_corelib/test_platform.cpp:42"
    std::string file_escaped = RegexEscape(file ? file : "");
    return ".*in " + file_escaped + ":" + std::to_string(line) + ".*";
}

TEST(PlatformTest, DebugMsg)
{
    StringCapture stderr_capture(STDERR_FILENO);

    int line = __LINE__ + 2; // The line where PLH_DEBUG is called
    std::string test_message = "This is a test debug message with value 42.";
    PLH_DEBUG("This is a test debug message with value {}.", 42);

    std::string output = stderr_capture.GetOutput();

    auto regex_str = GetLocationRegex(__FILE__, line);
    if (!output.empty() && output.back() == '\n')
        output.pop_back();

    EXPECT_THAT(output, ::testing::HasSubstr("DEBUG MESSAGE:"));
    EXPECT_THAT(output, ::testing::HasSubstr(test_message));
    EXPECT_THAT(output, ::testing::MatchesRegex(regex_str));
}

TEST(PlatformTest, PrintStackTrace)
{
    // IMPORTANT: This test redirects stderr to a file instead of using the
    // StringCapture helper. This is to avoid a deadlock specific to the Windows
    // implementation of print_stack_trace.
    //
    // The Deadlock Explained:
    // 1. `print_stack_trace` uses the DbgHelp library (`DbgHelp.dll`) on Windows.
    // 2. The first time it's called, DbgHelp must initialize by calling `SymInitialize`.
    //    This function inspects all loaded modules and can be slow.
    // 3. During this initialization, DbgHelp may write its own status or error
    //    messages to the stderr stream.
    // 4. The `StringCapture` helper redirects stderr to a fixed-size pipe. If
    //    DbgHelp writes enough data to fill this pipe, it will block, waiting for
    //    the pipe to be read.
    // 5. However, the test is also blocked, waiting for `print_stack_trace` to
    //    return before it calls `GetOutput()` to read the pipe.
    //
    // This creates a classic deadlock: the function waits for the pipe, and the
    // pipe-reader waits for the function. Using a file for redirection avoids
    // this, as file I/O is handled differently by the OS and is not prone to
    // this specific type of blocking.
    const auto temp_path = std::filesystem::temp_directory_path() / "stack_trace.log";
    const std::string temp_path_str = temp_path.string();

#ifdef _WIN32
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

#ifdef _WIN32
    // On Windows, freopen replaces the stderr stream. To restore it, we need to
    // reopen the "CONOUT$" stream, but a simpler approach for tests is to
    // just reopen a null device, as the primary check is done on the log file.
    freopen("NUL", "w", stderr); // Redirect to null device
#else
    dup2(stderr_copy, fileno(stderr));
    close(stderr_copy);
#endif

    // Read the output from the log file
    std::string output;
    ASSERT_TRUE(pylabhub::tests::helper::read_file_contents(temp_path_str, output));
    std::filesystem::remove(temp_path); // Clean up

    EXPECT_THAT(output, HasSubstr("Stack Trace (most recent call first):"));
    EXPECT_THAT(output, Not(EndsWith("Stack Trace (most recent call first):\n")));
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
