/**
 * @file test_datahub_hub_inbox_queue.cpp
 * @brief Unit tests for hub::InboxQueue (ROUTER receiver) and hub::InboxClient (DEALER sender).
 *
 * All tests use port-0 bind: InboxQueue binds "tcp://127.0.0.1:0", calls actual_endpoint()
 * after start(), then InboxClient connects to that endpoint.
 *
 * These tests exercise the same thread model used in production:
 *   - InboxQueue: single recv_one() / send_ack() caller thread
 *   - InboxClient: single acquire() / send() caller thread
 */
#include "utils/hub_inbox_queue.hpp"
#include "utils/crypto_utils.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>
#include <zmq.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

using namespace pylabhub::hub;
using ms = std::chrono::milliseconds;

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture: owns the process-wide LifecycleGuard for the suite so that
// InboxQueue/InboxClient can call pylabhub::hub::get_zmq_context(). Mirrors
// ZmqQueueTest::SetUpTestSuite.
// ─────────────────────────────────────────────────────────────────────────────
class InboxQueueTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::crypto::GetLifecycleModule(),
                pylabhub::hub::GetZMQContextModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

  private:
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> InboxQueueTest::s_lifecycle_;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: minimal schema with one uint32 field
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<ZmqSchemaField> uint32_schema()
{
    return {{"uint32", 1, 0}};
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.BindAndConnect_Basic
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, BindAndConnect_Basic)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    const std::string ep = q->actual_endpoint();
    EXPECT_FALSE(ep.empty());
    EXPECT_NE(ep.find("tcp://"), std::string::npos);

    auto c = InboxClient::connect_to(ep, "PROD-TEST-00000001", uint32_schema());
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->start());

    // acquire → send → recv → ack
    void* buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    uint32_t val = 0xDEADBEEF;
    std::memcpy(buf, &val, sizeof(val));

    // recv in background; send_ack inside the future so c->send() can receive the ACK
    const InboxItem* item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{2000});
        if (item) q->send_ack(0);
        return item != nullptr;
    });

    std::this_thread::sleep_for(ms{30});
    uint8_t ack = c->send(ms{1500});

    ASSERT_TRUE(fut.get()) << "recv_one timed out";
    ASSERT_NE(item, nullptr);
    ASSERT_NE(item->data, nullptr);

    uint32_t received = 0;
    std::memcpy(&received, item->data, sizeof(received));
    EXPECT_EQ(received, val);
    EXPECT_EQ(ack, 0u);

    c->stop();
    q->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.RecvOne_Timeout_ReturnsNull
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, RecvOne_Timeout_ReturnsNull)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    const auto* item = q->recv_one(ms{50});
    EXPECT_EQ(item, nullptr);

    q->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.MultipleMessages
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, MultipleMessages)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    auto c = InboxClient::connect_to(q->actual_endpoint(), "PROD-MULTI-00000001", uint32_schema());
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->start());

    // Send 3 messages and receive all 3
    const uint32_t kValues[3] = {0x11111111, 0x22222222, 0x33333333};

    for (int i = 0; i < 3; ++i)
    {
        void* buf = c->acquire();
        ASSERT_NE(buf, nullptr);
        std::memcpy(buf, &kValues[i], sizeof(kValues[i]));

        const InboxItem* item = nullptr;
        // recv_one + send_ack in background so c->send() can receive the ACK
        auto fut = std::async(std::launch::async, [&] {
            item = q->recv_one(ms{2000});
            if (item) q->send_ack(0);
            return item != nullptr;
        });

        std::this_thread::sleep_for(ms{20});
        uint8_t ack = c->send(ms{1500});

        ASSERT_TRUE(fut.get()) << "recv_one timed out at iteration " << i;
        ASSERT_NE(item, nullptr);
        ASSERT_NE(item->data, nullptr);

        uint32_t received = 0;
        std::memcpy(&received, item->data, sizeof(received));
        EXPECT_EQ(received, kValues[i]) << "value mismatch at iteration " << i;
        EXPECT_EQ(item->seq, static_cast<uint64_t>(i));
        EXPECT_EQ(ack, 0u);
    }

    c->stop();
    q->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.DoubleStop_NoThrow
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, DoubleStop_NoThrow)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    EXPECT_NO_THROW(q->stop());
    EXPECT_NO_THROW(q->stop()); // second stop is a no-op

    auto c = InboxClient::connect_to("tcp://127.0.0.1:5599", "PROD-DBLSTOP-00000001",
                                     uint32_schema());
    ASSERT_NE(c, nullptr);
    // Don't start — just double-stop
    EXPECT_NO_THROW(c->stop());
    EXPECT_NO_THROW(c->stop());
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.SenderUid_IsPreserved
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, SenderUid_IsPreserved)
{
    const std::string kSenderId = "PROD-TEST-12345678";

    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    auto c = InboxClient::connect_to(q->actual_endpoint(), kSenderId, uint32_schema());
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->start());

    void* buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    uint32_t v = 42;
    std::memcpy(buf, &v, sizeof(v));

    const InboxItem* item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{2000});
        // send_ack must be called on the same thread as recv_one (ZMQ socket
        // thread-safety) and before c->send() returns — call it here.
        if (item != nullptr)
            q->send_ack(0);
        return item != nullptr;
    });

    std::this_thread::sleep_for(ms{30});
    c->send(ms{1500});

    ASSERT_TRUE(fut.get());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->sender_id, kSenderId);

    c->stop();
    q->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.BadMagic_Drops
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, BadMagic_Drops)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    // Use a raw ZMQ context + DEALER to send a frame with wrong magic
    void* ctx = zmq_ctx_new();
    ASSERT_NE(ctx, nullptr);

    void* sock = zmq_socket(ctx, ZMQ_DEALER);
    ASSERT_NE(sock, nullptr);

    const std::string id = "BAD-MAGIC-SENDER";
    zmq_setsockopt(sock, ZMQ_IDENTITY, id.c_str(), id.size());
    int linger = 0;
    zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));

    const std::string ep = q->actual_endpoint();
    ASSERT_EQ(zmq_connect(sock, ep.c_str()), 0);

    std::this_thread::sleep_for(ms{50}); // let connect establish

    // Send a raw byte string (not valid msgpack with correct magic)
    const char bad_payload[] = "BAAD";
    zmq_send(sock, bad_payload, sizeof(bad_payload) - 1, 0);

    // recv_one should drop this frame and return nullptr (timeout)
    const auto* item = q->recv_one(ms{200});
    EXPECT_EQ(item, nullptr);
    EXPECT_GT(q->recv_frame_error_count(), uint64_t{0});

    zmq_close(sock);
    zmq_ctx_term(ctx);
    q->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.AckCode3_HandlerError
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, AckCode3_HandlerError)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    auto c = InboxClient::connect_to(q->actual_endpoint(), "PROD-ACKERR-00000001", uint32_schema());
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->start());

    void* buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    uint32_t v = 7;
    std::memcpy(buf, &v, sizeof(v));

    // recv_one + send_ack(3) must be in the SAME thread (ZMQ socket not thread-safe).
    // send_ack(3) is called immediately after recv_one returns, while c->send() awaits the ACK.
    const InboxItem* item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{2000});
        if (item) q->send_ack(3);   // handler_error ACK — same thread as recv_one
        return item != nullptr;
    });

    std::this_thread::sleep_for(ms{30});
    uint8_t ack_code = c->send(ms{2000});

    ASSERT_TRUE(fut.get());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(ack_code, 3u);

    c->stop();
    q->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.NotStarted_RecvReturnsNull
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, NotStarted_RecvReturnsNull)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    // Never called start()
    const auto* item = q->recv_one(ms{10});
    EXPECT_EQ(item, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.EmptySchema_FactoryFails
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, EmptySchema_FactoryFails)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", {}); // empty schema
    EXPECT_EQ(q, nullptr);
}

TEST_F(InboxQueueTest, EmptySchema_ClientFactoryFails)
{
    auto c = InboxClient::connect_to("tcp://127.0.0.1:5599", "TEST-UID", {}); // empty schema
    EXPECT_EQ(c, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// InboxQueueTest.ItemSize_MatchesSchema
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(InboxQueueTest, ItemSize_MatchesSchema)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->item_size(), sizeof(uint32_t));

    auto c = InboxClient::connect_to("tcp://127.0.0.1:5599", "UID", uint32_schema());
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->item_size(), sizeof(uint32_t));
}

// ─────────────────────────────────────────────────────────────────────────────
// Checksum policy tests — symmetric with ShmQueue + ZmqQueue
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Schema mismatch tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(InboxQueueTest, SchemaMismatch_DifferentType_DropsFrame)
{
    // Receiver expects uint32, sender sends with float64 schema → schema tag mismatch.
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    // Client uses a different schema (float64 instead of uint32).
    std::vector<ZmqSchemaField> float64_schema = {{"float64", 1, 0}};
    auto c = InboxClient::connect_to(q->actual_endpoint(), "MISMATCH-01", float64_schema);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->start());

    void *buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    double val = 3.14;
    std::memcpy(buf, &val, sizeof(val));

    const InboxItem *item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{500});
        return item != nullptr;
    });
    std::this_thread::sleep_for(ms{30});
    c->send(ms{1500});

    // Frame should be dropped due to schema tag mismatch.
    EXPECT_FALSE(fut.get()) << "Schema mismatch: recv_one should reject";
    EXPECT_GT(q->recv_frame_error_count(), 0u)
        << "Schema mismatch should increment frame error count";

    c->stop();
    q->stop();
}

TEST_F(InboxQueueTest, SchemaMismatch_DifferentSize_DropsFrame)
{
    // Receiver expects uint32 (4 bytes), sender sends with uint64 (8 bytes) → size mismatch.
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->start());

    std::vector<ZmqSchemaField> uint64_schema = {{"uint64", 1, 0}};
    auto c = InboxClient::connect_to(q->actual_endpoint(), "MISMATCH-02", uint64_schema);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->start());

    void *buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    uint64_t val = 0xDEADBEEF;
    std::memcpy(buf, &val, sizeof(val));

    const InboxItem *item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{500});
        return item != nullptr;
    });
    std::this_thread::sleep_for(ms{30});
    c->send(ms{1500});

    // Frame should be dropped — schema tag differs (uint32 vs uint64 produce different hashes).
    EXPECT_FALSE(fut.get()) << "Size mismatch: recv_one should reject";
    EXPECT_GT(q->recv_frame_error_count(), 0u);

    c->stop();
    q->stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Checksum policy tests — symmetric with ShmQueue + ZmqQueue
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(InboxQueueTest, ChecksumEnforced_Roundtrip)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    q->set_checksum_policy(ChecksumPolicy::Enforced);
    ASSERT_TRUE(q->start());

    auto c = InboxClient::connect_to(q->actual_endpoint(), "CKSUM-ENF", uint32_schema());
    ASSERT_NE(c, nullptr);
    c->set_checksum_policy(ChecksumPolicy::Enforced);
    ASSERT_TRUE(c->start());

    void *buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    uint32_t val = 0xCAFE;
    std::memcpy(buf, &val, sizeof(val));

    const InboxItem *item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{2000});
        if (item) q->send_ack(0);
        return item != nullptr;
    });
    std::this_thread::sleep_for(ms{30});
    c->send(ms{1500});

    ASSERT_TRUE(fut.get()) << "Enforced: recv_one should succeed";
    ASSERT_NE(item, nullptr);
    uint32_t received = 0;
    std::memcpy(&received, item->data, sizeof(received));
    EXPECT_EQ(received, val);
    EXPECT_EQ(q->checksum_error_count(), 0u);

    c->stop();
    q->stop();
}

TEST_F(InboxQueueTest, ChecksumManual_NoStamp_ReceiverRejects)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    q->set_checksum_policy(ChecksumPolicy::Enforced); // receiver verifies
    ASSERT_TRUE(q->start());

    auto c = InboxClient::connect_to(q->actual_endpoint(), "CKSUM-MAN", uint32_schema());
    ASSERT_NE(c, nullptr);
    c->set_checksum_policy(ChecksumPolicy::Manual); // sender does NOT auto-stamp
    ASSERT_TRUE(c->start());

    void *buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    uint32_t val = 0xDEAD;
    std::memcpy(buf, &val, sizeof(val));

    const InboxItem *item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{500});
        return item != nullptr;
    });
    std::this_thread::sleep_for(ms{30});
    c->send(ms{1500});

    EXPECT_FALSE(fut.get()) << "Manual no-stamp: recv_one should reject";
    EXPECT_GT(q->checksum_error_count(), 0u);

    c->stop();
    q->stop();
}

TEST_F(InboxQueueTest, ChecksumNone_Roundtrip)
{
    auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
    ASSERT_NE(q, nullptr);
    q->set_checksum_policy(ChecksumPolicy::None);
    ASSERT_TRUE(q->start());

    auto c = InboxClient::connect_to(q->actual_endpoint(), "CKSUM-NONE", uint32_schema());
    ASSERT_NE(c, nullptr);
    c->set_checksum_policy(ChecksumPolicy::None);
    ASSERT_TRUE(c->start());

    void *buf = c->acquire();
    ASSERT_NE(buf, nullptr);
    uint32_t val = 0xF00D;
    std::memcpy(buf, &val, sizeof(val));

    const InboxItem *item = nullptr;
    auto fut = std::async(std::launch::async, [&] {
        item = q->recv_one(ms{2000});
        if (item) q->send_ack(0);
        return item != nullptr;
    });
    std::this_thread::sleep_for(ms{30});
    c->send(ms{1500});

    ASSERT_TRUE(fut.get()) << "None: recv_one should succeed";
    ASSERT_NE(item, nullptr);
    uint32_t received = 0;
    std::memcpy(&received, item->data, sizeof(received));
    EXPECT_EQ(received, val);
    EXPECT_EQ(q->checksum_error_count(), 0u);

    c->stop();
    q->stop();
}
