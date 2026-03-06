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
 *   - Port numbers derived from PID to avoid ctest -j collisions
 */
#include "plh_service.hpp"
#include "utils/hub_zmq_queue.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace pylabhub::hub;
using namespace std::chrono_literals;

namespace
{

/// Return a tcp endpoint with a port derived from PID + offset.
/// Range: 40000 + (pid % 5000) * 5 + offset — ensures 5 ports per PID slice.
// Port formula: map PID into a wide range of high-numbered ports (40000–64990).
// Ports above 40000 are rarely used by system services. The *5 spacing means two
// processes whose PIDs differ by 5000 would need the same offset to collide —
// unlikely in a short CI window.  TIME_WAIT issues are avoided because the range
// is broad enough that re-using the exact same port in a 60-second window is rare.
std::string test_endpoint(int offset = 0)
{
    int base_port = 40000 + static_cast<int>(getpid() % 5000) * 5 + offset;
    return "tcp://127.0.0.1:" + std::to_string(base_port);
}

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

/// Separate port range for schema-mode tests: 48000 + (pid % 2000) * 12 + offset.
std::string schema_ep(int offset = 0)
{
    int base_port = 48000 + static_cast<int>(getpid() % 2000) * 12 + offset;
    return "tcp://127.0.0.1:" + std::to_string(base_port);
}

/// Port range for data-integrity size tests (20 offsets, 0–19).
/// Formula: 33000 + (pid % 1600) * 20 + offset → max port 64999.
std::string data_ep(int offset = 0)
{
    int base_port = 33000 + static_cast<int>(getpid() % 1600) * 20 + offset;
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
            pylabhub::utils::MakeModDefList(pylabhub::utils::Logger::GetLifecycleModule()));
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
    auto q = ZmqQueue::pull_from(test_endpoint(0), blob_schema(kItemSize), "natural", /*bind=*/true, 100);
    ASSERT_NE(q, nullptr);
    EXPECT_FALSE(q->is_running());
}

TEST_F(ZmqQueueTest, PushTo_Creates)
{
    auto q = ZmqQueue::push_to(test_endpoint(0), blob_schema(kItemSize), "natural", /*bind=*/false);
    ASSERT_NE(q, nullptr);
    EXPECT_FALSE(q->is_running());
}

// ============================================================================
// Lifecycle tests
// ============================================================================

TEST_F(ZmqQueueTest, Start_SetsRunning)
{
    auto q = ZmqQueue::pull_from(test_endpoint(0), blob_schema(kItemSize), "natural", /*bind=*/true);
    ASSERT_TRUE(q->start());
    std::this_thread::sleep_for(50ms); // recv_thread_ startup
    EXPECT_TRUE(q->is_running());
    q->stop();
}

TEST_F(ZmqQueueTest, Stop_ClearsRunning)
{
    // Use offset 10 (unique port not shared with any other test). [ZQ port-fix]
    auto q = ZmqQueue::pull_from(test_endpoint(10), blob_schema(kItemSize), "natural", /*bind=*/true);
    ASSERT_TRUE(q->start());
    q->stop();
    EXPECT_FALSE(q->is_running());
}

TEST_F(ZmqQueueTest, DoubleStop_NoThrow)
{
    auto q = ZmqQueue::pull_from(test_endpoint(2), blob_schema(kItemSize), "natural", /*bind=*/true);
    ASSERT_TRUE(q->start());
    q->stop();
    EXPECT_NO_THROW(q->stop());
}

// ============================================================================
// Read/Write roundtrip tests
// ============================================================================

TEST_F(ZmqQueueTest, Roundtrip_SingleItem)
{
    const std::string ep = test_endpoint(0);

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
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
    const std::string ep = test_endpoint(1);

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

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
    const std::string ep = test_endpoint(2);
    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true);
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
    const std::string ep = test_endpoint(0);

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

    // Acquire then abort — no message should be sent
    void *wbuf = push->write_acquire(1000ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xFF, kItemSize);
    push->write_abort();

    // Reader should timeout
    const void *rbuf = pull->read_acquire(500ms);
    EXPECT_EQ(rbuf, nullptr) << "Expected nullptr — aborted write should not be received";

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, ItemSize_Correct)
{
    auto q = ZmqQueue::pull_from(test_endpoint(0), blob_schema(kItemSize), "natural", /*bind=*/true);
    EXPECT_EQ(q->item_size(), kItemSize);
}

// ============================================================================
// Metadata tests
// ============================================================================

TEST_F(ZmqQueueTest, Name_ReturnsEndpoint)
{
    const std::string ep = test_endpoint(0);
    auto q = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true);
    EXPECT_EQ(q->name(), ep);
}

// ============================================================================
// Buffer overflow tests
// ============================================================================

TEST_F(ZmqQueueTest, PullFrom_BufferFull_DropsOldest)
{
    // Push many items into a small-buffer PULL queue; reader only gets latest items.
    const std::string ep = test_endpoint(3);
    constexpr size_t kBufDepth = 4;

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true, kBufDepth);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

    // Push 20 items — buffer can only hold kBufDepth
    for (int i = 0; i < 20; ++i)
    {
        void *wbuf = push->write_acquire(1000ms);
        ASSERT_NE(wbuf, nullptr);
        std::memset(wbuf, static_cast<uint8_t>(i + 1), kItemSize);
        push->write_commit();
    }

    // Allow recv_thread_ to process all messages
    std::this_thread::sleep_for(500ms);

    // Should be able to read up to kBufDepth items
    int read_count = 0;
    for (size_t i = 0; i < kBufDepth + 2; ++i)
    {
        const void *rbuf = pull->read_acquire(200ms);
        if (rbuf == nullptr)
            break;
        pull->read_release();
        ++read_count;
    }

    EXPECT_LE(static_cast<size_t>(read_count), kBufDepth)
        << "Should not read more than buffer depth";
    EXPECT_GT(read_count, 0) << "Should read at least one item";

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, PullFrom_BufferFull_NoDeadlock)
{
    // Push rapidly with small buffer; verify no hang after 2s.
    const std::string ep = test_endpoint(4);
    constexpr size_t kBufDepth = 2;

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true, kBufDepth);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

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

TEST_F(ZmqQueueTest, ItemSize_PreservedExact_Small)
{
    // item_size=7 → stored as 7 (no rounding; msgpack handles types)
    auto q = ZmqQueue::pull_from(test_endpoint(5), blob_schema(7), "natural", /*bind=*/true);
    EXPECT_EQ(q->item_size(), 7u);
}

TEST_F(ZmqQueueTest, ItemSize_PreservedExact_AlreadyMultipleOf8)
{
    // item_size=64 → stored as 64
    auto q = ZmqQueue::pull_from(test_endpoint(5), blob_schema(64), "natural", /*bind=*/true);
    EXPECT_EQ(q->item_size(), 64u);
}

TEST_F(ZmqQueueTest, ItemSize_PreservedExact_OddSize)
{
    // item_size=9 → stored as 9 (exact, no padding)
    auto q = ZmqQueue::pull_from(test_endpoint(5), blob_schema(9), "natural", /*bind=*/true);
    EXPECT_EQ(q->item_size(), 9u);
}

// ============================================================================
// Schema tag validation tests
// ============================================================================

TEST_F(ZmqQueueTest, SchemaTag_Match_DeliversItem)
{
    // PUSH and PULL configured with the SAME schema tag → items are delivered.
    const std::string ep = test_endpoint(5);
    auto tag = make_tag(0xDEADBEEFCAFEBABEull);

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true, 64, tag);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false, tag);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

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
    EXPECT_EQ(pull->recv_frame_error_count(), 0u);

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, SchemaTag_Mismatch_DropsAndCountsErrors)
{
    // PUSH uses tag A, PULL expects tag B → frames are rejected; none delivered.
    const std::string ep = test_endpoint(6);
    auto tag_a = make_tag(0x1111111111111111ull);
    auto tag_b = make_tag(0x2222222222222222ull);

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true, 64, tag_b);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false, tag_a);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

    constexpr int kCount = 5;
    for (int i = 0; i < kCount; ++i)
    {
        void *wbuf = push->write_acquire(500ms);
        ASSERT_NE(wbuf, nullptr);
        std::memset(wbuf, static_cast<uint8_t>(i + 1), kItemSize);
        push->write_commit();
    }

    // Give recv_thread_ time to process all frames.
    std::this_thread::sleep_for(500ms);

    // No items should reach the read buffer.
    const void *rbuf = pull->read_acquire(200ms);
    EXPECT_EQ(rbuf, nullptr) << "Schema mismatch: no items should be delivered";

    EXPECT_GE(pull->recv_frame_error_count(), static_cast<uint64_t>(kCount))
        << "Each rejected frame should be counted as a frame error";

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, SchemaTag_NoTag_AcceptsAnyFrame)
{
    // PUSH has a tag but PULL has NO tag (nullopt) → PULL accepts any frame.
    const std::string ep = test_endpoint(7);
    auto tag = make_tag(0xAAAABBBBCCCCDDDDull);

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true); // no tag
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false, tag);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

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
    EXPECT_EQ(pull->recv_frame_error_count(), 0u);

    push->stop();
    pull->stop();
}

// ============================================================================
// Overflow counter tests
// ============================================================================

TEST_F(ZmqQueueTest, OverflowCounter_Increments)
{
    // Push more items than the buffer can hold; confirm overflow counter rises.
    const std::string ep = test_endpoint(8);
    constexpr size_t kBufDepth = 2;
    constexpr int kSend = 20;

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true, kBufDepth);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

    // Send many items without reading.
    for (int i = 0; i < kSend; ++i)
    {
        void *wbuf = push->write_acquire(500ms);
        if (!wbuf) break;
        std::memset(wbuf, static_cast<uint8_t>(i + 1), kItemSize);
        push->write_commit();
    }

    // Wait for recv_thread_ to process all messages.
    std::this_thread::sleep_for(500ms);

    EXPECT_GT(pull->recv_overflow_count(), 0u)
        << "Buffer overflow should have been counted";

    push->stop();
    pull->stop();
}

// ============================================================================
// Sequence number / ordering tests
// ============================================================================

TEST_F(ZmqQueueTest, MultipleItems_Ordered)
{
    // Verify that items arrive in the same order they were sent.
    const std::string ep = test_endpoint(9);

    auto pull = ZmqQueue::pull_from(ep, blob_schema(kItemSize), "natural", /*bind=*/true);
    auto push = ZmqQueue::push_to(ep, blob_schema(kItemSize), "natural", /*bind=*/false);

    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);

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
    EXPECT_EQ(pull->recv_frame_error_count(), 0u);

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
    auto q = ZmqQueue::pull_from(schema_ep(0), schema, "natural", /*bind=*/true);
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
    auto q = ZmqQueue::pull_from(schema_ep(0), schema, "natural", /*bind=*/true);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->item_size(), 32u);
}

TEST_F(ZmqQueueTest, Schema_ItemSize_StringField)
{
    // {string(length=8)}: char[8]@0 → total 8.
    std::vector<ZmqSchemaField> schema = {{"string", 1, 8}};
    auto q = ZmqQueue::pull_from(schema_ep(0), schema, "natural", /*bind=*/true);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->item_size(), 8u);
}

// ── scalar roundtrip ─────────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_Scalars_Roundtrip)
{
    // {int32, float64} natural: int32@0, float64@8.
    // Scalars encoded as native msgpack types — exact values preserved.
    std::vector<ZmqSchemaField> schema = {{"int32", 1, 0}, {"float64", 1, 0}};
    const std::string ep = schema_ep(1);

    auto push = ZmqQueue::push_to(ep, schema, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, schema, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    EXPECT_EQ(push->item_size(), 16u);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
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

    EXPECT_EQ(pull->recv_frame_error_count(), 0u);
    push->stop();
    pull->stop();
}

// ── array roundtrip ──────────────────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_Array_Roundtrip)
{
    // {float64[4]}: array → bin(32), size-validated on recv.
    std::vector<ZmqSchemaField> schema = {{"float64", 4, 0}};
    const std::string ep = schema_ep(2);

    auto push = ZmqQueue::push_to(ep, schema, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, schema, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
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

    EXPECT_EQ(pull->recv_frame_error_count(), 0u);
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
    const std::string ep = schema_ep(3);

    auto push = ZmqQueue::push_to(ep, schema, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, schema, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    EXPECT_EQ(push->item_size(), 48u);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
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

    EXPECT_EQ(pull->recv_frame_error_count(), 0u);
    push->stop();
    pull->stop();
}

// ── type-safety rejection tests ──────────────────────────────────────────────

TEST_F(ZmqQueueTest, Schema_EmptySchema_ReturnsNullptr)
{
    // Empty schema is an error — both factories must return nullptr.
    auto pull = ZmqQueue::pull_from(schema_ep(4), {}, "natural", /*bind=*/true);
    EXPECT_EQ(pull, nullptr) << "pull_from with empty schema must return nullptr";

    auto push = ZmqQueue::push_to(schema_ep(4), {}, "natural", /*bind=*/true);
    EXPECT_EQ(push, nullptr) << "push_to with empty schema must return nullptr";
}

TEST_F(ZmqQueueTest, Schema_FieldCountMismatch_Rejected)
{
    // Sender has 2 fields, receiver expects 1 → array length mismatch → rejected.
    const std::string ep = schema_ep(5);
    std::vector<ZmqSchemaField> ss = {{"int32", 1, 0}, {"float64", 1, 0}};
    std::vector<ZmqSchemaField> sr = {{"int32", 1, 0}};

    auto push = ZmqQueue::push_to(ep, ss, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, sr, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(500ms);
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0, push->item_size());
    push->write_commit();
    std::this_thread::sleep_for(300ms);

    EXPECT_EQ(pull->read_acquire(200ms), nullptr) << "Field count mismatch must be rejected";
    EXPECT_GE(pull->recv_frame_error_count(), 1u);

    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Schema_ArraySizeMismatch_Rejected)
{
    // Sender encodes float64[4] (bin 32 B), receiver expects float64[3] (bin 24 B).
    // Bin size mismatch → rejected.
    const std::string ep = schema_ep(6);
    std::vector<ZmqSchemaField> ss = {{"float64", 4, 0}};
    std::vector<ZmqSchemaField> sr = {{"float64", 3, 0}};

    auto push = ZmqQueue::push_to(ep, ss, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, sr, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(pull->start());
    std::this_thread::sleep_for(50ms);

    void* wbuf = push->write_acquire(500ms);
    ASSERT_NE(wbuf, nullptr);
    const double vals[4] = {1.0, 2.0, 3.0, 4.0};
    std::memcpy(wbuf, vals, 32);
    push->write_commit();
    std::this_thread::sleep_for(300ms);

    EXPECT_EQ(pull->read_acquire(200ms), nullptr) << "Array size mismatch must be rejected";
    EXPECT_GE(pull->recv_frame_error_count(), 1u);

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
    // page size (4095/4097), and 4x-page (16384).  Each uses a distinct port offset to
    // avoid TIME_WAIT collisions between consecutive subtests.
    struct Case { size_t sz; int ep_offset; };
    const Case cases[] = {
        {1,     0}, {3,     1}, {5,     2}, {7,     3},
        {9,     4}, {11,    5}, {17,    6}, {63,    7},
        {65,    8}, {127,   9}, {128,  10}, {129,  11},
        {255,  12}, {256,  13}, {1023, 14}, {1024, 15},
        {4095, 16}, {4096, 17}, {4097, 18}, {16384, 19},
    };

    for (const auto& c : cases)
    {
        SCOPED_TRACE("size=" + std::to_string(c.sz));
        const std::string ep = data_ep(c.ep_offset);

        auto push = ZmqQueue::push_to(ep, blob_schema(c.sz), "natural", /*bind=*/true);
        auto pull = ZmqQueue::pull_from(ep, blob_schema(c.sz), "natural", /*bind=*/false);
        ASSERT_NE(push, nullptr) << "push_to nullptr for size=" << c.sz;
        ASSERT_NE(pull, nullptr) << "pull_from nullptr for size=" << c.sz;
        ASSERT_EQ(push->item_size(), c.sz);
        ASSERT_EQ(pull->item_size(), c.sz);

        ASSERT_TRUE(push->start());
        std::this_thread::sleep_for(50ms);
        ASSERT_TRUE(pull->start());
        std::this_thread::sleep_for(50ms);

        // Fill with deterministic byte pattern.
        void* wbuf = push->write_acquire(1000ms);
        ASSERT_NE(wbuf, nullptr);
        auto* wb = static_cast<uint8_t*>(wbuf);
        for (size_t i = 0; i < c.sz; ++i)
            wb[i] = data_pattern(i);
        push->write_commit();

        const void* rbuf = pull->read_acquire(2000ms);
        ASSERT_NE(rbuf, nullptr) << "Timed out for size=" << c.sz;

        // Verify every byte survived the msgpack roundtrip.
        const auto* rb = static_cast<const uint8_t*>(rbuf);
        for (size_t i = 0; i < c.sz; ++i)
        {
            ASSERT_EQ(rb[i], data_pattern(i))
                << "Corruption at byte=" << i << " size=" << c.sz;
        }
        pull->read_release();
        EXPECT_EQ(pull->recv_frame_error_count(), 0u);

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
    const std::string ep = schema_ep(7);

    auto push = ZmqQueue::push_to(ep, schema, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, schema, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    EXPECT_EQ(push->item_size(), 48u);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
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

    EXPECT_EQ(pull->recv_frame_error_count(), 0u);
    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Schema_LargeArrayOverPageSize_Roundtrip)
{
    // {float64[600]} = 4800 bytes — well over one 4096-byte page.
    // Tests that ring buffer allocation, frame buffer sizing, and msgpack
    // bin encode/decode all handle multi-page payloads correctly.
    std::vector<ZmqSchemaField> schema = {{"float64", 600, 0}};
    const std::string ep = schema_ep(8);

    auto push = ZmqQueue::push_to(ep, schema, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, schema, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    EXPECT_EQ(push->item_size(), 600u * 8u);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
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

    EXPECT_EQ(pull->recv_frame_error_count(), 0u);
    push->stop();
    pull->stop();
}

TEST_F(ZmqQueueTest, Schema_AlignmentPadding_FieldsPreserved)
{
    // {uint8, float64} natural: uint8@0(1), 7 pad bytes, float64@8(8) → 16 bytes.
    // Verifies that a 7-byte alignment gap between two fields doesn't corrupt
    // either value in the msgpack encode/decode roundtrip.
    std::vector<ZmqSchemaField> schema = {{"uint8", 1, 0}, {"float64", 1, 0}};
    const std::string ep = schema_ep(9);

    auto push = ZmqQueue::push_to(ep, schema, "natural", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, schema, "natural", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    EXPECT_EQ(push->item_size(), 16u);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
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

    EXPECT_EQ(pull->recv_frame_error_count(), 0u);
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
    const std::string ep = schema_ep(10);

    auto push = ZmqQueue::push_to(ep, schema, "packed", /*bind=*/true);
    auto pull = ZmqQueue::pull_from(ep, schema, "packed", /*bind=*/false);
    ASSERT_NE(push, nullptr);
    ASSERT_NE(pull, nullptr);
    EXPECT_EQ(push->item_size(), 460u);

    ASSERT_TRUE(push->start());
    std::this_thread::sleep_for(50ms);
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

    EXPECT_EQ(pull->recv_frame_error_count(), 0u);
    push->stop();
    pull->stop();
}
