/**
 * @file test_console_output_buffer.cpp
 * @brief L1 unit tests for pylabhub::utils::ConsoleOutputBuffer
 *        (HEP-CORE-0033 §11.0.1 layer 6 + §11.0.4).
 *
 * The operator console's single pull output buffer. The hub appends lines;
 * the operator drains them by polling (return-and-clear). These tests pin the
 * design contract, not incidental behaviour:
 *   - append/drain ordering + return-and-clear + explicit empty drain;
 *   - request_id preserved for command results, empty for volunteered lines;
 *   - content is always a JSON object — non-object / oversize → truncation
 *     marker so the line schema always holds;
 *   - the cap is a safety valve: overflow (max_lines / max_bytes) drops the
 *     OLDEST lines to the log sink and reports dropped_count, which resets on
 *     drain (overflow is reported, never silent);
 *   - lifecycle: on_connect starts clean, on_disconnect flushes a non-empty
 *     buffer to the log and clears.
 *
 * The clock and log sink are injected so the buffer is testable without the
 * real hub clock or logger (mirrors test_replay_guard.cpp).
 */

#include "utils/console_output_buffer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using pylabhub::utils::ConsoleOutputBuffer;
using nlohmann::json;

namespace
{

/// Captures every line the buffer spills, with the reason, for assertions.
struct LogCapture
{
    struct Spilled
    {
        ConsoleOutputBuffer::Line line;
        std::string reason;
    };
    std::vector<Spilled> spilled;

    ConsoleOutputBuffer::LogSinkFn sink()
    {
        return [this](const ConsoleOutputBuffer::Line &l, const char *reason)
        { spilled.push_back({l, reason}); };
    }
};

/// A message line — content is a JSON object per the contract.
json msg(const std::string &text)
{
    return json{{"message", text}};
}

}  // namespace

TEST(ConsoleOutputBufferTest, AppendDrainOldestFirstThenClears)
{
    ConsoleOutputBuffer buf;
    buf.append("", msg("one"));
    buf.append("", msg("two"));
    buf.append("", msg("three"));

    auto d = buf.drain();
    ASSERT_EQ(d.lines.size(), 3u);
    EXPECT_EQ(d.lines[0].content.at("message"), "one");
    EXPECT_EQ(d.lines[1].content.at("message"), "two");
    EXPECT_EQ(d.lines[2].content.at("message"), "three");
    EXPECT_EQ(d.dropped_count, 0u);

    // return-and-clear: a second drain is empty.
    auto d2 = buf.drain();
    EXPECT_TRUE(d2.lines.empty());
    EXPECT_EQ(d2.dropped_count, 0u);
    EXPECT_EQ(buf.size(), 0u);
}

TEST(ConsoleOutputBufferTest, DrainEmptyBufferIsExplicitlyEmpty)
{
    ConsoleOutputBuffer buf;
    auto d = buf.drain();
    EXPECT_TRUE(d.lines.empty());
    EXPECT_EQ(d.dropped_count, 0u);
}

TEST(ConsoleOutputBufferTest, RequestIdPreservedForResultEmptyForVolunteered)
{
    ConsoleOutputBuffer buf;
    buf.append("req-42", json{{"status", "closed"}});  // a late command result
    buf.append("", msg("hub says hi"));                // volunteered

    auto d = buf.drain();
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_EQ(d.lines[0].request_id, "req-42");
    EXPECT_TRUE(d.lines[1].request_id.empty());
}

TEST(ConsoleOutputBufferTest, NonObjectContentBecomesTruncationMarker)
{
    ConsoleOutputBuffer buf;
    buf.append("", json("a bare string"));  // NOT an object
    buf.append("", json(42));               // NOT an object

    auto d = buf.drain();
    ASSERT_EQ(d.lines.size(), 2u);
    for (const auto &l : d.lines)
    {
        ASSERT_TRUE(l.content.is_object());
        EXPECT_EQ(l.content.at("truncated"), true);
        EXPECT_EQ(l.content.at("reason"), "not_an_object");
    }
}

TEST(ConsoleOutputBufferTest, OversizeLineTruncatedWithMarker)
{
    ConsoleOutputBuffer::Caps caps;
    caps.max_line_bytes = 128;  // tiny per-line cap
    ConsoleOutputBuffer buf(caps);

    buf.append("", json{{"message", std::string(4096, 'x')}});  // way over 128 B

    auto d = buf.drain();
    ASSERT_EQ(d.lines.size(), 1u);
    ASSERT_TRUE(d.lines[0].content.is_object());
    EXPECT_EQ(d.lines[0].content.at("truncated"), true);
    EXPECT_EQ(d.lines[0].content.at("reason"), "line_too_large");
    EXPECT_GT(d.lines[0].content.at("bytes").get<std::size_t>(), 128u);
}

TEST(ConsoleOutputBufferTest, OverflowByLinesDropsOldestToLogAndCounts)
{
    ConsoleOutputBuffer::Caps caps;
    caps.max_lines = 2;
    LogCapture log;
    ConsoleOutputBuffer buf(caps, {}, log.sink());

    buf.append("", msg("l1"));
    buf.append("", msg("l2"));
    buf.append("", msg("l3"));  // evicts l1
    buf.append("", msg("l4"));  // evicts l2

    // The two oldest were spilled to the log, oldest-first.
    ASSERT_EQ(log.spilled.size(), 2u);
    EXPECT_EQ(log.spilled[0].line.content.at("message"), "l1");
    EXPECT_EQ(log.spilled[0].reason, std::string("overflow"));
    EXPECT_EQ(log.spilled[1].line.content.at("message"), "l2");

    // The buffer holds only the newest two, and reports the drop count.
    auto d = buf.drain();
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_EQ(d.lines[0].content.at("message"), "l3");
    EXPECT_EQ(d.lines[1].content.at("message"), "l4");
    EXPECT_EQ(d.dropped_count, 2u);
}

TEST(ConsoleOutputBufferTest, DroppedCountResetsAfterDrain)
{
    ConsoleOutputBuffer::Caps caps;
    caps.max_lines = 1;
    ConsoleOutputBuffer buf(caps);

    buf.append("", msg("a"));
    buf.append("", msg("b"));  // drops a
    EXPECT_EQ(buf.drain().dropped_count, 1u);

    // Next window starts fresh.
    buf.append("", msg("c"));
    EXPECT_EQ(buf.drain().dropped_count, 0u);
}

TEST(ConsoleOutputBufferTest, OnConnectClearsBuffer)
{
    ConsoleOutputBuffer buf;
    buf.append("", msg("stale"));
    buf.on_connect();
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_TRUE(buf.drain().lines.empty());
}

TEST(ConsoleOutputBufferTest, OnDisconnectFlushesNonEmptyToLogAndClears)
{
    LogCapture log;
    ConsoleOutputBuffer buf({}, {}, log.sink());
    buf.append("", msg("undrained-1"));
    buf.append("", msg("undrained-2"));

    buf.on_disconnect();

    ASSERT_EQ(log.spilled.size(), 2u);
    EXPECT_EQ(log.spilled[0].reason, std::string("disconnect_flush"));
    EXPECT_EQ(log.spilled[0].line.content.at("message"), "undrained-1");
    EXPECT_EQ(buf.size(), 0u);
}

TEST(ConsoleOutputBufferTest, OnDisconnectEmptyBufferDoesNotLog)
{
    LogCapture log;
    ConsoleOutputBuffer buf({}, {}, log.sink());
    buf.on_disconnect();
    EXPECT_TRUE(log.spilled.empty());
}

TEST(ConsoleOutputBufferTest, TimestampComesFromInjectedClock)
{
    std::uint64_t fake = 1000;
    ConsoleOutputBuffer buf({}, [&fake] { return fake; });
    buf.append("", msg("a"));
    fake = 2000;
    buf.append("", msg("b"));

    auto d = buf.drain();
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_EQ(d.lines[0].ts_ms, 1000u);
    EXPECT_EQ(d.lines[1].ts_ms, 2000u);
}
