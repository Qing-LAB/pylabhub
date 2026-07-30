#pragma once
/**
 * @file debug_info.hpp
 * @brief Provides cross-platform debugging utilities, including stack trace printing,
 *        panic handling for fatal errors, and debug messaging.
 *
 * This header defines a set of functions and macros within the `pylabhub::debug`
 * namespace designed for robust error reporting and debugging. It leverages `fmt`
 * for compile-time format string checks and `std::source_location` for automatic
 * source code location reporting.
 */
#include "pylabhub_utils_export.h"
#include "utils/format_tools.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fmt/format.h>
#include <source_location>
#include <string>
#include <string_view>

// ---------------- thin macros for convenience --------------
/**
 * @brief Macro to capture the current source location.
 * @details Expands to `std::source_location::current()`.
 */
inline std::string SRCLOC_TO_STR(std::source_location loc)
{
    return fmt::format("{}:{}:{}", pylabhub::format_tools::filename_only(loc.file_name()),
                       loc.line(), loc.function_name());
}

#ifndef PLH_LOC_HERE_STR
#define PLH_LOC_HERE_STR (SRCLOC_TO_STR(std::source_location::current()))
#endif

namespace pylabhub::debug
{

// ─────────────────────── last-resort trace buffer ───────────────────────
//
// A single process-global buffer that holds the last critical steps a
// process took before it died.
//
// **This is not a log.**  The logger is asynchronous: `LOGGER_*` puts a
// message on a queue that a worker thread drains later.  That is the right
// design for logging and the wrong one for forensics, because a process
// that wedges or is killed never drains the queue — so the messages that
// would have explained the failure die with it.  This buffer is the
// opposite trade: a synchronous memcpy into fixed storage, no queue, no
// worker, nothing to flush.  It survives because there is nothing left to
// go wrong between writing and reading.
//
// The rule for choosing between them:
//
//     Would I still need this if the process never finished shutting down?
//       yes -> trace_add()      no -> LOGGER_*
//
// So it holds step markers on shutdown / exit / panic paths, and nothing
// else.  Filling it with ordinary progress messages destroys the one
// property it exists for: that what is in it is what mattered.
//
// **Lifetime.**  The storage is a file-scope array with static storage
// duration and no destructor, deliberately.  A shutdown worker abandoned
// at its deadline may still be running — and still reporting — after the
// code that started it gave up, so anything that could be destroyed on
// schedule would be written to after death.
//
// **Concurrency.**  Lock-free.  Writers reserve a byte range with an
// atomic compare-exchange and then fill only their own slice.  A lock
// would be simpler to write and wrong to use: a worker wedged or killed
// mid-append would hold it forever, and every later reader — including
// the one at the end of teardown that exists to explain the wedge — would
// block on the corpse or need a bounded-retry hack to give up.
//
// See HEP-CORE-0048.

/// Capacity of the trace buffer in bytes.  Build option
/// `PLH_DEBUG_TRACE_BYTES` — see cmake/ToplevelOptions.cmake for how the
/// default was sized against the write volume of one full teardown.
#ifndef PLH_DEBUG_TRACE_BYTES
#define PLH_DEBUG_TRACE_BYTES 16384
#endif
inline constexpr std::size_t kTraceBytes = PLH_DEBUG_TRACE_BYTES;

/// Maximum size of ONE entry, stamp included.  Entries are formatted on
/// the caller's stack before being copied in, so this bounds a stack
/// buffer, not the pool.  Anything longer is truncated and the lost bytes
/// are counted the same as pool overflow.
inline constexpr std::size_t kTraceEntryBytes = 256;

/// Append one entry.  Stamps thread id and a monotonic microsecond
/// timestamp, and guarantees the entry ends in '\n' so a caller who
/// forgets cannot run two records into one unreadable line.
///
/// WHAT the step refers to — module, phase, task, request id — is the
/// caller's to say, in `event=<Verb> key='value'` form
/// (docs/IMPLEMENTATION_GUIDANCE.md).  The framework stamps only what the
/// caller cannot cheaply know and what is needed to demultiplex
/// concurrent writers; it must not guess at identity it does not own.
///
/// Never allocates, never throws, never blocks.  Bytes that do not fit —
/// whether because the entry is too long or the pool is full — are
/// counted and reported by the next `trace_print()`.
PYLABHUB_UTILS_EXPORT void trace_add(std::string_view msg) noexcept;

/// Write everything accumulated to stderr, then reset the buffer.
///
/// Printing drains, and that is what lets every printer compose with no
/// coordination at all: whoever gets there first empties it, and anyone
/// arriving later prints only what accumulated since. No ownership, no
/// duplicate output.
///
/// Drains by FREEZING rather than by zeroing: one atomic exchange takes the
/// current length and parks the counter where no append can fit, so for the
/// duration of the write every `trace_add` takes its normal drop path. The
/// bytes being emitted cannot be overwritten, and the refused entries are
/// counted rather than vanishing. Concurrent printers are handled by the
/// same mechanism — a second caller sees the freeze sentinel and returns
/// rather than emitting the buffer twice.
///
/// **Whether to print is decided HERE, from the dirty latch** — not by the
/// caller, and not with a `trace_clear()` beforehand:
///
///   - dirty (`trace_mark_dirty` was called) — prints, in EVERY build. This
///     is the case the facility exists for and it is never suppressed.
///   - clean, Debug build — prints. A developer wants to see what a normal
///     exit looks like; that is what makes an abnormal one recognisable.
///   - clean, Release build — prints NOTHING and leaves the buffer intact.
///     An operator running a CLI command should not get an exit dump every
///     time. The content is left in place rather than dropped, so if
///     something later marks dirty its report carries this context too.
///
/// A caller that wants to force emission marks dirty first — which is what
/// `panic()` and the fatal-signal path do, since arriving there is itself
/// the thing that went wrong.
///
/// Uses `write(2)` rather than stdio, which takes a lock and may allocate.
/// Emits nothing when the buffer is empty, so callers never need to ask
/// whether it is worth calling.
PYLABHUB_UTILS_EXPORT void trace_print() noexcept;

/// Mark this process's report as DIRTY — something went wrong.
///
/// A one-way latch. **Any** number of reporters may set it; **nothing**
/// clears it, not even `trace_clear()`. That asymmetry is the design: the
/// accident already happened, and no later phase completing normally makes
/// it un-happen. A clearable flag would let a subsystem that tore down
/// cleanly after someone else's failure erase the record of that failure —
/// exactly the loss this buffer exists to prevent.
///
/// Owned here, not by any client. This module depends on nothing and is
/// usable on its own, so a program that never touches lifecycle can still
/// say "my exit was not clean" and have its report survive. Putting the
/// flag in a client would deny that to every other user and tie a general
/// facility to one caller.
PYLABHUB_UTILS_EXPORT void trace_mark_dirty() noexcept;

/// Has anything marked this process's report dirty?
///
/// Provided for diagnostics and tests. Callers do NOT need to consult this
/// before printing — `trace_print()` applies it internally, which is what
/// keeps the policy in one place instead of at every print site.
[[nodiscard]] PYLABHUB_UTILS_EXPORT bool trace_is_dirty() noexcept;

/// Write `n` bytes to stderr with `write(2)`, retrying partial writes.
///
/// Exposed because `panic()` must emit its own message through the same
/// door the trace uses: stdio takes a lock and may allocate, and on a
/// panic path either can be the reason we are here.  Not for general use
/// — ordinary output belongs to the logger.
PYLABHUB_UTILS_EXPORT void trace_write_stderr(const char *data, std::size_t n) noexcept;

/// Discard the accumulated entries and the dropped-byte count without
/// printing.
///
/// Does **not** reset the dirty latch — see `trace_mark_dirty`. Clearing
/// content is a caller saying "these particular steps were unremarkable";
/// it is not a claim that the process is healthy, and it must never be able
/// to become one.
///
/// `trace_print()` does NOT call this. It drains by parking the length at a
/// freeze sentinel for the duration of the write and then resetting it,
/// which lets it subtract exactly the dropped bytes it reported instead of
/// zeroing a counter that may have grown while it was still emitting.
///
/// This is here for a caller that deliberately wants to discard without
/// emitting — legitimate, but such a caller had better know what it is
/// throwing away, including any drop count it has not reported.
PYLABHUB_UTILS_EXPORT void trace_clear() noexcept;

/**
 * @brief Prints the current call stack (stack trace) to `stderr`.
 *
 * This function provides a platform-specific implementation to capture and print
 * the program's call stack. On Windows, it uses `CaptureStackBackTrace` and `DbgHelp`.
 * On POSIX systems, it uses `backtrace`, `dladdr`, and optionally external tools
 * like `addr2line` or `atos` for detailed symbol resolution.
 *
 * Errors during stack trace capture are reported to `stderr`.
 *
 * @param use_external_tools On POSIX, if `true`, the function will also attempt
 *        to call external tools (`addr2line`, `atos`) for more detailed symbols.
 *        This carries a minor risk of hanging in some contexts. This parameter has
 *        no effect on Windows, which always uses in-process `DbgHelp` APIs.
 *
 * @warning **Not Async-Signal-Safe**: This function must NOT be called from a
 *          signal handler. It allocates memory (`new`, `std::string`), performs
 *          I/O (`popen`, `fmt::print`), and calls other non-reentrant functions,
 *          which can lead to deadlocks or crashes if used in a signal handler.
 * @warning **Not Thread-Safe**: This function is not guaranteed to be safe for
 *          concurrent calls from multiple threads. Prefer calling it from a single
 *          thread or from crash handlers where concurrency is controlled.
 */
PYLABHUB_UTILS_EXPORT void print_stack_trace(bool use_external_tools = false) noexcept;

/**
 * @brief Halts program execution with a fatal error message and prints a stack trace.
 *
 * This function is intended for unrecoverable errors. It formats and prints an
 * error message to `stderr`, along with the source location where `panic` was called.
 * It then calls `print_stack_trace(true)` for a detailed trace and `std::abort()`.
 *
 * The format string is checked at compile-time using `fmt::format_string`.
 * Exception handling is included to prevent further issues if formatting itself fails.
 *
 * @tparam Args Variadic template arguments for the format string.
 * @param loc The source location (file, line, function) where `panic` was called.
 *            Automatically captured by `PLH_HERE` or `std::source_location::current()`.
 * @param fmt_str The `fmt`-style format string for the error message.
 * @param args The arguments to be formatted into `fmt_str`.
 * @noreturn This function never returns.
 */
template <typename... Args>
[[noreturn]] inline void panic(std::source_location loc, fmt::format_string<Args...> fmt_str,
                               Args &&...args) noexcept
{
    // Formatted into a fixed stack buffer, never into a std::string.
    // `fmt::format` returns a heap string, so the previous shape asked the
    // allocator for memory as the FIRST act of dying — and one common
    // reason to be here is that the heap is exhausted or corrupt.  The old
    // code caught the resulting `bad_alloc`, but its fallback formatted
    // too, so a real out-of-memory panic lost its message entirely and
    // aborted without ever saying why.
    {
        char msg[kTraceEntryBytes * 4];
        std::size_t n = 0;
        auto room = [&]() { return sizeof(msg) - n; };
        auto advance = [&](std::size_t written) { n += written < room() ? written : room(); };
        try
        {
            // The location is formatted field-by-field rather than through
            // SRCLOC_TO_STR, which returns a std::string and would put an
            // allocation right back on this path.
            advance(fmt::format_to_n(msg + n, room(), "[PANIC] {}:{}:{} -- ",
                                     pylabhub::format_tools::filename_only(loc.file_name()),
                                     loc.line(), loc.function_name())
                        .size);
            // The caller's format string writes straight into the tail of
            // the same buffer — no intermediate string anywhere.
            advance(fmt::format_to_n(msg + n, room(), fmt_str, std::forward<Args>(args)...).size);
            advance(fmt::format_to_n(msg + n, room(), "\n").size);
        }
        catch (...)
        {
            n = 0;
            advance(fmt::format_to_n(msg + n, room(),
                                     "[PANIC] {}:{} -- FORMATTING THE PANIC MESSAGE FAILED\n",
                                     pylabhub::format_tools::filename_only(loc.file_name()),
                                     loc.line())
                        .size);
        }
        trace_write_stderr(msg, n);
    }

    // A panic is by definition not a clean exit, so mark the report dirty
    // before printing rather than teaching trace_print() about special
    // callers.  One rule, applied by whoever knows they are in trouble.
    trace_mark_dirty();

    // Order matters: message, then the trace, then the backtrace.  The
    // trace is what the process was DOING; the backtrace is where it
    // stopped.  Reading them the other way round means holding the stack
    // in your head with no idea what it was in the middle of.
    //
    // An empty buffer emits nothing, and because trace_print() drains, a
    // shutdown that already printed leaves this silent rather than repeating
    // itself.
    trace_print();

    print_stack_trace(true); // Go for max detail on panic, accepting the risks.
    std::abort();
}

/**
 * @brief Prints a debug message to `stderr` with compile-time format string checking.
 *
 * This function formats and prints a debug message to `stderr`, including the
 * source location where `debug_msg` was called. It uses `fmt` for efficient and
 * type-safe formatting, with the format string checked at compile-time.
 *
 * This function is intended for general debugging output that can be easily
 * enabled/disabled or filtered.
 *
 * @tparam Args Variadic template arguments for the format string.
 * @param fmt_str The `fmt`-style format string for the debug message.
 * @param args The arguments to be formatted into `fmt_str`.
 */
template <typename... Args>
inline void debug_msg(fmt::format_string<Args...> fmt_str, Args &&...args) noexcept
{
    try
    {
        const auto body = fmt::format(fmt_str, std::forward<Args>(args)...);
        fmt::print(stderr, "[DBG]  {}\n", body);
    }
    catch (const fmt::format_error &e)
    {
        fmt::print(stderr,
                   "[DBG]  FATAL FORMAT ERROR DURING DEBUG_MSG: fmt_str['{}']\n"
                   "[DBG]  Exception: '{}'\n",
                   fmt_str.get(), e.what());
        std::fflush(stderr);
    }
    catch (...)
    {
        fmt::print(stderr, "[DBG]  FATAL EXCEPTION DURING DEBUG_MSG: fmt_str['{}']\n",
                   fmt_str.get());
        std::fflush(stderr);
    }
}

// debug_msg_rt: runtime format string, take args by const& so make_format_args binds
/**
 * @brief Prints a debug message to `stderr` using a runtime-determined format string.
 *
 * This function provides similar functionality to `debug_msg` but accepts a `std::string_view`
 * for the format string, allowing the format to be determined at runtime.
 * This comes at the cost of compile-time format string validation.
 *
 * The arguments are passed by `const&` to ensure `fmt::make_format_args` can bind
 * them correctly.
 *
 * @tparam Args Variadic template arguments for the format string.
 * @param fmt_str A `std::string_view` representing the `fmt`-style format string.
 * @param args The arguments to be formatted into `fmt_str`.
 */
template <typename... Args>
inline void debug_msg_rt(std::string_view fmt_str, const Args &...args) noexcept
{
    try
    {
        fmt::print(stderr, "[DBG]  ");
        fmt::vprint(stderr, fmt_str, fmt::make_format_args(args...));
        fmt::print(stderr, "\n");
    }
    catch (const fmt::format_error &e)
    {
        fmt::print(stderr,
                   "[DBG]  FATAL FORMAT ERROR DURING DEBUG_MSG_RT: fmt_str['{}']\n"
                   "[DBG]  Exception: '{}'\n",
                   fmt_str, e.what());
        std::fflush(stderr);
    }
    catch (...)
    {
        fmt::print(stderr, "[DBG]  FATAL UNKNOWN EXCEPTION DURING DEBUG_MSG_RT: fmt_str['{}']\n",
                   fmt_str);
        std::fflush(stderr);
    }
}

} // namespace pylabhub::debug

/**
 * @brief Macro for calling `pylabhub::debug::panic` with automatic source location.
 * @details This macro provides a convenient way to trigger a fatal error with
 *          a compile-time checked format string. It will record the place where the macro
 *          is called.
 * @param ... Variable arguments to be formatted into `fmt`.
 * @see pylabhub::debug::panic
 */
#ifndef PLH_PANIC
#define PLH_PANIC(fmt, ...)                                                                        \
    ::pylabhub::debug::panic(std::source_location::current(),                                      \
                             FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#endif

/**
 * @brief Macro for calling `pylabhub::debug::debug_msg` with automatic source location.
 * @details This macro provides a convenient way to print debug messages with
 *          a compile-time checked format string.
 * @param fmt The `fmt`-style format string literal.
 * @param ... Variable arguments to be formatted into `fmt`.
 * @see pylabhub::debug::debug_msg
 */
#ifndef PLH_DEBUG
#if defined(PYLABHUB_ENABLE_DEBUG_MESSAGES)
#define PLH_DEBUG(fmt, ...) ::pylabhub::debug::debug_msg(FMT_STRING(fmt) __VA_OPT__(, ) __VA_ARGS__)
#else
#define PLH_DEBUG(fmt, ...)                                                                        \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#endif
#endif

/**
 * @brief Macro for calling `pylabhub::debug::debug_msg_rt` with automatic source location.
 * @details This macro provides a convenient way to print debug messages where the
 *          format string is determined at runtime.
 * @param fmt The runtime format string (e.g., `std::string_view` or `const char*`).
 * @param ... Variable arguments to be formatted into `fmt`.
 * @see pylabhub::debug::debug_msg_rt
 */
#ifndef PLH_DEBUG_RT
#if defined(PYLABHUB_ENABLE_DEBUG_MESSAGES)
#define PLH_DEBUG_RT(fmt, ...) ::pylabhub::debug::debug_msg_rt(fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define PLH_DEBUG_RT(fmt, ...)                                                                     \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#endif
#endif