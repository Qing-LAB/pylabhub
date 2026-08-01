/**
 * @file test_zmq_wire_frame.cpp
 * @brief Pattern 1 (PureApiTest) — the 5-tuple wire frame codec.
 *
 * `zmq_wire_helpers.hpp` calls itself "the single source of truth for the
 * 5-tuple frame" and is shared by ZmqQueue (HEP-CORE-0021 §13) and
 * InboxQueue (HEP-CORE-0027 §3).  Until review S-7 it had no direct test:
 * everything about it was exercised only indirectly, through live sockets.
 *
 * The codec is a pure function over bytes — no LOGGER_*, no lifecycle
 * module, no filesystem — so it is Pattern 1 per README_testing.md
 * § "Choosing a test pattern".
 *
 * These tests pin the DECODE BOUNDS, which is where S-7 lived.  A test that
 * only asserted "a malformed frame is rejected" would pass with or without
 * the fix, because an unbounded decode also rejects the frame — after it has
 * already tried the allocation.  So the assertions below are about *which*
 * frames are refused, not merely that bad ones are.
 */
// Included on its own, deliberately: this test is also the check that
// `zmq_wire_helpers.hpp` is self-contained.  It was not — it named
// `ZmqSchemaField` (which lives in `hub_zmq_queue.hpp`) for a
// `compute_field_layout` overload that reinterpret_cast a type to itself,
// since `ZmqSchemaField` IS `SchemaFieldDesc`.  That overload is gone; if
// anyone reintroduces a dependency on include order, this file stops
// compiling.
#include "hub/zmq_wire_helpers.hpp"

#include <gtest/gtest.h>

#include <msgpack.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

namespace wd = pylabhub::hub::wire_detail;

/// Pack a well-formed 5-tuple whose payload array has `payload_fields`
/// elements, each a uint32.  `payload_fields` is a parameter so a test can
/// ask for one more than the decoder is told to expect.
msgpack::sbuffer pack_frame_with_payload_count(std::uint32_t payload_fields,
                                               std::uint64_t seq = 7)
{
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    const std::array<std::uint8_t, 8> tag{1, 2, 3, 4, 5, 6, 7, 8};
    std::uint8_t checksum[32]{};

    pk.pack_array(5);
    pk.pack(wd::kFrameMagic);
    pk.pack_bin(8);
    pk.pack_bin_body(reinterpret_cast<const char *>(tag.data()), 8);
    pk.pack(seq);
    pk.pack_array(payload_fields);
    for (std::uint32_t i = 0; i < payload_fields; ++i)
        pk.pack(static_cast<std::uint32_t>(i));
    pk.pack_bin(32);
    pk.pack_bin_body(reinterpret_cast<const char *>(checksum), 32);
    return buf;
}

} // namespace

// ── Baseline: the decoder still accepts what the encoder produces ──────────

TEST(ZmqWireFrameTest, WellFormedFrameDecodes)
{
    const auto buf = pack_frame_with_payload_count(4);
    auto frame = wd::decode_frame(buf.data(), buf.size(), /*max_payload_fields=*/4);

    ASSERT_TRUE(static_cast<bool>(frame)) << "a frame the packer just produced must decode";
    EXPECT_TRUE(frame.env.valid);
    EXPECT_EQ(frame.env.seq, 7u);
    EXPECT_EQ(frame.env.payload_size, 4u);
    ASSERT_NE(frame.env.recv_tag, nullptr);
    EXPECT_EQ(frame.env.recv_tag[0], 1);
    EXPECT_EQ(frame.env.recv_tag[7], 8);
    ASSERT_NE(frame.env.checksum, nullptr);
}

// ── S-7: the array bound is wired to max_payload_fields ───────────────────
//
// This is the assertion that distinguishes fixed from unfixed.  With the
// bound in place the decode is refused outright.  WITHOUT it the frame
// parses happily and comes back valid with payload_size == 5 — the caller's
// own `payload_size != schema_defs_.size()` check would then reject it, but
// only AFTER msgpack allocated for every element the frame declared.

TEST(ZmqWireFrameTest, RejectsPayloadArrayLargerThanTheSchemaAllows)
{
    const auto buf = pack_frame_with_payload_count(9);
    auto frame = wd::decode_frame(buf.data(), buf.size(), /*max_payload_fields=*/8);

    EXPECT_FALSE(static_cast<bool>(frame))
        << "a payload array of 9 must be refused when the schema has 8 fields — "
           "the limit is what prevents the allocation, so rejection happens in "
           "the decoder rather than downstream";
    EXPECT_FALSE(frame.env.valid);
}

TEST(ZmqWireFrameTest, AcceptsPayloadArrayExactlyAtTheLimit)
{
    // Boundary the other way: the bound must not be off by one and reject a
    // frame the schema legitimately produces.
    const auto buf = pack_frame_with_payload_count(8);
    auto frame = wd::decode_frame(buf.data(), buf.size(), /*max_payload_fields=*/8);

    EXPECT_TRUE(static_cast<bool>(frame)) << "exactly max_payload_fields must be accepted";
}

TEST(ZmqWireFrameTest, SmallSchemasFloorAtTheTupleSize_ExactCheckIsTheCaller)
{
    // Honest bound, pinned so nobody later "tightens" it and breaks decoding.
    //
    // msgpack exposes ONE array limit covering every array in the message,
    // and the outer frame is always a 5-tuple.  So the bound can never be
    // lower than kFrameTupleSize, and for a schema with fewer than 5 fields
    // a slightly-oversized payload array passes the DECODER.
    //
    // That is not a hole: the limit's job is bounding ALLOCATION, and 5
    // elements is not an allocation concern.  Exact field-count enforcement
    // belongs to the caller's `payload_size != schema_defs_.size()` check,
    // which both ZmqQueue and InboxQueue perform immediately after decoding.
    const auto buf = pack_frame_with_payload_count(5);
    auto frame = wd::decode_frame(buf.data(), buf.size(), /*max_payload_fields=*/1);

    EXPECT_TRUE(static_cast<bool>(frame))
        << "with a 1-field schema the array bound floors at the 5-tuple, so a "
           "5-element payload decodes; the caller rejects it on payload_size";
    EXPECT_EQ(frame.env.payload_size, 5u)
        << "the decoder must report the true count so the caller CAN reject it";
}

TEST(ZmqWireFrameTest, SmallSchemaStillAdmitsTheFiveTupleItself)
{
    // The outer frame is always 5 elements.  A schema with fewer fields than
    // that must not cause the outer array to breach the limit — the bound is
    // floored at kFrameTupleSize for exactly this reason.
    const auto buf = pack_frame_with_payload_count(1);
    auto frame = wd::decode_frame(buf.data(), buf.size(), /*max_payload_fields=*/1);

    EXPECT_TRUE(static_cast<bool>(frame))
        << "a 1-field schema must still decode: the outer 5-tuple cannot be "
           "squeezed out by a small max_payload_fields";
    EXPECT_EQ(frame.env.payload_size, 1u);
}

// ── S-7: a declared size larger than the buffer must not drive allocation ──

TEST(ZmqWireFrameTest, RejectsDeclaredArrayFarLargerThanTheBuffer)
{
    // The DoS shape: an array header claiming 0xffffffff elements with none
    // of them present.  msgpack checks the limit at unpack.hpp:114 and only
    // then allocates n * sizeof(msgpack::object) at :124 — so on a 64-bit
    // host this header alone was a ~68 GB allocation request from a frame
    // that is a handful of bytes on the wire.
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_array(5);
    pk.pack(wd::kFrameMagic);
    pk.pack_bin(8);
    const std::array<std::uint8_t, 8> tag{};
    pk.pack_bin_body(reinterpret_cast<const char *>(tag.data()), 8);
    pk.pack(std::uint64_t{1});
    pk.pack_array(0xffffffffu); // header only — the elements are never sent
    ASSERT_LT(buf.size(), 128u) << "the hostile frame must stay tiny; that is the point";

    auto frame = wd::decode_frame(buf.data(), buf.size(), /*max_payload_fields=*/4);
    EXPECT_FALSE(static_cast<bool>(frame));
}

TEST(ZmqWireFrameTest, RejectsDeclaredBinLargerThanTheBuffer)
{
    // Same shape on the bin axis: a `bin` header declaring far more bytes
    // than were transmitted.  str/bin are bounded by the input size, so a
    // blob can never claim to be bigger than the buffer it arrived in.
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    pk.pack_array(5);
    pk.pack(wd::kFrameMagic);
    pk.pack_bin(0x7fffffff); // header only
    ASSERT_LT(buf.size(), 128u);

    auto frame = wd::decode_frame(buf.data(), buf.size(), /*max_payload_fields=*/4);
    EXPECT_FALSE(static_cast<bool>(frame));
}

// ── Structural rejections (unchanged by S-7, pinned so they stay) ─────────

TEST(ZmqWireFrameTest, RejectsWrongMagic)
{
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(&buf);
    const std::array<std::uint8_t, 8> tag{};
    std::uint8_t checksum[32]{};
    pk.pack_array(5);
    pk.pack(static_cast<std::uint32_t>(0xDEADBEEF)); // not kFrameMagic
    pk.pack_bin(8);
    pk.pack_bin_body(reinterpret_cast<const char *>(tag.data()), 8);
    pk.pack(std::uint64_t{1});
    pk.pack_array(0);
    pk.pack_bin(32);
    pk.pack_bin_body(reinterpret_cast<const char *>(checksum), 32);

    auto frame = wd::decode_frame(buf.data(), buf.size(), 4);
    EXPECT_FALSE(static_cast<bool>(frame));
}

TEST(ZmqWireFrameTest, RejectsGarbageAndEmptyInput)
{
    const std::string garbage = "not msgpack at all, just text";
    auto g = wd::decode_frame(garbage.data(), garbage.size(), 4);
    EXPECT_FALSE(static_cast<bool>(g));

    auto e = wd::decode_frame("", 0, 4);
    EXPECT_FALSE(static_cast<bool>(e)) << "empty input must be refused, not crash";
}

// ── Lifetime: the decoded frame owns its bytes ────────────────────────────

TEST(ZmqWireFrameTest, DecodedFrameOutlivesTheSourceBuffer)
{
    // `decode_frame` passes no reference function, so msgpack copies every
    // str/bin into its own zone (unpack.hpp:173-177) rather than pointing at
    // the caller's buffer.  That is what lets DecodedFrame be returned by
    // value and read after the message it came from is gone — the property
    // the API doc promises.  If it ever regressed to referencing, this test
    // reads freed memory and ASAN fails the build.
    wd::DecodedFrame frame;
    {
        const auto buf = pack_frame_with_payload_count(3, /*seq=*/99);
        frame = wd::decode_frame(buf.data(), buf.size(), 3);
        ASSERT_TRUE(static_cast<bool>(frame));
    } // source buffer destroyed here

    EXPECT_EQ(frame.env.seq, 99u);
    EXPECT_EQ(frame.env.payload_size, 3u);
    ASSERT_NE(frame.env.recv_tag, nullptr);
    EXPECT_EQ(frame.env.recv_tag[3], 4) << "tag bytes must still be readable after the "
                                           "source buffer is gone";
}
