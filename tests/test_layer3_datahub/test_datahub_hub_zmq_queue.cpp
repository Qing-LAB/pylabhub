/**
 * @file test_datahub_hub_zmq_queue.cpp
 * @brief Unit tests for hub::ZmqQueue (ZMQ PULL/PUSH-backed Queue).
 *
 * Suite: ZmqQueueTest
 *
 * ZmqQueue creates its own private zmq_ctx_new() per instance, so no
 * shared ZmqContext lifecycle is needed. Tests use an in-process
 * LifecycleGuard for Logger only.
 *
 * Cross-platform notes:
 *   - All endpoints use tcp://127.0.0.1:<port> (never ipc://)
 *   - Timeouts are >= 500ms (Windows timer resolution ~15.6ms)
 *   - All tests use port 0 + actual_endpoint() for OS-assigned ports (no collisions)
 *   - schema_ep() uses a PID-based port range for factory-only tests (no bind/start)
 */
#include "plh_service.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/hub_queue.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "test_sync_utils.h"

#include <gtest/gtest.h>

using namespace pylabhub::hub;
using namespace std::chrono_literals;
using pylabhub::tests::helper::poll_until;

namespace
{

constexpr size_t kItemSize = 64;

/// Build an 8-byte schema tag from a uint64 value.
inline std::array<uint8_t, 8> make_tag(uint64_t val)
{
    std::array<uint8_t, 8> tag{};
    std::memcpy(tag.data(), &val, 8);
    return tag;
}

/// Single-blob schema: one bytes field of exactly n bytes.
/// Used for transport-level tests that treat the payload as opaque bytes.
inline std::vector<ZmqSchemaField> blob_schema(size_t n)
{
    return {{"bytes", 1, static_cast<uint32_t>(n)}};
}

/// Deterministic endpoint for factory-only tests (no bind/start).
/// These tests only create queue objects to test construction and schema logic;
/// they never call start() so the port is never actually bound.
/// Tests that bind must use "tcp://127.0.0.1:0" + actual_endpoint() instead.
std::string schema_ep(int offset = 0)
{
    constexpr int kPidModulus = 1459; // prime — reduces hash collisions in parallel runs
    int base_port = 48000 + static_cast<int>(pylabhub::platform::get_pid() % kPidModulus) * 12 + offset;
    return "tcp://127.0.0.1:" + std::to_string(base_port);
}

/// Deterministic test pattern: byte at position i gets value (i*7+13) & 0xFF.
inline uint8_t data_pattern(size_t i) noexcept
{
    return static_cast<uint8_t>((i * 7u + 13u) & 0xFFu);
}

} // anonymous namespace

// ============================================================================
// Fixture — Logger lifecycle only (ZmqQueue creates its own ZMQ context)
// ============================================================================

class ZmqQueueTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(pylabhub::utils::Logger::GetLifecycleModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

private:
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> ZmqQueueTest::s_lifecycle_;

// ============================================================================
// Factory tests
// ============================================================================

TEST_F(ZmqQueueTest, PullFrom_Creates)
{
    auto q = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true, 100);
    ASSERT_NE(q, nullptr);
    EXPECT_FALSE(q->is_running());
}

TEST_F(ZmqQueueTest, PushTo_Creates)
{
    auto q = ZmqQueue::push_to("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/false);
    ASSERT_NE(q, nullptr);
    EXPECT_FALSE(q->is_running());
}

// ============================================================================
// Lifecycle tests
// ============================================================================

TEST_F(ZmqQueueTest, Start_SetsRunning)
{
    auto q = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(q->start());
    std::this_thread::sleep_for(50ms); // recv_thread_ startup
    EXPECT_TRUE(q->is_running());
    q->stop();
}

TEST_F(ZmqQueueTest, Stop_ClearsRunning)
{
    auto q = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(q->start());
    q->stop();
    EXPECT_FALSE(q->is_running());
}

TEST_F(ZmqQueueTest, DoubleStop_NoThrow)
{
    auto q = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(q->start());
    q->stop();
    EXPECT_NO_THROW(q->stop());
}

// ============================================================================
// Read/Write roundtrip tests
// ============================================================================

TEST_F(ZmqQueueTest, Roundtrip_SingleItem)
{
    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    // Write a pattern
    void *wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xAB, kItemSize);
    push->write_commit();

    // Read it back
    const void *rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr) << "read_acquire timed out";

    // Verify pattern
    auto bytes = static_cast<const uint8_t *>(rbuf);
    for (size_t i = 0; i < kItemSize; ++i)
    {
        ASSERT_EQ(bytes[i], 0xAB) << "Mismatch at byte " << i;
    }
    pull->read_release();

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Roundtrip_MultipleItems)
{
    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    constexpr int kCount = 5;
    for (int i = 0; i < kCount; ++i)
    {
        void *wbuf = push->write_acquire(1000ms);
        ASSERT_NE(wbuf, nullptr);
        std::memset(wbuf, static_cast<uint8_t>(i + 1), kItemSize);
        push->write_commit();
    }

    for (int i = 0; i < kCount; ++i)
    {
        const void *rbuf = pull->read_acquire(2000ms);
        ASSERT_NE(rbuf, nullptr) << "read_acquire timed out on item " << i;
        auto bytes = static_cast<const uint8_t *>(rbuf);
        EXPECT_EQ(bytes[0], static_cast<uint8_t>(i + 1)) << "Item " << i << " mismatch";
        pull->read_release();
    }

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, ReadTimeout_ReturnsNull)
{
    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    // No writer — read should time out
    const void *rbuf = pull->read_acquire(500ms);
    EXPECT_EQ(rbuf, nullptr) << "Expected nullptr on timeout";

    pull->stop();
}

// ============================================================================
// Write semantics tests
// ============================================================================

TEST_F(ZmqQueueTest, WriteAbort_NotSent)
{
    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    // Acquire then discard — no message should be sent
    void *wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xFF, kItemSize);
    push->write_discard();

    // Reader should timeout
    const void *rbuf = pull->read_acquire(500ms);
    EXPECT_EQ(rbuf, nullptr) << "Expected nullptr — discarded write should not be received";

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, ItemSize_Correct)
{
    auto q = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    EXPECT_EQ(q->item_size(), kItemSize);
}

// ============================================================================
// Metadata tests
// ============================================================================

TEST_F(ZmqQueueTest, Name_ReturnsEndpoint)
{
    const std::string ep = "tcp://127.0.0.1:0";
    auto q = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "aligned", /*bind=*/true);
    EXPECT_EQ(q->name(), ep);
}

// ============================================================================
// Buffer overflow tests
// ============================================================================

TEST_F(ZmqQueueTest, PullFrom_BufferFull_DropsOldest)
{
    // When the recv ring is full, recv_thread_ drops the OLDEST slot and writes
    // the NEWEST frame into the vacated position.  After overflow, the ring must
    // contain exactly the last kBufDepth items sent, in order.
    //
    // Proof: with kBufDepth=4 and kSend=12, items 1–8 overflow (recv_overflow_count=8),
    // ring retains items 9–12.  read_acquire() returns them FIFO: 9, 10, 11, 12.
    constexpr size_t kBufDepth = 4;
    constexpr int    kSend     = 12;

    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true, kBufDepth);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false,
                                  /*tag=*/std::nullopt, /*sndhwm=*/0, /*depth=*/64);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    // Push kSend items — first byte of each slot = send index (1-based).
    for (int i = 0; i < kSend; ++i)
    {
        void *wbuf = push->write_acquire(500ms);
        ASSERT_NE(wbuf, nullptr) << "write_acquire failed at i=" << i;
        std::memset(wbuf, static_cast<uint8_t>(i + 1), kItemSize);
        push->write_commit();
    }

    // Wait for recv_thread_ to decode and buffer all kSend frames.
    ASSERT_TRUE(poll_until(
        [&]{ return pull->metrics().recv_overflow_count >= static_cast<uint64_t>(kSend - kBufDepth); },
        3000ms))
        << "recv_thread_ did not overflow the expected number of frames within 3s";

    // Verify overflow count: kSend - kBufDepth items must have been dropped.
    EXPECT_EQ(pull->metrics().recv_overflow_count, static_cast<uint64_t>(kSend - kBufDepth))
        << "Exactly kSend-kBufDepth items must have overflowed";

    // Read the remaining kBufDepth items; verify they are the NEWEST (oldest dropped).
    for (int i = 0; i < static_cast<int>(kBufDepth); ++i)
    {
        const void *rbuf = pull->read_acquire(500ms);
        ASSERT_NE(rbuf, nullptr) << "Expected item at position " << i;
        // The newest kBufDepth items have values kSend-kBufDepth+1 … kSend.
        const uint8_t expected = static_cast<uint8_t>(kSend - static_cast<int>(kBufDepth) + 1 + i);
        EXPECT_EQ(static_cast<const uint8_t*>(rbuf)[0], expected)
            << "Wrong item at position " << i << ": expected value " << (int)expected;
        pull->read_release();
    }

    // Ring must now be empty (no extra items).
    EXPECT_EQ(pull->read_acquire(200ms), nullptr) << "Ring must be empty after reading kBufDepth items";

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, PullFrom_BufferFull_NoDeadlock)
{
    // Push rapidly with small buffer; verify no hang after 2s.
    constexpr size_t kBufDepth = 2;

    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true, kBufDepth);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    // Rapid push without reading
    for (int i = 0; i < 50; ++i)
    {
        void *wbuf = push->write_acquire(500ms);
        if (wbuf == nullptr)
            break; // push side may back-pressure; that's OK
        std::memset(wbuf, 0xCD, kItemSize);
        push->write_commit();
    }

    // Stop both — must complete without deadlock
    push->stop();
    pull->stop();
    // If we reach here, no deadlock occurred.
}

// ============================================================================
// item_size preservation tests
// MessagePack encodes types in the frame envelope; no alignment rounding needed.
// The recv_buf_ allocator guarantees >= max_align_t alignment independently.
// ============================================================================

TEST_F(ZmqQueueTest, ItemSize_BlobSchema_ExactBytes)
{
    // blob_schema(N) must yield item_size() == N with no rounding.
    // Covered for a wide range by DataIntegrity_VariousSizes; this is a fast
    // smoke-test that confirms the factory-side computation is exact for three
    // representative values (odd, non-power-of-2, and power-of-2).
    auto q7  = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(7),  "aligned", true);
    auto q64 = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(64), "aligned", true);
    auto q9  = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(9),  "aligned", true);
    ASSERT_NE(q7,  nullptr); EXPECT_EQ(q7->item_size(),   7u);
    ASSERT_NE(q64, nullptr); EXPECT_EQ(q64->item_size(), 64u);
    ASSERT_NE(q9,  nullptr); EXPECT_EQ(q9->item_size(),   9u);
}

// ============================================================================
// Schema tag validation tests
// ============================================================================

TEST_F(ZmqQueueTest, SchemaTag_Match_DeliversItem)
{
    // PUSH and PULL configured with the SAME schema tag → items are delivered.
    auto tag = make_tag(0xDEADBEEFCAFEBABEull);

    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true, 64, tag);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false, tag);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    void *wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0x42, kItemSize);
    push->write_commit();

    const void *rbuf = pull->read_acquire(2000ms);
    EXPECT_NE(rbuf, nullptr) << "Matching schema tag: item should be delivered";

    if (rbuf)
    {
        auto *bytes = static_cast<const uint8_t *>(rbuf);
        EXPECT_EQ(bytes[0], 0x42u);
        pull->read_release();
    }
    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, SchemaTag_Mismatch_DropsAndCountsErrors)
{
    // PUSH uses tag A, PULL expects tag B → frames are rejected; none delivered.
    auto tag_a = make_tag(0x1111111111111111ull);
    auto tag_b = make_tag(0x2222222222222222ull);

    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true, 64, tag_b);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false, tag_a);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    constexpr int kCount = 5;
    for (int i = 0; i < kCount; ++i)
    {
        void *wbuf = push->write_acquire(500ms);
        ASSERT_NE(wbuf, nullptr);
        std::memset(wbuf, static_cast<uint8_t>(i + 1), kItemSize);
        push->write_commit();
    }

    // Wait for recv_thread_ to reject all frames.
    ASSERT_TRUE(poll_until(
        [&]{ return pull->metrics().recv_frame_error_count >= static_cast<uint64_t>(kCount); },
        3000ms))
        << "recv_thread_ did not reject all mismatched frames within 3s";

    // No items should reach the read buffer.
    const void *rbuf = pull->read_acquire(0ms);
    EXPECT_EQ(rbuf, nullptr) << "Schema mismatch: no items should be delivered";

    EXPECT_GE(pull->metrics().recv_frame_error_count, static_cast<uint64_t>(kCount))
        << "Each rejected frame should be counted as a frame error";

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, SchemaTag_NoTag_AcceptsAnyFrame)
{
    // PUSH has a tag but PULL has NO tag (nullopt) → PULL accepts any frame.
    auto tag = make_tag(0xAAAABBBBCCCCDDDDull);

    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true); // no tag
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false, tag);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    void *wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0x77, kItemSize);
    push->write_commit();

    const void *rbuf = pull->read_acquire(2000ms);
    EXPECT_NE(rbuf, nullptr) << "No tag on receiver: any frame should be accepted";
    if (rbuf)
    {
        EXPECT_EQ(static_cast<const uint8_t *>(rbuf)[0], 0x77u);
        pull->read_release();
    }
    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);

    push->stop();
    pull->stop();
}

// ============================================================================
// Overflow counter tests
// ============================================================================

TEST_F(ZmqQueueTest, OverflowCounter_Increments)
{
    // Conservation law: every sent frame is either buffered or overflowed.
    // With kBufDepth=2 and kSend=20, exactly kSend-kBufDepth items overflow.
    // After stop(), unread items in the ring are drained, so:
    //   recv_overflow_count() + items_readable == kSend (conservation law).
    constexpr size_t kBufDepth = 2;
    constexpr int    kSend     = 20;

    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true, kBufDepth);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    // Send kSend items without reading.
    int sent = 0;
    for (int i = 0; i < kSend; ++i)
    {
        void *wbuf = push->write_acquire(500ms);
        if (!wbuf) break;
        std::memset(wbuf, static_cast<uint8_t>(i + 1), kItemSize);
        push->write_commit();
        ++sent;
    }
    ASSERT_EQ(sent, kSend) << "All writes must succeed (push ring depth is large)";

    // Wait for recv_thread_ to process all frames (conservation: overflow >= kSend-kBufDepth).
    ASSERT_TRUE(poll_until(
        [&]{ return pull->metrics().recv_overflow_count >= static_cast<uint64_t>(kSend - kBufDepth); },
        3000ms))
        << "recv_thread_ did not process all frames within 3s";

    // Drain whatever survives in the ring.
    int readable = 0;
    while (pull->read_acquire(0ms) != nullptr)
    {
        pull->read_release();
        ++readable;
    }

    // Conservation: every sent frame is either in the ring or overflowed.
    const uint64_t overflowed = pull->metrics().recv_overflow_count;
    EXPECT_EQ(static_cast<int>(overflowed) + readable, kSend)
        << "recv_overflow_count + readable items must equal kSend";
    EXPECT_GT(overflowed, 0u) << "At least kSend-kBufDepth items must have overflowed";
    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);

    push->stop();
    pull->stop();
}

// ============================================================================
// Sequence number / ordering tests
// ============================================================================

TEST_F(ZmqQueueTest, MultipleItems_Ordered)
{
    // Verify that items arrive in the same order they were sent.
    auto pull = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(kItemSize), "aligned", /*bind=*/true);
    ASSERT_TRUE(pull->start());
    const std::string ep = static_cast<ZmqQueue*>(pull.get())->actual_endpoint();
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "aligned", /*bind=*/false);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms); // connection setup

    constexpr int kCount = 8;
    for (int i = 0; i < kCount; ++i)
    {
        void *wbuf = push->write_acquire(500ms);
        ASSERT_NE(wbuf, nullptr);
        // Stamp first byte with sequence value i+1.
        static_cast<uint8_t *>(wbuf)[0] = static_cast<uint8_t>(i + 1);
        push->write_commit();
    }

    for (int i = 0; i < kCount; ++i)
    {
        const void *rbuf = pull->read_acquire(2000ms);
        ASSERT_NE(rbuf, nullptr) << "Item " << i << " not received";
        EXPECT_EQ(static_cast<const uint8_t *>(rbuf)[0], static_cast<uint8_t>(i + 1))
            << "Order mismatch at position " << i;
        pull->read_release();
    }
    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);

    push->stop();
    pull->stop();
}

// ============================================================================
// Schema-mode tests (ZmqSchemaField factories)
// Payload element [3] is a msgpack array of typed field values in schema mode.
// ============================================================================

// ── item_size computation (no ZMQ) ──────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_ItemSize_NaturalAlignment)
{
    // {int32, float64} natural: int32@0(4), pad(4), float64@8(8) → total 16.
    std::vector<ZmqSchemaField> schema = {{"int32", 1, 0}, {"float64", 1, 0}};
    auto q = ZmqQueue::pull_from(schema_ep(0), schema, "aligned", /*bind=*/true);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->item_size(), 16u);
}

TEST_F(ZmqQueueTest, Schema_ItemSize_Packed)
{
    // {int32, float64} packed: int32@0(4), float64@4(8) → total 12.
    std::vector<ZmqSchemaField> schema = {{"int32", 1, 0}, {"float64", 1, 0}};
    auto q = ZmqQueue::pull_from(schema_ep(0), schema, "packed", /*bind=*/true);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->item_size(), 12u);
}

TEST_F(ZmqQueueTest, Schema_ItemSize_Array)
{
    // {float64[4]} natural: float64[4]@0(32) → total 32.
    std::vector<ZmqSchemaField> schema = {{"float64", 4, 0}};
    auto q = ZmqQueue::pull_from(schema_ep(0), schema, "aligned", /*bind=*/true);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->item_size(), 32u);
}

TEST_F(ZmqQueueTest, Schema_ItemSize_StringField)
{
    // {string(length=8)}: char[8]@0 → total 8.
    std::vector<ZmqSchemaField> schema = {{"string", 1, 8}};
    auto q = ZmqQueue::pull_from(schema_ep(0), schema, "aligned", /*bind=*/true);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->item_size(), 8u);
}

// ── scalar roundtrip ─────────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_Scalars_Roundtrip)
{
    // {int32, float64} natural: int32@0, float64@8.
    // Scalars encoded as native msgpack types — exact values preserved.
    std::vector<ZmqSchemaField> schema = {{"int32", 1, 0}, {"float64", 1, 0}};

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", schema, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->item_size(), 16u);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, schema, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0, 16);
    int32_t si = -42;
    double  sd = 3.14;
    std::memcpy(static_cast<char*>(wbuf) + 0, &si, 4);
    std::memcpy(static_cast<char*>(wbuf) + 8, &sd, 8);
    push->write_commit();

    const void* rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr) << "Schema scalars roundtrip: item not received";
    int32_t ri = 0; double rd = 0.0;
    std::memcpy(&ri, static_cast<const char*>(rbuf) + 0, 4);
    std::memcpy(&rd, static_cast<const char*>(rbuf) + 8, 8);
    EXPECT_EQ(ri, -42);
    EXPECT_DOUBLE_EQ(rd, 3.14);
    pull->read_release();

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

// ── array roundtrip ──────────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_Array_Roundtrip)
{
    // {float64[4]}: array → bin(32), size-validated on recv.
    std::vector<ZmqSchemaField> schema = {{"float64", 4, 0}};

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", schema, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, schema, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    const double sv[4] = {1.1, 2.2, 3.3, 4.4};
    void* wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memcpy(wbuf, sv, 32);
    push->write_commit();

    const void* rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr);
    double rv[4] = {};
    std::memcpy(rv, rbuf, 32);
    for (int i = 0; i < 4; ++i)
        EXPECT_DOUBLE_EQ(rv[i], sv[i]) << "Element " << i;
    pull->read_release();

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

// ── mixed schema roundtrip ───────────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_Mixed_Natural_Roundtrip)
{
    // {int32, float64[4], uint8} natural:
    //   int32@0(4), pad(4), float64[4]@8(32), uint8@40(1), pad(7) → 48 bytes.
    std::vector<ZmqSchemaField> schema = {
        {"int32",   1, 0},
        {"float64", 4, 0},
        {"uint8",   1, 0},
    };

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", schema, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->item_size(), 48u);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, schema, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0, 48);
    int32_t si    = 777;
    double  sd[4] = {10.0, 20.0, 30.0, 40.0};
    uint8_t su    = 99;
    std::memcpy(static_cast<char*>(wbuf) +  0, &si, 4);
    std::memcpy(static_cast<char*>(wbuf) +  8, sd,  32);
    std::memcpy(static_cast<char*>(wbuf) + 40, &su, 1);
    push->write_commit();

    const void* rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr);
    int32_t ri = 0; double rd[4] = {}; uint8_t ru = 0;
    std::memcpy(&ri, static_cast<const char*>(rbuf) +  0, 4);
    std::memcpy(rd,  static_cast<const char*>(rbuf) +  8, 32);
    std::memcpy(&ru, static_cast<const char*>(rbuf) + 40, 1);
    EXPECT_EQ(ri, 777);
    for (int i = 0; i < 4; ++i)
        EXPECT_DOUBLE_EQ(rd[i], sd[i]) << "float64[" << i << "]";
    EXPECT_EQ(ru, 99u);
    pull->read_release();

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

// ── factory validation ────────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_InvalidPacking_ReturnsNullptr)
{
    // Any packing string other than "aligned" or "packed" must return nullptr (B1).
    auto pull = ZmqQueue::pull_from(schema_ep(4), blob_schema(8), "NATURAL", /*bind=*/true);
    EXPECT_EQ(pull, nullptr) << "pull_from: invalid packing must return nullptr";

    auto push = ZmqQueue::push_to(schema_ep(4), blob_schema(8), "big-endian");
    EXPECT_EQ(push, nullptr) << "push_to: invalid packing must return nullptr";
}

TEST_F(ZmqQueueTest, Schema_ZeroLengthBytesField_ReturnsNullptr)
{
    // A string/bytes field with length=0 creates a degenerate zero-size schema (B2).
    // Both factories must reject it.
    std::vector<ZmqSchemaField> schema = {{"bytes", 1, 0}};

    auto pull = ZmqQueue::pull_from(schema_ep(4), schema, "aligned", /*bind=*/true);
    EXPECT_EQ(pull, nullptr) << "pull_from: bytes field length=0 must return nullptr";

    auto push = ZmqQueue::push_to(schema_ep(4), schema, "aligned");
    EXPECT_EQ(push, nullptr) << "push_to: bytes field length=0 must return nullptr";
}

// ── type-safety rejection tests ──────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_EmptySchema_ReturnsNullptr)
{
    // Empty schema is an error — both factories must return nullptr.
    auto pull = ZmqQueue::pull_from(schema_ep(4), {}, "aligned", /*bind=*/true);
    EXPECT_EQ(pull, nullptr) << "pull_from with empty schema must return nullptr";

    auto push = ZmqQueue::push_to(schema_ep(4), {}, "aligned", /*bind=*/true);
    EXPECT_EQ(push, nullptr) << "push_to with empty schema must return nullptr";
}

TEST_F(ZmqQueueTest, Schema_FieldCountMismatch_Rejected)
{
    // Sender has 2 fields, receiver expects 1 → array length mismatch → rejected.
    std::vector<ZmqSchemaField> ss = {{"int32", 1, 0}, {"float64", 1, 0}};
    std::vector<ZmqSchemaField> sr = {{"int32", 1, 0}};

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", ss, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, sr, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(500ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0, push->item_size());
    push->write_commit();

    ASSERT_TRUE(poll_until([&]{ return pull->metrics().recv_frame_error_count >= 1u; }, 3000ms))
        << "recv_thread_ did not reject the mismatched frame within 3s";

    EXPECT_EQ(pull->read_acquire(0ms), nullptr) << "Field count mismatch must be rejected";
    EXPECT_GE(pull->metrics().recv_frame_error_count, 1u);

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Schema_ArraySizeMismatch_Rejected)
{
    // Sender encodes float64[4] (bin 32 B), receiver expects float64[3] (bin 24 B).
    // Bin size mismatch → rejected.
    std::vector<ZmqSchemaField> ss = {{"float64", 4, 0}};
    std::vector<ZmqSchemaField> sr = {{"float64", 3, 0}};

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", ss, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, sr, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(500ms);
    ASSERT_NE(wbuf, nullptr);
    const double vals[4] = {1.0, 2.0, 3.0, 4.0};
    std::memcpy(wbuf, vals, 32);
    push->write_commit();

    ASSERT_TRUE(poll_until([&]{ return pull->metrics().recv_frame_error_count >= 1u; }, 3000ms))
        << "recv_thread_ did not reject the mismatched frame within 3s";

    EXPECT_EQ(pull->read_acquire(0ms), nullptr) << "Array size mismatch must be rejected";
    EXPECT_GE(pull->metrics().recv_frame_error_count, 1u);

    push->stop();
    pull->stop();
}

// ============================================================================
// Data integrity tests — various sizes and alignment edge cases
//
// Purpose: verify that the msgpack framing encode/decode roundtrip is correct
// for payload sizes from 1 byte to well above one memory page (4096 bytes),
// covering every important alignment boundary.  The ZMQ frame size is the
// msgpack envelope (magic + schema_tag + seq + payload[]), whose total byte
// count does NOT align with the data item size.  These tests confirm that the
// length-prefix logic in pack/unpack handles all sizes without corruption.
// ============================================================================

TEST_F(ZmqQueueTest, DataIntegrity_VariousSizes)
{
    // Sizes chosen to exercise: minimal, odd non-power-of-2 (3,5,7,9,11,17,63,65),
    // cache-line boundary (63/65), power-of-2 (128,256,1024,4096), one byte below/above
    // page size (4095/4097), and 4x-page (16384).
    //
    // Port 0 (OS-assigned ephemeral port) + actual_endpoint() is used so that
    // each sub-test binds to an independent port with no PID-formula collisions.
    const size_t cases[] = {
        1, 3, 5, 7, 9, 11, 17, 63, 65, 127,
        128, 129, 255, 256, 1023, 1024, 4095, 4096, 4097, 16384,
    };

    for (const size_t sz : cases)
    {
        SCOPED_TRACE("size=" + std::to_string(sz));

        // Bind PUSH to port 0 — OS assigns an ephemeral port.
        auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", blob_schema(sz), "aligned", /*bind=*/true);
        ASSERT_NE(push, nullptr) << "push_to nullptr for size=" << sz;
        ASSERT_EQ(push->item_size(), sz);

        ASSERT_TRUE(push->start());
        // Retrieve the actual OS-assigned endpoint for PULL to connect to.
        const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();
        ASSERT_FALSE(ep.empty()) << "actual_endpoint empty for size=" << sz;

        auto pull = ZmqQueue::pull_from(ep, blob_schema(sz), "aligned", /*bind=*/false);
        ASSERT_NE(pull, nullptr) << "pull_from nullptr for size=" << sz;
        ASSERT_EQ(pull->item_size(), sz);

        ASSERT_TRUE(pull->start());
        std::this_thread::sleep_for(50ms);

        // Fill with deterministic byte pattern.
        void* wbuf = push->write_acquire(1000ms);
        ASSERT_NE(wbuf, nullptr);
        auto* wb = static_cast<uint8_t*>(wbuf);
        for (size_t i = 0; i < sz; ++i)
            wb[i] = data_pattern(i);
        push->write_commit();

        const void* rbuf = pull->read_acquire(2000ms);
        ASSERT_NE(rbuf, nullptr) << "Timed out for size=" << sz;

        // Verify every byte survived the msgpack roundtrip.
        const auto* rb = static_cast<const uint8_t*>(rbuf);
        for (size_t i = 0; i < sz; ++i)
        {
            ASSERT_EQ(rb[i], data_pattern(i))
                << "Corruption at byte=" << i << " size=" << sz;
        }
        pull->read_release();
        EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);

        push->stop();
        pull->stop();
    }
}

TEST_F(ZmqQueueTest, Schema_AllScalarTypes_Roundtrip)
{
    // Schema: one scalar of every supported numeric type.
    // Natural layout (48 bytes):
    //   bool@0(1), int8@1(1), uint8@2(1), pad@3(1),
    //   int16@4(2), uint16@6(2),
    //   int32@8(4), uint32@12(4),
    //   int64@16(8), uint64@24(8),
    //   float32@32(4), pad@36-39(4), float64@40(8)
    std::vector<ZmqSchemaField> schema = {
        {"bool",    1, 0}, {"int8",    1, 0}, {"uint8",   1, 0},
        {"int16",   1, 0}, {"uint16",  1, 0},
        {"int32",   1, 0}, {"uint32",  1, 0},
        {"int64",   1, 0}, {"uint64",  1, 0},
        {"float32", 1, 0}, {"float64", 1, 0},
    };
    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", schema, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->item_size(), 48u);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, schema, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    // Write test values at the computed natural offsets.
    void* wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    char* wb = static_cast<char*>(wbuf);
    std::memset(wb, 0, 48);

    bool     sv_bool = true;
    int8_t   sv_i8   = -55;
    uint8_t  sv_u8   = 200;
    int16_t  sv_i16  = -1234;
    uint16_t sv_u16  = 65000;
    int32_t  sv_i32  = -999999;
    uint32_t sv_u32  = 3000000000u;
    int64_t  sv_i64  = -9000000000LL;
    uint64_t sv_u64  = 18000000000ULL;
    float    sv_f32  = 1.5f;
    double   sv_f64  = -3.14159265358979;

    std::memcpy(wb +  0, &sv_bool, 1);
    std::memcpy(wb +  1, &sv_i8,   1);
    std::memcpy(wb +  2, &sv_u8,   1);
    std::memcpy(wb +  4, &sv_i16,  2);
    std::memcpy(wb +  6, &sv_u16,  2);
    std::memcpy(wb +  8, &sv_i32,  4);
    std::memcpy(wb + 12, &sv_u32,  4);
    std::memcpy(wb + 16, &sv_i64,  8);
    std::memcpy(wb + 24, &sv_u64,  8);
    std::memcpy(wb + 32, &sv_f32,  4);
    std::memcpy(wb + 40, &sv_f64,  8);
    push->write_commit();

    const void* rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr) << "All-scalar-types: item not received";
    const char* rb = static_cast<const char*>(rbuf);

    bool     rv_bool = false; int8_t   rv_i8  = 0; uint8_t  rv_u8  = 0;
    int16_t  rv_i16  = 0;     uint16_t rv_u16 = 0;
    int32_t  rv_i32  = 0;     uint32_t rv_u32 = 0;
    int64_t  rv_i64  = 0;     uint64_t rv_u64 = 0;
    float    rv_f32  = 0.0f;  double   rv_f64 = 0.0;

    std::memcpy(&rv_bool, rb +  0, 1);
    std::memcpy(&rv_i8,   rb +  1, 1);
    std::memcpy(&rv_u8,   rb +  2, 1);
    std::memcpy(&rv_i16,  rb +  4, 2);
    std::memcpy(&rv_u16,  rb +  6, 2);
    std::memcpy(&rv_i32,  rb +  8, 4);
    std::memcpy(&rv_u32,  rb + 12, 4);
    std::memcpy(&rv_i64,  rb + 16, 8);
    std::memcpy(&rv_u64,  rb + 24, 8);
    std::memcpy(&rv_f32,  rb + 32, 4);
    std::memcpy(&rv_f64,  rb + 40, 8);

    EXPECT_EQ(rv_bool, sv_bool);
    EXPECT_EQ(rv_i8,   sv_i8);
    EXPECT_EQ(rv_u8,   sv_u8);
    EXPECT_EQ(rv_i16,  sv_i16);
    EXPECT_EQ(rv_u16,  sv_u16);
    EXPECT_EQ(rv_i32,  sv_i32);
    EXPECT_EQ(rv_u32,  sv_u32);
    EXPECT_EQ(rv_i64,  sv_i64);
    EXPECT_EQ(rv_u64,  sv_u64);
    EXPECT_FLOAT_EQ(rv_f32, sv_f32);
    EXPECT_DOUBLE_EQ(rv_f64, sv_f64);
    pull->read_release();

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Schema_LargeArrayOverPageSize_Roundtrip)
{
    // {float64[600]} = 4800 bytes — well over one 4096-byte page.
    // Tests that ring buffer allocation, frame buffer sizing, and msgpack
    // bin encode/decode all handle multi-page payloads correctly.
    std::vector<ZmqSchemaField> schema = {{"float64", 600, 0}};

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", schema, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->item_size(), 600u * 8u);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, schema, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    auto* wd = static_cast<double*>(wbuf);
    for (int i = 0; i < 600; ++i)
        wd[i] = static_cast<double>(i) * 1.5 + 0.25;
    push->write_commit();

    const void* rbuf = pull->read_acquire(3000ms);
    ASSERT_NE(rbuf, nullptr) << "Large array (4800 B) item not received";
    const auto* rd = static_cast<const double*>(rbuf);
    for (int i = 0; i < 600; ++i)
    {
        ASSERT_DOUBLE_EQ(rd[i], static_cast<double>(i) * 1.5 + 0.25)
            << "Mismatch at element=" << i;
    }
    pull->read_release();

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Schema_AlignmentPadding_FieldsPreserved)
{
    // {uint8, float64} natural: uint8@0(1), 7 pad bytes, float64@8(8) → 16 bytes.
    // Verifies that a 7-byte alignment gap between two fields doesn't corrupt
    // either value in the msgpack encode/decode roundtrip.
    std::vector<ZmqSchemaField> schema = {{"uint8", 1, 0}, {"float64", 1, 0}};

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", schema, "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->item_size(), 16u);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, schema, "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xCC, 16); // fill padding sentinel
    uint8_t sv_u8  = 0xAB;
    double  sv_f64 = 2.718281828;
    std::memcpy(static_cast<char*>(wbuf) + 0, &sv_u8,  1);
    std::memcpy(static_cast<char*>(wbuf) + 8, &sv_f64, 8);
    push->write_commit();

    const void* rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr);
    uint8_t rv_u8  = 0;
    double  rv_f64 = 0.0;
    std::memcpy(&rv_u8,  static_cast<const char*>(rbuf) + 0, 1);
    std::memcpy(&rv_f64, static_cast<const char*>(rbuf) + 8, 8);
    EXPECT_EQ(rv_u8, 0xABu);
    EXPECT_DOUBLE_EQ(rv_f64, sv_f64);
    pull->read_release();

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Schema_MixedArrayFields_MultipleTypes_Roundtrip)
{
    // {uint8[100], int32[50], float64[20]} packed:
    //   uint8[100]@0(100B) + int32[50]@100(200B) + float64[20]@300(160B) = 460 bytes.
    // Tests a multi-field schema with array fields of different element widths.
    // Packed mode means no inter-field alignment padding.
    std::vector<ZmqSchemaField> schema = {
        {"uint8",   100, 0},
        {"int32",    50, 0},
        {"float64",  20, 0},
    };

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", schema, "packed", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->item_size(), 460u);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, schema, "packed", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    char* wb = static_cast<char*>(wbuf);
    for (int i = 0; i < 100; ++i)
        wb[i] = static_cast<uint8_t>(i * 3 + 1);
    for (int i = 0; i < 50; ++i)
    {
        int32_t v = -i;
        std::memcpy(wb + 100 + i * 4, &v, 4);
    }
    for (int i = 0; i < 20; ++i)
    {
        double v = static_cast<double>(i) * 0.1;
        std::memcpy(wb + 300 + i * 8, &v, 8);
    }
    push->write_commit();

    const void* rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr) << "Mixed array fields: item not received";
    const char* rb = static_cast<const char*>(rbuf);

    for (int i = 0; i < 100; ++i)
        ASSERT_EQ(static_cast<uint8_t>(rb[i]), static_cast<uint8_t>(i * 3 + 1))
            << "uint8[" << i << "]";
    for (int i = 0; i < 50; ++i)
    {
        int32_t rv = 0;
        std::memcpy(&rv, rb + 100 + i * 4, 4);
        ASSERT_EQ(rv, -i) << "int32[" << i << "]";
    }
    for (int i = 0; i < 20; ++i)
    {
        double rv = 0.0;
        std::memcpy(&rv, rb + 300 + i * 8, 8);
        ASSERT_DOUBLE_EQ(rv, static_cast<double>(i) * 0.1) << "float64[" << i << "]";
    }
    pull->read_release();

    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    push->stop();
    pull->stop();
}

// ============================================================================
// Level 1 — OverflowPolicy + send_thread_ + metrics (primitive direct API)
// ============================================================================

// ── Factory validation ────────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, PushTo_ZeroDepth_ReturnsNull)
{
    // send_buffer_depth == 0 is invalid; factory must return nullptr.
    auto q = ZmqQueue::push_to(schema_ep(11), blob_schema(16), "aligned",
                               /*bind=*/true, /*tag=*/std::nullopt,
                               /*sndhwm=*/0, /*send_buffer_depth=*/0);
    EXPECT_EQ(q, nullptr);
}

TEST_F(ZmqQueueTest, PullFrom_ZeroDepth_ReturnsNull)
{
    // max_buffer_depth == 0 causes % 0 UB in recv_thread_; factory must reject it (C1).
    auto q = ZmqQueue::pull_from(schema_ep(11), blob_schema(16), "aligned",
                                 /*bind=*/true, /*max_buffer_depth=*/0);
    EXPECT_EQ(q, nullptr);
}

TEST_F(ZmqQueueTest, Schema_ZeroCountField_ReturnsNull)
{
    // count == 0 for a numeric field yields zero byte_size with aliased struct offsets
    // — silent data corruption.  Both factories must reject it (C2).
    std::vector<ZmqSchemaField> bad = {{"int32", 0, 0}}; // count=0 is the defect
    auto pull = ZmqQueue::pull_from(schema_ep(11), bad, "aligned", /*bind=*/true);
    EXPECT_EQ(pull, nullptr) << "pull_from: count=0 numeric field must return nullptr";
    auto push = ZmqQueue::push_to(schema_ep(11), bad, "aligned");
    EXPECT_EQ(push, nullptr) << "push_to: count=0 numeric field must return nullptr";
}

// ── Metrics zero on creation ──────────────────────────────────────────────────

TEST_F(ZmqQueueTest, Metrics_ZeroOnCreation_PushQueue)
{
    // All metrics fields must be zero immediately after factory (before start()).
    auto q = ZmqQueue::push_to(schema_ep(11), blob_schema(16), "aligned");
    ASSERT_NE(q, nullptr);

    EXPECT_EQ(q->metrics().overrun_count,    0u);
    EXPECT_EQ(q->metrics().send_drop_count,  0u);
    EXPECT_EQ(q->metrics().send_retry_count, 0u);

    const QueueMetrics m = q->metrics();
    EXPECT_EQ(m.recv_overflow_count,    0u);
    EXPECT_EQ(m.recv_frame_error_count, 0u);
    EXPECT_EQ(m.recv_gap_count,         0u);
    EXPECT_EQ(m.send_drop_count,        0u);
    EXPECT_EQ(m.send_retry_count,       0u);
    EXPECT_EQ(m.overrun_count,          0u);
}

TEST_F(ZmqQueueTest, Metrics_ZeroOnCreation_PullQueue)
{
    // All metrics fields must be zero immediately after factory (before start()).
    auto q = ZmqQueue::pull_from(schema_ep(11), blob_schema(16), "aligned", /*bind=*/true);
    ASSERT_NE(q, nullptr);

    EXPECT_EQ(q->metrics().recv_overflow_count,    0u);
    EXPECT_EQ(q->metrics().recv_frame_error_count, 0u);
    EXPECT_EQ(q->metrics().recv_gap_count,         0u);

    const QueueMetrics m = q->metrics();
    EXPECT_EQ(m.recv_overflow_count,    0u);
    EXPECT_EQ(m.recv_frame_error_count, 0u);
    EXPECT_EQ(m.recv_gap_count,         0u);
    EXPECT_EQ(m.send_drop_count,        0u);
    EXPECT_EQ(m.send_retry_count,       0u);
    EXPECT_EQ(m.overrun_count,          0u);
}

// ── OverflowPolicy::Drop ──────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, OverflowDrop_FullBuffer_ReturnsNullAndIncrements)
{
    // Create a push queue with depth=2, no start() (send_thread_ not running).
    // After 2 successful write_acquire/commit pairs, buffer is full.
    // The 3rd write_acquire must return nullptr and increment overrun_count.
    auto push = ZmqQueue::push_to(schema_ep(12), blob_schema(8), "aligned",
                                  /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0,
                                  /*depth=*/2, OverflowPolicy::Drop);
    ASSERT_NE(push, nullptr);

    void* b1 = push->write_acquire(0ms);
    ASSERT_NE(b1, nullptr) << "slot 1 must succeed (buffer not full)";
    push->write_commit();

    void* b2 = push->write_acquire(0ms);
    ASSERT_NE(b2, nullptr) << "slot 2 must succeed (buffer not full)";
    push->write_commit();

    // Buffer is now full (send_thread_ not running, nothing drained).
    EXPECT_EQ(push->metrics().overrun_count, 0u) << "overrun must be 0 before overflow";

    void* b3 = push->write_acquire(0ms);
    EXPECT_EQ(b3, nullptr) << "Drop policy: 3rd acquire on full buffer must return nullptr";
    EXPECT_EQ(push->metrics().overrun_count, 1u) << "overrun_count must increment on drop";

    // A 4th attempt also drops.
    void* b4 = push->write_acquire(0ms);
    EXPECT_EQ(b4, nullptr);
    EXPECT_EQ(push->metrics().overrun_count, 2u);

    // metrics() struct agrees with individual accessor.
    EXPECT_EQ(push->metrics().overrun_count, 2u);
}

TEST_F(ZmqQueueTest, OverflowDrop_Discard_DoesNotFillSlot)
{
    // write_discard() must not advance the ring tail — buffer stays at 0.
    auto push = ZmqQueue::push_to(schema_ep(12), blob_schema(8), "aligned",
                                  /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0,
                                  /*depth=*/1, OverflowPolicy::Drop);
    ASSERT_NE(push, nullptr);

    void* b1 = push->write_acquire(0ms);
    ASSERT_NE(b1, nullptr);
    push->write_discard(); // discard without committing

    // Buffer should still have one free slot.
    void* b2 = push->write_acquire(0ms);
    EXPECT_NE(b2, nullptr) << "after discard, slot must still be available";
    push->write_commit();

    EXPECT_EQ(push->metrics().overrun_count, 0u);
}

// ── OverflowPolicy::Block ─────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, OverflowBlock_Timeout_ReturnsNullAndIncrements)
{
    // Create a push queue with Block policy and depth=1; fill the slot.
    // A second write_acquire with a short timeout must block then return nullptr.
    auto push = ZmqQueue::push_to(schema_ep(13), blob_schema(8), "aligned",
                                  /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0,
                                  /*depth=*/1, OverflowPolicy::Block);
    ASSERT_NE(push, nullptr);

    void* b1 = push->write_acquire(0ms);
    ASSERT_NE(b1, nullptr);
    push->write_commit(); // fills the only slot

    // Block policy: wait 100ms then give up.
    auto t0 = std::chrono::steady_clock::now();
    void* b2 = push->write_acquire(100ms);
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(b2, nullptr) << "Block policy: timeout must return nullptr";
    EXPECT_GE(elapsed, 80ms) << "must have actually waited close to the timeout";
    EXPECT_EQ(push->metrics().overrun_count, 1u);
    EXPECT_EQ(push->metrics().overrun_count, 1u);
}

// ── Individual accessors agree with metrics() struct ─────────────────────────

TEST_F(ZmqQueueTest, Metrics_IndividualAccessorsMatchStruct)
{
    // After triggering some overruns, verify individual accessors and metrics() agree.
    auto push = ZmqQueue::push_to(schema_ep(14), blob_schema(8), "aligned",
                                  /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0,
                                  /*depth=*/1, OverflowPolicy::Drop);
    ASSERT_NE(push, nullptr);

    push->write_acquire(0ms);
    push->write_commit(); // fill depth=1

    push->write_acquire(0ms); // overflow 1
    push->write_acquire(0ms); // overflow 2
    push->write_acquire(0ms); // overflow 3

    const QueueMetrics m = push->metrics();
    EXPECT_EQ(m.overrun_count,    push->metrics().overrun_count);
    EXPECT_EQ(m.send_drop_count,  push->metrics().send_drop_count);
    EXPECT_EQ(m.send_retry_count, push->metrics().send_retry_count);
    EXPECT_EQ(m.overrun_count,    3u);
    EXPECT_EQ(m.send_drop_count,  0u);
    EXPECT_EQ(m.send_retry_count, 0u);
}

// ── send_thread_ drain on stop ────────────────────────────────────────────────

TEST_F(ZmqQueueTest, SendThread_DrainOnStop_CompletesWithoutHang)
{
    // Fill the send ring with N items, then stop().
    // Verifies stop() terminates cleanly (thread joins) even with pending items.
    // Uses port 0 so OS assigns a free port.
    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", blob_schema(8), "aligned",
                                  /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0,
                                  /*depth=*/8, OverflowPolicy::Drop);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());

    // Connect a PULL socket so ZMQ HWM is not a concern.
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();
    auto pull = ZmqQueue::pull_from(ep, blob_schema(8), "aligned", /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms); // allow TCP handshake

    // Write several items quickly (all fit in ring depth=8).
    for (int i = 0; i < 5; ++i)
    {
        void* buf = push->write_acquire(0ms);
        if (!buf) break;
        static_cast<uint8_t*>(buf)[0] = static_cast<uint8_t>(i + 1);
        push->write_commit();
    }

    // stop() must join send_thread_ without hanging.
    auto t0 = std::chrono::steady_clock::now();
    push->stop();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, 3000ms) << "stop() must complete promptly";
    EXPECT_FALSE(push->is_running());

    pull->stop();
}

TEST_F(ZmqQueueTest, SendThread_Roundtrip_ThenStop)
{
    // Write 3 items and verify they are received before stop().
    // Confirms send_thread_ encodes and delivers without loss under normal conditions.
    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", blob_schema(4), "aligned",
                                  /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0,
                                  /*depth=*/16, OverflowPolicy::Drop);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());

    auto pull = ZmqQueue::pull_from(static_cast<ZmqQueue*>(push.get())->actual_endpoint(), blob_schema(4), "aligned",
                                    /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    constexpr int kN = 3;
    for (int i = 0; i < kN; ++i)
    {
        void* buf = push->write_acquire(500ms);
        ASSERT_NE(buf, nullptr) << "item " << i;
        static_cast<uint8_t*>(buf)[0] = static_cast<uint8_t>(i + 10);
        push->write_commit();
    }

    for (int i = 0; i < kN; ++i)
    {
        const void* rbuf = pull->read_acquire(2000ms);
        ASSERT_NE(rbuf, nullptr) << "item " << i << " not received";
        EXPECT_EQ(static_cast<const uint8_t*>(rbuf)[0], static_cast<uint8_t>(i + 10))
            << "item " << i << " value mismatch";
        pull->read_release();
    }

    EXPECT_EQ(push->metrics().overrun_count,          0u);
    EXPECT_EQ(push->metrics().send_drop_count,        0u);
    EXPECT_EQ(pull->metrics().recv_frame_error_count, 0u);
    EXPECT_EQ(pull->metrics().recv_gap_count,         0u);

    push->stop();
    pull->stop();
}

// ============================================================================
// Level 2 — Abstract Queue* interface (polymorphic)
// ============================================================================

TEST_F(ZmqQueueTest, AbstractQueue_Roundtrip_ViaPushPullPtr)
{
    // Verify that the Queue abstract interface (write_acquire/write_commit,
    // read_acquire/read_release) works correctly through Queue* base pointers.
    std::unique_ptr<QueueWriter> push = ZmqQueue::push_to(
        "tcp://127.0.0.1:0", blob_schema(8), "aligned",
        /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0, /*depth=*/16);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());

    // Downcast only to get actual_endpoint(); all other calls use Queue*.
    std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();
    std::unique_ptr<QueueReader> pull = ZmqQueue::pull_from(ep, blob_schema(8), "aligned",
                                                      /*bind=*/false);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    // Write via Queue*.
    void* wbuf = push->write_acquire(500ms);
    ASSERT_NE(wbuf, nullptr);
    static_cast<uint8_t*>(wbuf)[0] = 0xAB;
    static_cast<uint8_t*>(wbuf)[7] = 0xCD;
    push->write_commit();

    // Read via Queue*.
    const void* rbuf = pull->read_acquire(2000ms);
    ASSERT_NE(rbuf, nullptr) << "item not received via abstract Queue*";
    EXPECT_EQ(static_cast<const uint8_t*>(rbuf)[0], 0xAB);
    EXPECT_EQ(static_cast<const uint8_t*>(rbuf)[7], 0xCD);
    pull->read_release();

    // item_size() / name() / is_running() via Queue*.
    EXPECT_GT(push->item_size(), 0u);
    EXPECT_FALSE(push->name().empty());
    EXPECT_TRUE(push->is_running());

    push->stop();
    pull->stop();
    EXPECT_FALSE(push->is_running());
}

TEST_F(ZmqQueueTest, AbstractQueue_Metrics_ViaBasePointer)
{
    // metrics() called through Queue* must return correct values.
    std::unique_ptr<QueueWriter> push = ZmqQueue::push_to(
        schema_ep(15), blob_schema(4), "aligned",
        /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0, /*depth=*/1, OverflowPolicy::Drop);
    ASSERT_NE(push, nullptr);

    // Trigger 2 overruns via Queue* interface (buffer depth=1, no start()).
    push->write_acquire(0ms);
    push->write_commit(); // fill the only slot

    push->write_acquire(0ms); // overflow 1
    push->write_acquire(0ms); // overflow 2

    // Read metrics via Queue* base pointer (virtual dispatch).
    const QueueMetrics m = push->metrics();
    EXPECT_EQ(m.overrun_count,          2u);
    EXPECT_EQ(m.send_drop_count,        0u);
    EXPECT_EQ(m.recv_frame_error_count, 0u);
    EXPECT_EQ(m.recv_gap_count,         0u);
    EXPECT_EQ(m.send_retry_count,       0u);
}

TEST_F(ZmqQueueTest, AbstractQueue_Flexzone_AlwaysNull)
{
    // ZmqQueue has no flexzone — inherited defaults must return nullptr/0.
    std::unique_ptr<QueueWriter> push = ZmqQueue::push_to(schema_ep(16), blob_schema(4), "aligned");
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->write_flexzone(), nullptr);
    EXPECT_EQ(push->flexzone_size(),  0u);

    std::unique_ptr<QueueReader> pull = ZmqQueue::pull_from(
        schema_ep(16), blob_schema(4), "aligned", /*bind=*/true);
    ASSERT_NE(pull, nullptr);
    EXPECT_EQ(pull->read_flexzone(), nullptr);
    EXPECT_EQ(pull->flexzone_size(), 0u);
}

// ── Concurrent metrics() reads during active I/O (race safety) ───────────────

TEST_F(ZmqQueueTest, ConcurrentMetrics_NoDataRaceUnderIO)
{
    // Spawn a reader thread that repeatedly calls metrics() while the main thread
    // fills the push queue and triggers overruns.
    // All metric counters are std::atomic so this must not data-race (TSAN clean).
    auto push = ZmqQueue::push_to(schema_ep(17), blob_schema(8), "aligned",
                                  /*bind=*/true, /*tag=*/std::nullopt, /*sndhwm=*/0,
                                  /*depth=*/2, OverflowPolicy::Drop);
    ASSERT_NE(push, nullptr);

    std::atomic<bool> stop_flag{false};
    std::atomic<int>  reads_done{0};

    // Reader thread: repeatedly read metrics() via base Queue* pointer.
    QueueWriter* qptr = push.get();
    std::thread reader([&] {
        while (!stop_flag.load(std::memory_order_relaxed))
        {
            QueueMetrics m = qptr->metrics();
            (void)m.overrun_count; // access each field to exercise all atomic loads
            (void)m.send_drop_count;
            (void)m.recv_overflow_count;
            ++reads_done;
            std::this_thread::sleep_for(1ms);
        }
    });

    // Wait until reader has run at least once before starting the writer.
    while (reads_done.load(std::memory_order_relaxed) == 0)
        std::this_thread::sleep_for(1ms);

    // Main thread: rapidly alternates acquire/commit and overflow, saturating the ring.
    for (int i = 0; i < 200; ++i)
    {
        void* b = push->write_acquire(0ms);
        if (b) push->write_commit();
    }

    stop_flag.store(true, std::memory_order_release);
    reader.join();

    EXPECT_GT(reads_done.load(), 0) << "reader thread must have executed";
    // Some items must have overflowed (depth=2, 200 commits, no send_thread_).
    EXPECT_GT(push->metrics().overrun_count, 0u);
    EXPECT_EQ(push->metrics().overrun_count, push->metrics().overrun_count);
}

// ── Directional field zero-guarantee ─────────────────────────────────────────

TEST_F(ZmqQueueTest, PullQueue_SendFieldsAreZero)
{
    // A PULL (read-mode) queue never uses send paths — those metrics stay 0.
    auto pull = ZmqQueue::pull_from(schema_ep(18), blob_schema(4), "aligned", /*bind=*/true);
    ASSERT_NE(pull, nullptr);

    const QueueMetrics m = pull->metrics();
    EXPECT_EQ(m.send_drop_count,  0u);
    EXPECT_EQ(m.send_retry_count, 0u);
    EXPECT_EQ(m.overrun_count,    0u);
    EXPECT_EQ(pull->metrics().send_drop_count,  0u);
    EXPECT_EQ(pull->metrics().send_retry_count, 0u);
    EXPECT_EQ(pull->metrics().overrun_count,    0u);
}

TEST_F(ZmqQueueTest, PushQueue_RecvFieldsAreZero)
{
    // A PUSH (write-mode) queue never uses recv paths — those metrics stay 0.
    auto push = ZmqQueue::push_to(schema_ep(18), blob_schema(4), "aligned");
    ASSERT_NE(push, nullptr);

    const QueueMetrics m = push->metrics();
    EXPECT_EQ(m.recv_overflow_count,    0u);
    EXPECT_EQ(m.recv_frame_error_count, 0u);
    EXPECT_EQ(m.recv_gap_count,         0u);
    EXPECT_EQ(push->metrics().recv_overflow_count,    0u);
    EXPECT_EQ(push->metrics().recv_frame_error_count, 0u);
    EXPECT_EQ(push->metrics().recv_gap_count,         0u);
}

// ============================================================================
// Error code path tests — factory/lifecycle/wrong-mode guard paths
// ============================================================================

TEST_F(ZmqQueueTest, Schema_InvalidTypeStr_ReturnsNull)
{
    // Factories must return nullptr if any ZmqSchemaField has an unrecognised type_str.
    // This covers the ZQ1 validation path (factory-time type check).
    auto push = ZmqQueue::push_to(schema_ep(19), {{"invalid_type", 1, 0}}, "aligned");
    EXPECT_EQ(push, nullptr) << "push_to: invalid type_str must return nullptr";

    auto pull = ZmqQueue::pull_from(schema_ep(19), {{"invalid_type", 1, 0}}, "aligned", true);
    EXPECT_EQ(pull, nullptr) << "pull_from: invalid type_str must return nullptr";
}

TEST_F(ZmqQueueTest, DoubleStart_IsIdempotent)
{
    // Calling start() on an already-running queue must return true (idempotent).
    // The queue must remain running and usable.
    auto q = ZmqQueue::pull_from("tcp://127.0.0.1:0", blob_schema(8), "aligned", /*bind=*/true);
    ASSERT_NE(q, nullptr);

    ASSERT_TRUE(q->start());
    EXPECT_TRUE(q->is_running());

    // Second start() on running queue returns true without side effects.
    EXPECT_TRUE(q->start()) << "start() on running queue must return true (idempotent)";
    EXPECT_TRUE(q->is_running()) << "queue must remain running after idempotent start()";

    q->stop();
    EXPECT_FALSE(q->is_running());
}

TEST_F(ZmqQueueTest, WriteAcquire_OnReadModeQueue_ReturnsNull)
{
    // A PULL (read-mode) queue has no write buffer — write_acquire() must return nullptr.
    // Verifies wrong-mode guard path in write_acquire().
    auto pull = ZmqQueue::pull_from(schema_ep(20), blob_schema(8), "aligned", /*bind=*/true);
    ASSERT_NE(pull, nullptr);
    auto* zpull = static_cast<ZmqQueue*>(pull.get());

    void* buf = zpull->write_acquire(0ms);
    EXPECT_EQ(buf, nullptr) << "write_acquire() on PULL queue must return nullptr";

    // commit/discard on nullptr acquire must not crash.
    EXPECT_NO_THROW(zpull->write_commit());
    EXPECT_NO_THROW(zpull->write_discard());
}

TEST_F(ZmqQueueTest, ReadAcquire_OnWriteModeQueue_ReturnsNull)
{
    // A PUSH (write-mode) queue has no recv ring — read_acquire() must return nullptr.
    // Verifies wrong-mode guard path in read_acquire().
    auto push = ZmqQueue::push_to(schema_ep(20), blob_schema(8), "aligned");
    ASSERT_NE(push, nullptr);
    auto* zpush = static_cast<ZmqQueue*>(push.get());

    const void* buf = zpush->read_acquire(0ms);
    EXPECT_EQ(buf, nullptr) << "read_acquire() on PUSH queue must return nullptr";

    // release on nullptr acquire must not crash.
    EXPECT_NO_THROW(zpush->read_release());
}

TEST_F(ZmqQueueTest, ActualEndpoint_BeforeStart_ReturnsConfiguredEndpoint)
{
    // actual_endpoint() before start() must return the configured endpoint string.
    // This covers the fallback path: pImpl->actual_endpoint.empty() → pImpl->endpoint.
    // After start() with port 0, it returns the OS-assigned port instead.
    const std::string ep = "tcp://127.0.0.1:0";
    auto push = ZmqQueue::push_to(ep, blob_schema(8), "aligned", /*bind=*/true);
    ASSERT_NE(push, nullptr);
    EXPECT_FALSE(push->is_running());

    // Before start(), actual_endpoint() returns the configured endpoint.
    EXPECT_EQ(static_cast<ZmqQueue*>(push.get())->actual_endpoint(), ep);

    // After start(), actual_endpoint() returns the OS-assigned port (differs from ":0").
    ASSERT_TRUE(push->start());
    const std::string bound_ep = static_cast<ZmqQueue *>(push.get())->actual_endpoint();
    EXPECT_NE(bound_ep, ep) << "After start(), actual_endpoint() should return the OS-assigned port";
    EXPECT_TRUE(bound_ep.find("tcp://127.0.0.1:") == 0) << "Bound endpoint should be tcp://127.0.0.1:<port>";
    // Verify the assigned port is a valid non-zero port.
    const int port = std::stoi(bound_ep.substr(std::string("tcp://127.0.0.1:").size()));
    EXPECT_GT(port, 0) << "OS-assigned port must be non-zero";
    push->stop();
}

// ============================================================================
// J2 — Schema tag mismatch increments recv_frame_error_count
// ============================================================================

TEST_F(ZmqQueueTest, SchemaMismatch_TagMismatch_IncrementsFrameError)
{
    // PUSH sends with tag A; PULL is configured with tag B.
    // Receiver must reject every frame and count it in recv_frame_error_count().
    const auto tag_a = make_tag(0x0102030405060708ULL);
    const auto tag_b = make_tag(0xAABBCCDDEEFF0011ULL);

    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", blob_schema(8), "aligned",
                                  /*bind=*/true, /*tag=*/tag_a);
    ASSERT_NE(push, nullptr);
    ASSERT_TRUE(push->start());
    const std::string ep = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    auto pull = ZmqQueue::pull_from(ep, blob_schema(8), "aligned",
                                    /*bind=*/false, /*max_buffer_depth=*/8,
                                    /*schema_tag=*/tag_b);
    ASSERT_NE(pull, nullptr);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(500ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xAB, push->item_size());
    push->write_commit();
    std::this_thread::sleep_for(300ms);

    // Frame must be rejected — tag mismatch — no data available to read.
    EXPECT_EQ(pull->read_acquire(200ms), nullptr);
    EXPECT_GE(pull->metrics().recv_frame_error_count, 1u)
        << "Schema tag mismatch must increment recv_frame_error_count";

    push->stop();
    pull->stop();
}

// ============================================================================
// J4 — Block overflow policy: write_acquire times out when send ring is full
// ============================================================================

TEST_F(ZmqQueueTest, Block_WriteAcquire_Timeout_IncrementsOverrun)
{
    // Create a PUSH queue with depth=2 and Block policy.
    // Without start(), send_thread_ never drains — ring stays full.
    // After filling 2 slots, Block write_acquire must wait the timeout then return nullptr.
    auto push = ZmqQueue::push_to(schema_ep(23), blob_schema(8), "aligned",
                                  /*bind=*/true,
                                  /*tag=*/std::nullopt,
                                  /*sndhwm=*/0,
                                  /*send_buffer_depth=*/2,
                                  OverflowPolicy::Block,
                                  /*send_retry_interval_ms=*/10);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->metrics().overrun_count, 0u);

    // Fill the ring (predicate immediately satisfied — no waiting).
    for (int i = 0; i < 2; ++i)
    {
        void* buf = push->write_acquire(0ms);
        ASSERT_NE(buf, nullptr) << "Slot " << i << " must be available";
        push->write_commit();
    }
    EXPECT_EQ(push->metrics().overrun_count, 0u);

    // Ring is full; Block policy must block for ~100ms then return nullptr.
    const auto t0 = std::chrono::steady_clock::now();
    void* buf = push->write_acquire(100ms);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(buf, nullptr)     << "Block policy must return nullptr when send ring is full";
    EXPECT_GE(elapsed, 80ms)    << "Block policy must have waited for the timeout";
    EXPECT_EQ(push->metrics().overrun_count, 1u) << "Timeout must increment overrun_count";
}

// ============================================================================
// J6 — actual_endpoint() after bind to port 0 resolves to real port
// ============================================================================

TEST_F(ZmqQueueTest, ActualEndpoint_BindPort0_ResolvesActualPort)
{
    // Binding to port 0 requests an OS-assigned ephemeral port.
    // After start(), actual_endpoint() must return the resolved address, NOT ":0".
    auto push = ZmqQueue::push_to("tcp://127.0.0.1:0", blob_schema(8), "aligned",
                                  /*bind=*/true);
    ASSERT_NE(push, nullptr);

    // Before start: still returns the configured ":0" endpoint.
    EXPECT_EQ(static_cast<ZmqQueue*>(push.get())->actual_endpoint(), "tcp://127.0.0.1:0");

    ASSERT_TRUE(push->start());
    const std::string actual = static_cast<ZmqQueue*>(push.get())->actual_endpoint();

    // After start: must NOT be the wildcard address.
    EXPECT_NE(actual, "tcp://127.0.0.1:0")
        << "Port 0 must be resolved to an OS-assigned port after start()";
    EXPECT_FALSE(actual.empty());

    // Verify the resolved port is a real positive integer.
    const auto colon = actual.rfind(':');
    ASSERT_NE(colon, std::string::npos);
    const int port = std::stoi(actual.substr(colon + 1));
    EXPECT_GT(port, 0) << "Resolved port must be > 0; got: " << actual;

    push->stop();
}

// ============================================================================
// J8 — Natural vs packed packing produces different item_size for same schema
// ============================================================================

TEST_F(ZmqQueueTest, Packing_NaturalVsPacked_DifferentItemSize)
{
    // Schema: bool (1B, align=1) + int32 (4B, align=4).
    //   aligned: bool@0 (1B), pad 3B, int32@4 (4B), struct-pad to align=4 → 8 bytes
    //   packed:  bool@0 (1B), int32@1 (4B) → 5 bytes (no padding)
    const std::vector<ZmqSchemaField> schema = {{"bool", 1, 0}, {"int32", 1, 0}};
    const std::string ep = schema_ep(24); // endpoint unused; queues are not started

    auto aligned = ZmqQueue::pull_from(ep, schema, "aligned", /*bind=*/true);
    auto packed  = ZmqQueue::pull_from(ep, schema, "packed",  /*bind=*/true);
    ASSERT_NE(aligned, nullptr);
    ASSERT_NE(packed,  nullptr);

    EXPECT_EQ(aligned->item_size(), 8u)
        << "aligned packing: bool(1)+pad(3)+int32(4) = 8 bytes";
    EXPECT_EQ(packed->item_size(),  5u)
        << "packed packing:  bool(1)+int32(4) = 5 bytes";
    EXPECT_NE(aligned->item_size(), packed->item_size())
        << "aligned and packed must produce different item sizes for bool+int32";
}
