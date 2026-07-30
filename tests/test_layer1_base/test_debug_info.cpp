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

#include <algorithm>
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

// ============================================================================
// Last-resort trace (HEP-CORE-0048)
// ============================================================================
//
// These pin the CONTRACT the design rests on, not the current output shape.
// Everything the rest of the system assumes — that finalize(), the SIGTERM
// watcher and panic() can each call trace_print() with no coordination — is a
// consequence of print draining, so that is what gets asserted directly.
//
// `trace_print` writes with `write(2)` to fd 2, bypassing stdio, so capturing
// it means redirecting the descriptor rather than swapping a stream buffer.

#if defined(PYLABHUB_IS_POSIX) && (PYLABHUB_IS_POSIX)

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace
{

/// Redirects fd 2 to a temporary file for the duration of a scope.
class StderrCapture
{
  public:
    StderrCapture()
    {
        path_ = std::filesystem::temp_directory_path() /
                ("plh_trace_capture_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++) + ".txt");
        saved_fd_ = ::dup(STDERR_FILENO);
        file_fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        ::dup2(file_fd_, STDERR_FILENO);
    }

    ~StderrCapture()
    {
        if (saved_fd_ >= 0)
        {
            ::dup2(saved_fd_, STDERR_FILENO);
            ::close(saved_fd_);
        }
        if (file_fd_ >= 0)
            ::close(file_fd_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    StderrCapture(const StderrCapture &) = delete;
    StderrCapture &operator=(const StderrCapture &) = delete;

    /// Everything written to fd 2 since construction.
    [[nodiscard]] std::string text() const
    {
        std::ifstream in(path_, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }

  private:
    std::filesystem::path path_;
    int saved_fd_{-1};
    int file_fd_{-1};
    static inline int counter_{0};
};

} // namespace

TEST(DebugTraceTest, PrintDrains_SecondPrintEmitsNothing)
{
    // THE load-bearing property.  finalize(), the SIGTERM watcher and panic()
    // all call trace_print() without knowing about each other; that is only
    // safe because printing empties the buffer.  If this regresses, an
    // abnormal exit prints the same teardown twice and a reader cannot tell
    // which copy is current.
    trace_clear();
    // Printing is gated on the dirty latch (silent for a clean report in a
    // release build), so establish that precondition explicitly. Without it
    // this test would pass in Debug and fail in Release for a reason that has
    // nothing to do with draining.
    trace_mark_dirty();
    trace_add("event=Step step='one'");

    std::string first;
    {
        StderrCapture cap;
        trace_print();
        first = cap.text();
    }
    EXPECT_NE(first.find("step='one'"), std::string::npos)
        << "first print must emit what was added";

    std::string second;
    {
        StderrCapture cap;
        trace_print();
        second = cap.text();
    }
    EXPECT_TRUE(second.empty())
        << "print must drain; a second print had nothing to say but wrote: " << second;
}

TEST(DebugTraceTest, EmptyBufferPrintsNothing)
{
    // Why callers never have to ask whether calling is worthwhile — which is
    // what lets finalize() call it unconditionally on a clean run.
    trace_clear();
    trace_mark_dirty(); // so emptiness is the reason for silence, not the gate
    StderrCapture cap;
    trace_print();
    EXPECT_TRUE(cap.text().empty());
}

TEST(DebugTraceTest, ClearDiscardsWithoutPrinting)
{
    // lifecycle's clean-teardown path depends on this: clear, then print
    // unconditionally, and nothing reaches the operator.
    trace_clear();
    trace_mark_dirty(); // so the CLEAR is the reason for silence, not the gate
    trace_add("event=Step step='discarded'");
    trace_clear();

    StderrCapture cap;
    trace_print();
    EXPECT_TRUE(cap.text().empty())
        << "cleared content must not resurface on the next print";
}

TEST(DebugTraceTest, EntryCarriesThreadIdTimestampAndNewline)
{
    // The envelope the API promises to enforce.  A caller supplies content
    // only; the framework stamps the two things a caller cannot cheaply know
    // and guarantees the separator, so interleaved writers stay readable.
    trace_clear();
    trace_mark_dirty();
    trace_add("event=Step step='envelope'"); // deliberately no trailing newline

    StderrCapture cap;
    trace_print();
    const std::string out = cap.text();

    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front(), '[') << "entry should open with the [tid|ts] stamp: " << out;
    EXPECT_NE(out.find("us] "), std::string::npos)
        << "monotonic microsecond stamp missing: " << out;
    EXPECT_NE(out.find(std::to_string(pylabhub::platform::get_native_thread_id())),
              std::string::npos)
        << "writing thread's id missing: " << out;
    EXPECT_EQ(out.back(), '\n')
        << "newline must be appended even when the caller omits it, or two "
           "entries run together into one unparseable line";
}

TEST(DebugTraceTest, TwoEntriesStayOnSeparateLines)
{
    trace_clear();
    trace_mark_dirty();
    trace_add("event=Step step='first'");
    trace_add("event=Step step='second'");

    StderrCapture cap;
    trace_print();
    const std::string out = cap.text();

    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 2)
        << "one newline per entry, no more and no fewer: " << out;
    EXPECT_LT(out.find("step='first'"), out.find("step='second'"))
        << "entries must appear in the order they were added";
}

TEST(DebugTraceTest, OverflowIsReportedRatherThanSilentlyDropped)
{
    // Truncation that does not announce itself is worse than truncation: a
    // short trace reads as "that is all that happened".
    trace_clear();
    trace_mark_dirty();
    const std::string chunk(kTraceEntryBytes / 2, 'x');
    for (std::size_t written = 0; written <= kTraceBytes; written += chunk.size())
        trace_add(chunk);

    StderrCapture cap;
    trace_print();
    const std::string out = cap.text();

    EXPECT_NE(out.find("[trace] dropped="), std::string::npos)
        << "an overflowing buffer must say so on print";
    EXPECT_LE(out.size(), kTraceBytes + 128u)
        << "output must stay bounded by the buffer plus the dropped-bytes note";
}

TEST(DebugTraceTest, OversizedEntryIsTruncatedNotRejected)
{
    // A caller that writes too much loses the tail of THAT entry; it does not
    // lose the entry, and it does not corrupt the ones around it.
    trace_clear();
    trace_mark_dirty();
    trace_add(std::string(kTraceEntryBytes * 4, 'y'));
    trace_add("event=Step step='after'");

    StderrCapture cap;
    trace_print();
    const std::string out = cap.text();

    EXPECT_NE(out.find("step='after'"), std::string::npos)
        << "a following entry must be unaffected by an oversized predecessor";
    EXPECT_NE(out.find("[trace] dropped="), std::string::npos)
        << "bytes lost to entry truncation count the same as pool overflow";
}

#endif // POSIX

// ── dirty latch ─────────────────────────────────────────────────────────────
//
// The latch is the emission policy, so these pin the property that makes it
// trustworthy: it only ever goes one way. A test that merely checked
// "mark_dirty then is_dirty" would pass on an implementation that also let
// something clear it.

TEST(DebugTraceTest, DirtyLatchIsSetByAnyoneAndClearedByNothing)
{
    trace_mark_dirty();
    ASSERT_TRUE(trace_is_dirty());

    // trace_clear() discards CONTENT.  It must not be able to launder a
    // process back to "healthy" — otherwise a subsystem that tore down
    // cleanly after someone else's failure could erase that failure.
    trace_clear();
    EXPECT_TRUE(trace_is_dirty())
        << "trace_clear() must not reset the dirty latch";

    // Repeated marking by multiple reporters is a no-op, not an error.
    trace_mark_dirty();
    trace_mark_dirty();
    EXPECT_TRUE(trace_is_dirty());

    // And printing does not launder it either.
    {
        StderrCapture cap;
        trace_print();
    }
    EXPECT_TRUE(trace_is_dirty()) << "trace_print() must not reset the dirty latch";
}

TEST(DebugTraceTest, DirtyReportPrintsInEveryBuild)
{
    // The case the facility exists for: whatever the build, an accident
    // speaks.  In a release build a CLEAN report is silent, so without this
    // the one report that matters could be suppressed by the same rule.
    trace_clear();
    trace_mark_dirty();
    trace_add("event=Step step='accident'");

    StderrCapture cap;
    trace_print();
    EXPECT_NE(cap.text().find("step='accident'"), std::string::npos)
        << "a dirty report must never be suppressed";
}
