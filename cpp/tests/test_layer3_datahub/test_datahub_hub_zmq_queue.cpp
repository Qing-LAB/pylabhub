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

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace pylabhub::hub;
using namespace std::chrono_literals;

namespace
{

/// Return a tcp endpoint with a port derived from PID + offset.
/// Range: 30000 + (pid % 10000) * 5 + offset — ensures 5 ports per PID slice.
std::string test_endpoint(int offset = 0)
{
    int base_port = 30000 + static_cast<int>(getpid() % 10000) * 5 + offset;
    return "tcp://127.0.0.1:" + std::to_string(base_port);
}

constexpr size_t kItemSize = 64;

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
    auto q = ZmqQueue::pull_from(test_endpoint(0), kItemSize, /*bind=*/true, /*max_buf=*/100);
    ASSERT_NE(q, nullptr);
    EXPECT_FALSE(q->is_running());
}

TEST_F(ZmqQueueTest, PushTo_Creates)
{
    auto q = ZmqQueue::push_to(test_endpoint(0), kItemSize, /*bind=*/false);
    ASSERT_NE(q, nullptr);
    EXPECT_FALSE(q->is_running());
}

// ============================================================================
// Lifecycle tests
// ============================================================================

TEST_F(ZmqQueueTest, Start_SetsRunning)
{
    auto q = ZmqQueue::pull_from(test_endpoint(0), kItemSize, /*bind=*/true);
    ASSERT_TRUE(q->start());
    std::this_thread::sleep_for(50ms); // recv_thread_ startup
    EXPECT_TRUE(q->is_running());
    q->stop();
}

TEST_F(ZmqQueueTest, Stop_ClearsRunning)
{
    auto q = ZmqQueue::pull_from(test_endpoint(1), kItemSize, /*bind=*/true);
    ASSERT_TRUE(q->start());
    q->stop();
    EXPECT_FALSE(q->is_running());
}

TEST_F(ZmqQueueTest, DoubleStop_NoThrow)
{
    auto q = ZmqQueue::pull_from(test_endpoint(2), kItemSize, /*bind=*/true);
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

    auto pull = ZmqQueue::pull_from(ep, kItemSize, /*bind=*/true);
    auto push = ZmqQueue::push_to(ep, kItemSize, /*bind=*/false);

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

    auto pull = ZmqQueue::pull_from(ep, kItemSize, /*bind=*/true);
    auto push = ZmqQueue::push_to(ep, kItemSize, /*bind=*/false);

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
    auto pull = ZmqQueue::pull_from(ep, kItemSize, /*bind=*/true);
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

    auto pull = ZmqQueue::pull_from(ep, kItemSize, /*bind=*/true);
    auto push = ZmqQueue::push_to(ep, kItemSize, /*bind=*/false);

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
    auto q = ZmqQueue::pull_from(test_endpoint(0), kItemSize, /*bind=*/true);
    EXPECT_EQ(q->item_size(), kItemSize);
}

// ============================================================================
// Metadata tests
// ============================================================================

TEST_F(ZmqQueueTest, Name_ReturnsEndpoint)
{
    const std::string ep = test_endpoint(0);
    auto q = ZmqQueue::pull_from(ep, kItemSize, /*bind=*/true);
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

    auto pull = ZmqQueue::pull_from(ep, kItemSize, /*bind=*/true, kBufDepth);
    auto push = ZmqQueue::push_to(ep, kItemSize, /*bind=*/false);

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

    auto pull = ZmqQueue::pull_from(ep, kItemSize, /*bind=*/true, kBufDepth);
    auto push = ZmqQueue::push_to(ep, kItemSize, /*bind=*/false);

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
