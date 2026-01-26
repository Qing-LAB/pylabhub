#include "plh_base.hpp"
#include "plh_service.hpp"

#if PYLABHUB_IS_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h> // for open()
#endif

#include "shared_test_helpers.h" // For StringCapture & read_file_contents
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "utils/format_tools.hpp" // Required for SRCLOC_TO_STR and filename_only

using namespace pylabhub::platform;
using namespace pylabhub::debug;
using namespace pylabhub::format_tools; // Added for filename_only in SRCLOC_TO_STR test
using namespace ::testing;
using pylabhub::tests::helper::StringCapture;

// #include "string_capture.h" // <-- include your project's StringCapture definition if needed
// #include "debug.h" // <-- include your PLH_DEBUG / debug_msg macros if needed



// The test checks the three important pieces separately:
//  - the debug preamble and file path
//  - the location fragment containing line number and func marker
//  - the message body (with optional trailing newline)

TEST(PlatformTest, DebugMsg)
{
    // Capture stderr output (assumes your test environment has this helper)
    StringCapture stderr_capture(STDERR_FILENO);

    std::string test_message = "This is a test debug message with value 42.";

    // Call the macro that prints the debug message to stderr
    // compute the line where PLH_DEBUG will be called
    int debug_call_line = __LINE__ + 1; // update the offset if you move the PLH_DEBUG line
    PLH_DEBUG("This is a test debug message with value {}. Called at {}", 42, PLH_LOC_HERE_STR);

    // Get the captured output
    std::string output = stderr_capture.GetOutput(); // Changed to use new StderrCapture

    // --- Helper that asserts a substring exists with a clear failure message ---
    auto expect_contains = [&](const std::string &needle)
    {
        auto pos = output.find(needle);
        EXPECT_NE(pos, std::string::npos)
            << "Expected output to contain: \n  " << needle << "\nActual output:\n"
            << output;
    };

    expect_contains(std::string("[DBG]  This is a test debug message with value 42."));
    expect_contains(std::string(pylabhub::format_tools::filename_only(__FILE__)));
    expect_contains(std::string(__func__));
    expect_contains(std::to_string(debug_call_line));

    // 3) message body: allow either the exact message or the message followed by a trailing newline
    bool found_body = (output.find(test_message) != std::string::npos) ||
                      (output.find(test_message + "\n") != std::string::npos);
    EXPECT_TRUE(found_body) << "Expected message body not found. Expected: \"" << test_message
                            << "\nActual output:\n"
                            << output;

    // Optional: (uncomment if you want to be strict about trailing newline)
    EXPECT_TRUE(output.size() >= 1 && output.back() == '\n')
        << "Expected a trailing newline in output.";
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
    EXPECT_DEATH(function_that_panics(),
                 AllOf(HasSubstr("This is a panic test."), HasSubstr("PANIC"),
                       HasSubstr("Stack Trace (most recent call first):")));
}

#include <limits>
#include <thread>

#ifdef PYLABHUB_SANITIZER_IS_THREAD
#include <gtest/gtest.h>
TEST(SanitizerCheckNoLifecycle, DetectsDataRace)
{
    auto data_race_func = []()
    {
        long shared_value = 0;
        std::thread t1(
            [&] () {
                for (int i = 0; i < 1000; ++i)
                    shared_value++;
            });
        std::thread t2(
            [&] () {
                for (int i = 0; i < 1000; ++i)
                    shared_value++;
            });
        t1.join();
        t2.join();
    };
    EXPECT_DEATH(data_race_func(), ".*ThreadSanitizer: data race.*\n");
}
#endif

#ifdef PYLABHUB_SANITIZER_IS_ADDRESS
#include <gtest/gtest.h>

// Test heap-buffer-overflow (write)
TEST(SanitizerCheckNoLifecycle, DetectsHeapBufferOverflowWrite)
{
    auto overflow_func = []()
    {
        int *array = new int[10];
        // Use volatile to prevent optimization
        *(volatile int *)&array[100] = 0;
    };
    EXPECT_DEATH(overflow_func(), ".*AddressSanitizer: heap-buffer-overflow.*\n");
}

// Test heap-buffer-overflow (read)
TEST(SanitizerCheckNoLifecycle, DetectsHeapBufferOverflowRead)
{
    auto overflow_func = []()
    {
        int *array = new int[10];
        int volatile x = array[100]; // Trigger overflow with a larger OOB access
        (void)x;                     // Avoid unused variable warning
        delete[] array;
    };
    EXPECT_DEATH(overflow_func(), ".*AddressSanitizer: heap-buffer-overflow.*\n");
}

// Test heap-use-after-free
TEST(SanitizerCheckNoLifecycle, DetectsHeapUseAfterFree)
{
    auto use_after_free_func = []()
    {
        int *array = new int[10];
        delete[] array;
        int volatile x = array[5]; // Trigger use-after-free
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

// Keep this noinline so it has a distinct stack frame
PYLABHUB_NOINLINE
static void trigger_stack_oob_small_write()
{
    // small buffer so compiler keeps it on the stack predictably
    fmt::print(stderr, "Before writing to stack buffer...\n");
    volatile char buf[256];
    // use the buffer to avoid it being optimized away
    buf[0] = 1;
    // write just one byte past the end (most likely to hit ASan's redzone)
    volatile char *p = buf;
    p[256] = 0;                    // 1 byte OOB (index == size)
    asm volatile("" ::: "memory"); // compiler barrier: avoid reordering/elision
    fmt::print(stderr, "After writing to stack buffer overflow...\n");
}

TEST(SanitizerCheckNoLifecycle, DetectsStackBufferOverflow)
{
    // run the noinline function and expect ASan stack-buffer-overflow
    EXPECT_DEATH(trigger_stack_oob_small_write(), ".*AddressSanitizer: stack-buffer-overflow.*\n");
}

#endif

#ifdef PYLABHUB_SANITIZER_IS_UNDEFINED
#include <gtest/gtest.h>
TEST(SanitizerCheckNoLifecycle, DetectsSignedIntegerOverflow)
{
    auto overflow_func = []()
    {
        // Use volatile to prevent the compiler from optimizing away the increment.
        volatile int value = std::numeric_limits<int>::max();
        value++;
    };
    EXPECT_DEATH(overflow_func(), ".*runtime error: signed integer overflow.*\n");
}
#endif

// --- Tests from test_debug_platform.cpp ---

TEST(DebugPlatformTest, SRCLOC_TO_STR) {
    std::source_location loc = std::source_location::current();
    std::string expected_filename(pylabhub::format_tools::filename_only(loc.file_name()));
    std::string result = SRCLOC_TO_STR(loc);

    // Expected format: filename:line:function_name
    EXPECT_THAT(result, StartsWith(expected_filename + ":"));
    EXPECT_THAT(result, HasSubstr(std::to_string(loc.line())));
    EXPECT_THAT(result, EndsWith(std::string(":") + loc.function_name()));
}

TEST(DebugPlatformTest, GetPID) {
    uint64_t pid = get_pid();
    // On most systems, PID is > 0.
    EXPECT_GT(pid, 0);
}

TEST(DebugPlatformTest, GetNativeThreadID) {
    uint64_t tid = get_native_thread_id();
    // On most systems, TID is > 0.
    EXPECT_GT(tid, 0);

    // Verify that different threads get different TIDs (or at least valid ones).
    std::thread t([&]() {
        uint64_t new_tid = get_native_thread_id();
        EXPECT_GT(new_tid, 0);
    });
    t.join();
}

TEST(DebugPlatformTest, GetExecutableName) {
    // Test with include_path = true
    std::string full_path = get_executable_name(true);
    EXPECT_FALSE(full_path.empty());
    // The name of the executable for these tests is 'test_pylabhub_corelib'
    EXPECT_THAT(full_path, HasSubstr("test_pylabhub_corelib"));

    // Test with include_path = false
    std::string filename_only = get_executable_name(false);
    EXPECT_FALSE(filename_only.empty());
#ifdef PYLABHUB_IS_WINDOWS
    EXPECT_EQ(filename_only, "test_pylabhub_corelib.exe");
#else
    EXPECT_EQ(filename_only, "test_pylabhub_corelib");
#endif

    // Test error path (difficult to simulate, relies on internal exception handling)
    // For now, we trust the internal `try-catch` in `get_executable_name`.
}

TEST(DebugPlatformTest, DebugMsgRtFormatError) {
    StringCapture capture(STDERR_FILENO); // Use the shared helper class

    // Trigger fmt::format_error by providing an incorrect number of arguments for a runtime format string.
    // debug_msg_rt expects (string_view fmt_str, const Args&... args)
    // If we pass an invalid format specifier, fmt::vprint will throw.
    std::string runtime_fmt = "Value: {} {}"; // Expects 2 args, but we'll pass 1.
    debug_msg_rt(runtime_fmt, 123);

    std::string output = capture.GetOutput();

    EXPECT_THAT(output, HasSubstr("FATAL FORMAT ERROR DURING DEBUG_MSG_RT"));
    EXPECT_THAT(output, HasSubstr("fmt_str['Value: {} {}']"));
    EXPECT_THAT(output, HasSubstr("Exception: 'argument not found'"));
}