/**
 * @file hub_inbox_queue_workers.cpp
 * @brief Worker bodies for `InboxQueue` + `InboxClient` tests
 *        (Phase 3 Inbox Facility; Pattern 3).
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.
 *
 * Subject: `pylabhub::hub::InboxQueue` (ROUTER receiver) and
 * `pylabhub::hub::InboxClient` (DEALER sender).  Tests use TCP
 * loopback port-0 bind for ephemeral endpoint allocation; the queue
 * exposes `actual_endpoint()` post-start for the client to connect.
 *
 * Real production wiring per feedback_test_layering_and_no_mocks.md:
 * real InboxQueue / InboxClient classes, real ZMQ context via the
 * production lifecycle module.  Test #6 (`bad_magic_drops`) uses raw
 * `zmq_ctx_new()` + `zmq_socket()` to fabricate a malformed wire
 * frame — that is NOT a mock; it is the same underlying ZMQ library
 * producing a non-production wire shape so the receiver's drop path
 * can be exercised.  Legitimate test fabrication for error-path
 * coverage.
 *
 * Body assertions transplanted verbatim, preserving the file's pins:
 *   - ack codes (0 happy / 3 handler-error / 255 timeout)
 *   - content + seq + sender_id roundtrip
 *   - recv_frame_error_count / checksum_error_count thresholds
 *   - 5 ExpectLog* declarations across the rejection paths
 *
 * The 2026-05-01 audit §1.3 "Class C silent failure" rationale at the
 * sender_id_is_preserved test (`EXPECT_EQ(ack, 0u)` instead of
 * discarding the ack) is preserved unchanged.
 *
 * Module surface: Logger + SecureSubsystem + ZMQContext (matches the
 * original SetUpTestSuite).
 *
 * @see HEP-CORE-0007 §"Inbox" (broker inbox queue wire protocol)
 */

#include "hub_inbox_queue_workers.h"

#include "log_capture_fixture.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/hub_inbox_queue.hpp"
#include "utils/logger.hpp"
#include "utils/zmq_context.hpp"
#include "utils/security/key_store.hpp"        // add_identity_from_z85, pubkey
#include "utils/security/peer_admission.hpp"   // PeerAllowlist, PeerIdentity
#include "utils/security/secure_subsystem.hpp" // secure()
#include "utils/security/zap_router.hpp"       // ZapPumpThread

#include <gtest/gtest.h>
#include <zmq.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using pylabhub::hub::ChecksumPolicy;
using pylabhub::hub::InboxClient;
using pylabhub::hub::InboxItem;
using pylabhub::hub::InboxQueue;
using pylabhub::hub::ZmqSchemaField;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;
using ms = std::chrono::milliseconds;

namespace pylabhub::tests::worker
{
namespace hub_inbox_queue
{

namespace
{

std::vector<ZmqSchemaField> uint32_schema()
{
    return {{"uint32", 1, 0}};
}

/// 3-module lifecycle list shared across every worker: Logger for
/// `LOGGER_*` macros and the LogCaptureFixture's sink redirect,
/// SecureSubsystem for hash routines underneath checksum/schema
/// fingerprinting, ZMQContext for the production ZMQ context the
/// InboxQueue/InboxClient pull via `pylabhub::hub::get_zmq_context()`.
#define PLH_INBOX_MODS                                                                             \
    Logger::GetLifecycleModule(),                                                                  \
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),                          \
        pylabhub::hub::GetZMQContextModule()

/// Generate a fresh CURVE keypair (Z85 pub, Z85 sec) for the inbox
/// CURVE-auth workers.  Mirrors the L2 zmq_queue_auth helper.
inline std::pair<std::string, std::string> make_keypair()
{
    std::array<char, 41> pub{};
    std::array<char, 41> sec{};
    if (::zmq_curve_keypair(pub.data(), sec.data()) != 0)
        throw std::runtime_error("zmq_curve_keypair failed");
    return {std::string(pub.data(), 40), std::string(sec.data(), 40)};
}

/// CURVE identities for one inbox pair.  There is no unarmed inbox: both
/// `InboxQueue::start()` and `InboxClient::start()` PANIC if no identity was
/// armed (HEP-CORE-0027 §3.5, HEP-CORE-0035 §2).  So this is not a testing
/// convenience — it is the only legal way to construct these objects, and
/// every worker below uses it.
struct InboxCurve
{
    std::string recv_pub; ///< pass to the client as curve_serverkey
    std::string send_pub; ///< seed into the queue's allowlist
    std::string send_sec; ///< only needed by workers that arm a RAW DEALER
};

/// Arm the receiving ROUTER.  MUST be called BEFORE `q.start()`.
/// @param domain  distinct ZAP domain; give each worker its own so a stale
///                registration from another test cannot satisfy this one.
inline InboxCurve arm_inbox_queue(InboxQueue &q, const char *domain)
{
    namespace sec = pylabhub::utils::security;
    InboxCurve k;
    const auto [rp, rs] = make_keypair();
    const auto [sp, ss] = make_keypair();
    k.recv_pub = rp;
    k.send_pub = sp;
    k.send_sec = ss;
    sec::secure().keys().add_identity_from_z85("inbox_recv_id", rp, rs);
    sec::secure().keys().add_identity_from_z85("inbox_send_id", sp, ss);
    q.set_curve_server_identity("inbox_recv_id", domain);
    return k;
}

/// Admit the sender and arm the dialing DEALER.  Call AFTER `q.start()`
/// (the queue binds deny-all) and BEFORE `c.start()`.  A `ZapPumpThread` must
/// be alive in the worker for the handshake to be serviced.
inline void admit_sender(InboxQueue &q, const InboxCurve &k)
{
    namespace sec = pylabhub::utils::security;
    sec::PeerAllowlist allow;
    allow.peers.insert(sec::PeerIdentity{"curve", k.send_pub});
    q.set_peer_allowlist(allow);
}

inline void admit_and_arm_client(InboxQueue &q, InboxClient &c, const InboxCurve &k)
{
    admit_sender(q, k);
    c.set_curve_client_identity("inbox_send_id", k.recv_pub);
}

} // namespace

// ─── Test #1: BindAndConnect_Basic ──────────────────────────────────────────

int bind_and_connect_basic()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            const std::string ep = q->actual_endpoint();
            EXPECT_FALSE(ep.empty());
            EXPECT_NE(ep.find("tcp://"), std::string::npos);

            auto c = InboxClient::connect_to(ep, "prod.test.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t val = 0xDEADBEEF;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{2000});
                                      if (item)
                                          q->send_ack(0);
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

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::bind_and_connect_basic", PLH_INBOX_MODS);
}

// ─── Test #2: RecvOne_Timeout_ReturnsNull ───────────────────────────────────

int recv_one_timeout_returns_null()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            const auto *item = q->recv_one(ms{50});
            EXPECT_EQ(item, nullptr);

            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::recv_one_timeout_returns_null", PLH_INBOX_MODS);
}

// ─── Test #3: MultipleMessages ──────────────────────────────────────────────

int multiple_messages()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c = InboxClient::connect_to(q->actual_endpoint(), "prod.multi.uid00000001",
                                             uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            const uint32_t kValues[3] = {0x11111111, 0x22222222, 0x33333333};

            for (int i = 0; i < 3; ++i)
            {
                void *buf = c->acquire();
                ASSERT_NE(buf, nullptr);
                std::memcpy(buf, &kValues[i], sizeof(kValues[i]));

                const InboxItem *item = nullptr;
                auto fut = std::async(std::launch::async,
                                      [&]
                                      {
                                          item = q->recv_one(ms{2000});
                                          if (item)
                                              q->send_ack(0);
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

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::multiple_messages", PLH_INBOX_MODS);
}

// ─── Test #4: DoubleStop_NoThrow ────────────────────────────────────────────

int double_stop_no_throw()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            EXPECT_NO_THROW(q->stop());
            EXPECT_NO_THROW(q->stop()); // second stop is a no-op

            auto c = InboxClient::connect_to("tcp://127.0.0.1:5599", "prod.dblstop.uid00000001",
                                             uint32_schema());
            ASSERT_NE(c, nullptr);
            EXPECT_NO_THROW(c->stop());
            EXPECT_NO_THROW(c->stop());

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::double_stop_no_throw", PLH_INBOX_MODS);
}

// ─── Test #5: SenderUid_IsPreserved ─────────────────────────────────────────

int sender_uid_is_preserved()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const std::string kSenderId = "prod.test.uid12345678";

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c = InboxClient::connect_to(q->actual_endpoint(), kSenderId, uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t v = 42;
            std::memcpy(buf, &v, sizeof(v));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{2000});
                                      if (item != nullptr)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });

            std::this_thread::sleep_for(ms{30});
            // ACK code 0 = success.  Discarding this return is a Class C
            // silent-failure per REVIEW_TestAudit_2026-05-01.md §1.3 —
            // capture and assert.
            const uint8_t ack = c->send(ms{1500});
            EXPECT_EQ(ack, 0u) << "send timed out or got non-zero ack=" << static_cast<int>(ack);

            ASSERT_TRUE(fut.get());
            ASSERT_NE(item, nullptr);
            EXPECT_EQ(item->sender_id, kSenderId);

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::sender_uid_is_preserved", PLH_INBOX_MODS);
}

// ─── Test #6: BadMagic_Drops ────────────────────────────────────────────────

int bad_magic_drops()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            // Raw ZMQ context + DEALER to fabricate a malformed frame.
            // Not a mock — same ZMQ library, non-production wire shape.
            void *ctx = zmq_ctx_new();
            ASSERT_NE(ctx, nullptr);

            void *sock = zmq_socket(ctx, ZMQ_DEALER);
            ASSERT_NE(sock, nullptr);

            const std::string id = "BAD-MAGIC-SENDER";
            admit_sender(*q, inbox_keys);
            zmq_setsockopt(sock, ZMQ_IDENTITY, id.c_str(), id.size());
            // The receiver is CURVE-only.  This worker fabricates a bad
            // PAYLOAD; it is not testing an unauthenticated peer, so it
            // presents a genuine admitted identity and lets the frame itself
            // be the malformed thing.
            zmq_setsockopt(sock, ZMQ_CURVE_PUBLICKEY, inbox_keys.send_pub.c_str(), 40);
            zmq_setsockopt(sock, ZMQ_CURVE_SECRETKEY, inbox_keys.send_sec.c_str(), 40);
            zmq_setsockopt(sock, ZMQ_CURVE_SERVERKEY, inbox_keys.recv_pub.c_str(), 40);
            int linger = 0;
            zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));

            const std::string ep = q->actual_endpoint();
            ASSERT_EQ(zmq_connect(sock, ep.c_str()), 0);

            std::this_thread::sleep_for(ms{50}); // let connect establish

            const char bad_payload[] = "BAAD";
            zmq_send(sock, bad_payload, sizeof(bad_payload) - 1, 0);

            const auto *item = q->recv_one(ms{200});
            EXPECT_EQ(item, nullptr);
            EXPECT_GT(q->recv_frame_error_count(), uint64_t{0});

            zmq_close(sock);
            zmq_ctx_term(ctx);
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::bad_magic_drops", PLH_INBOX_MODS);
}

// ─── Replay defense: replayed + skewed frames are dropped (§3.6) ────────────

int replay_and_skew_dropped()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            // The reject paths log WARNs; frame 1 uses a dummy payload that
            // fails unpack (also a WARN).  Declare all three up-front.
            log_cap.ExpectLogWarn("unpack error");
            log_cap.ExpectLogWarn("dropping replayed frame");
            log_cap.ExpectLogWarn("wall-clock skew");

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            // Raw DEALER lets us control the replay-metadata frame directly
            // (the production InboxClient always stamps a FRESH nonce, so a
            // replay can only be fabricated at the wire level).  Not a mock —
            // same ZMQ library, real InboxQueue receiver.
            void *ctx = zmq_ctx_new();
            ASSERT_NE(ctx, nullptr);
            void *sock = zmq_socket(ctx, ZMQ_DEALER);
            ASSERT_NE(sock, nullptr);
            const std::string id = "REPLAY-SENDER";
            admit_sender(*q, inbox_keys);
            zmq_setsockopt(sock, ZMQ_IDENTITY, id.c_str(), id.size());
            // The receiver is CURVE-only.  This worker fabricates a bad
            // PAYLOAD; it is not testing an unauthenticated peer, so it
            // presents a genuine admitted identity and lets the frame itself
            // be the malformed thing.
            zmq_setsockopt(sock, ZMQ_CURVE_PUBLICKEY, inbox_keys.send_pub.c_str(), 40);
            zmq_setsockopt(sock, ZMQ_CURVE_SECRETKEY, inbox_keys.send_sec.c_str(), 40);
            zmq_setsockopt(sock, ZMQ_CURVE_SERVERKEY, inbox_keys.recv_pub.c_str(), 40);
            int linger = 0;
            zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
            ASSERT_EQ(zmq_connect(sock, q->actual_endpoint().c_str()), 0);
            std::this_thread::sleep_for(ms{50});

            const uint64_t now =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
            auto put_be64 = [](unsigned char *p, uint64_t v)
            {
                for (int i = 0; i < 8; ++i)
                    p[7 - i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFu);
            };
            // 24-byte meta = nonce(16) + wall_ts(8, big-endian).
            auto send_frame = [&](unsigned char nonce_byte, uint64_t wall_ts)
            {
                unsigned char meta[24];
                std::memset(meta, nonce_byte, 16);
                put_be64(meta + 16, wall_ts);
                zmq_send(sock, "", 0, ZMQ_SNDMORE);    // empty delimiter
                zmq_send(sock, meta, 24, ZMQ_SNDMORE); // replay metadata
                zmq_send(sock, "BAAD", 4, 0);          // dummy payload
            };

            // Frame 1: fresh nonce, current ts.  The replay guard RECORDS the
            // nonce before the payload unpack, so even though the dummy
            // payload then fails unpack, the nonce is remembered.
            send_frame(0xAB, now);
            (void)q->recv_one(ms{200});

            // Frame 2: SAME nonce -> rejected as a replay.
            send_frame(0xAB, now);
            EXPECT_EQ(q->recv_one(ms{200}), nullptr);

            // Frame 3: fresh nonce but a stale wall_ts (60 s old) -> rejected
            // on skew, before the guard even sees the nonce.
            send_frame(0xCD, now - 60'000);
            EXPECT_EQ(q->recv_one(ms{200}), nullptr);

            EXPECT_EQ(q->recv_replay_reject_count(), uint64_t{2})
                << "one replay + one skew must both be dropped by §3.6";

            zmq_close(sock);
            zmq_ctx_term(ctx);
            q->stop();

            log_cap.Uninstall();
        },
        "hub_inbox_queue::replay_and_skew_dropped", PLH_INBOX_MODS);
}

// ─── Test #7: AckCode3_HandlerError ─────────────────────────────────────────

int ack_code_3_handler_error()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c = InboxClient::connect_to(q->actual_endpoint(), "prod.ackerr.uid00000001",
                                             uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t v = 7;
            std::memcpy(buf, &v, sizeof(v));

            // recv_one + send_ack(3) on the SAME thread (ZMQ socket
            // thread-safety).
            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{2000});
                                      if (item)
                                          q->send_ack(3); // handler_error ACK
                                      return item != nullptr;
                                  });

            std::this_thread::sleep_for(ms{30});
            uint8_t ack_code = c->send(ms{2000});

            ASSERT_TRUE(fut.get());
            ASSERT_NE(item, nullptr);
            EXPECT_EQ(ack_code, 3u);

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::ack_code_3_handler_error", PLH_INBOX_MODS);
}

// ─── Test #8: NotStarted_RecvReturnsNull ────────────────────────────────────

int not_started_recv_returns_null()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // Never called start()
            const auto *item = q->recv_one(ms{10});
            EXPECT_EQ(item, nullptr);

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::not_started_recv_returns_null", PLH_INBOX_MODS);
}

// ─── Test #9: EmptySchema_FactoryFails ──────────────────────────────────────

int empty_schema_factory_fails()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogError("schema must not be empty");

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", {});
            EXPECT_EQ(q, nullptr);

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::empty_schema_factory_fails", PLH_INBOX_MODS);
}

// ─── Test #10: EmptySchema_ClientFactoryFails ───────────────────────────────

int empty_schema_client_factory_fails()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogError("schema must not be empty");

            auto c = InboxClient::connect_to("tcp://127.0.0.1:5599", "TEST-UID", {});
            EXPECT_EQ(c, nullptr);

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::empty_schema_client_factory_fails", PLH_INBOX_MODS);
}

// ─── Test #11: ItemSize_MatchesSchema ───────────────────────────────────────

int item_size_matches_schema()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            EXPECT_EQ(q->item_size(), sizeof(uint32_t));

            auto c = InboxClient::connect_to("tcp://127.0.0.1:5599", "UID", uint32_schema());
            ASSERT_NE(c, nullptr);
            EXPECT_EQ(c->item_size(), sizeof(uint32_t));

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::item_size_matches_schema", PLH_INBOX_MODS);
}

// ─── Test #12: SchemaMismatch_DifferentType_DropsFrame ──────────────────────

int schema_mismatch_different_type_drops_frame()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogWarn("ACK timeout");

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            std::vector<ZmqSchemaField> float64_schema = {{"float64", 1, 0}};
            auto c = InboxClient::connect_to(q->actual_endpoint(), "MISMATCH-01", float64_schema);
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            double val = 3.14;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{500});
                                      // Ack only when accepted — makes the ack=255 assert
                                      // below sensitive to a "failed to drop" regression.
                                      if (item)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });
            std::this_thread::sleep_for(ms{30});
            const uint8_t ack = c->send(ms{1500});
            EXPECT_EQ(ack, 255u) << "expected ACK timeout (255) on rejected frame; got "
                                 << static_cast<int>(ack);

            EXPECT_FALSE(fut.get()) << "Schema mismatch: recv_one should reject";
            EXPECT_GT(q->recv_frame_error_count(), 0u)
                << "Schema mismatch should increment frame error count";

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::schema_mismatch_different_type_drops_frame", PLH_INBOX_MODS);
}

// ─── Test #13: SchemaMismatch_DifferentSize_DropsFrame ──────────────────────

int schema_mismatch_different_size_drops_frame()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogWarn("ACK timeout");

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            std::vector<ZmqSchemaField> uint64_schema = {{"uint64", 1, 0}};
            auto c = InboxClient::connect_to(q->actual_endpoint(), "MISMATCH-02", uint64_schema);
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint64_t val = 0xDEADBEEF;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{500});
                                      if (item)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });
            std::this_thread::sleep_for(ms{30});
            const uint8_t ack = c->send(ms{1500});
            EXPECT_EQ(ack, 255u) << "expected ACK timeout (255) on rejected frame; got "
                                 << static_cast<int>(ack);

            EXPECT_FALSE(fut.get()) << "Size mismatch: recv_one should reject";
            EXPECT_GT(q->recv_frame_error_count(), 0u);

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::schema_mismatch_different_size_drops_frame", PLH_INBOX_MODS);
}

// ─── Test #14: ChecksumEnforced_Roundtrip ───────────────────────────────────

int checksum_enforced_roundtrip()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            q->set_checksum_policy(ChecksumPolicy::Enforced);
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c = InboxClient::connect_to(q->actual_endpoint(), "CKSUM-ENF", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_checksum_policy(ChecksumPolicy::Enforced);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t val = 0xCAFE;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{2000});
                                      if (item)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });
            std::this_thread::sleep_for(ms{30});
            const uint8_t ack = c->send(ms{1500});
            EXPECT_EQ(ack, 0u) << "send timed out or got non-zero ack=" << static_cast<int>(ack);

            ASSERT_TRUE(fut.get()) << "Enforced: recv_one should succeed";
            ASSERT_NE(item, nullptr);
            uint32_t received = 0;
            std::memcpy(&received, item->data, sizeof(received));
            EXPECT_EQ(received, val);
            EXPECT_EQ(q->checksum_error_count(), 0u);

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::checksum_enforced_roundtrip", PLH_INBOX_MODS);
}

// ─── Test #15: ChecksumManual_NoStamp_ReceiverRejects ───────────────────────

int checksum_manual_no_stamp_receiver_rejects()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogError("checksum error after decode");
            log_cap.ExpectLogWarn("ACK timeout");

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            q->set_checksum_policy(ChecksumPolicy::Enforced); // verifies
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c = InboxClient::connect_to(q->actual_endpoint(), "CKSUM-MAN", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_checksum_policy(ChecksumPolicy::Manual); // no auto-stamp
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t val = 0xDEAD;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{500});
                                      if (item)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });
            std::this_thread::sleep_for(ms{30});
            const uint8_t ack = c->send(ms{1500});
            EXPECT_EQ(ack, 255u) << "expected ACK timeout (255) on rejected frame; got "
                                 << static_cast<int>(ack);

            EXPECT_FALSE(fut.get()) << "Manual no-stamp: recv_one should reject";
            EXPECT_GT(q->checksum_error_count(), 0u);

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::checksum_manual_no_stamp_receiver_rejects", PLH_INBOX_MODS);
}

// ─── Test #16: ChecksumNone_Roundtrip ───────────────────────────────────────

int checksum_none_roundtrip()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            q->set_checksum_policy(ChecksumPolicy::None);
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c = InboxClient::connect_to(q->actual_endpoint(), "CKSUM-NONE", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_checksum_policy(ChecksumPolicy::None);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t val = 0xF00D;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{2000});
                                      if (item)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });
            std::this_thread::sleep_for(ms{30});
            const uint8_t ack = c->send(ms{1500});
            EXPECT_EQ(ack, 0u) << "send timed out or got non-zero ack=" << static_cast<int>(ack);

            ASSERT_TRUE(fut.get()) << "None: recv_one should succeed";
            ASSERT_NE(item, nullptr);
            uint32_t received = 0;
            std::memcpy(&received, item->data, sizeof(received));
            EXPECT_EQ(received, val);
            EXPECT_EQ(q->checksum_error_count(), 0u);

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::checksum_none_roundtrip", PLH_INBOX_MODS);
}

// ─── Inbox CURVE auth (HEP-CORE-0027 §3.5, HEP-CORE-0036 §9.3) ───────────────
// The inbox ROUTER binds as a CURVE server under a distinct "<uid>:inbox"
// ZAP domain and admits ONLY pubkeys in its hub-wide known_roles roster
// (installed via set_peer_allowlist).  These two workers pin both sides of
// that gate against the real InboxQueue + InboxClient + ZapRouter — no mocks.

int inbox_curve_authorized_delivers()
{
    return run_gtest_worker(
        []
        {
            namespace sec = pylabhub::utils::security;
            const auto [recv_pub, recv_sec] = make_keypair();
            const auto [alice_pub, alice_sec] = make_keypair();
            sec::secure().keys().add_identity_from_z85("inbox_recv_id", recv_pub, recv_sec);
            sec::secure().keys().add_identity_from_z85("alice_id", alice_pub, alice_sec);

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            q->set_curve_server_identity("inbox_recv_id", "test.inbox.curve.pos");
            ASSERT_TRUE(q->start());
            // Seed the roster: alice is authorized (mirrors the S3 roster
            // seed the role does off REG_ACK.known_roles).
            sec::PeerAllowlist allow;
            allow.peers.insert(sec::PeerIdentity{"curve", alice_pub});
            ASSERT_TRUE(q->set_peer_allowlist(allow));

            sec::ZapPumpThread pump; // authorizes CURVE handshakes via ZapRouter

            auto c =
                InboxClient::connect_to(q->actual_endpoint(), "alice.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_curve_client_identity("alice_id", recv_pub);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t val = 0xA11CE;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{2000});
                                      if (item)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });
            std::this_thread::sleep_for(ms{30});
            const uint8_t ack = c->send(ms{1500});

            ASSERT_TRUE(fut.get())
                << "authorized (alice in roster) CURVE inbox send should deliver";
            ASSERT_NE(item, nullptr);
            uint32_t received = 0;
            std::memcpy(&received, item->data, sizeof(received));
            EXPECT_EQ(received, val);
            EXPECT_EQ(ack, 0u);

            c->stop();
            q->stop();
        },
        "hub_inbox_queue::inbox_curve_authorized_delivers", PLH_INBOX_MODS);
}

int inbox_curve_unknown_denied()
{
    return run_gtest_worker(
        []
        {
            namespace sec = pylabhub::utils::security;
            const auto [recv_pub, recv_sec] = make_keypair();
            const auto [alice_pub, alice_sec] = make_keypair(); // authorized (roster)
            const auto [bob_pub, bob_sec] = make_keypair();     // NOT in roster
            (void)alice_sec; // alice's pubkey seeds the roster; no alice socket here
            sec::secure().keys().add_identity_from_z85("inbox_recv_id", recv_pub, recv_sec);
            sec::secure().keys().add_identity_from_z85("bob_id", bob_pub, bob_sec);

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            q->set_curve_server_identity("inbox_recv_id", "test.inbox.curve.neg");
            ASSERT_TRUE(q->start());
            // Roster holds ONLY alice — bob is a known-keypair peer that the
            // hub does NOT know (its pubkey is not in known_roles).
            sec::PeerAllowlist allow;
            allow.peers.insert(sec::PeerIdentity{"curve", alice_pub});
            ASSERT_TRUE(q->set_peer_allowlist(allow));

            sec::ZapPumpThread pump;

            auto c =
                InboxClient::connect_to(q->actual_endpoint(), "bob.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_curve_client_identity("bob_id", recv_pub);
            ASSERT_TRUE(c->start()); // socket-level connect succeeds; ZAP denies handshake

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t val = 0xB0B;
            std::memcpy(buf, &val, sizeof(val));

            const InboxItem *item = nullptr;
            auto fut = std::async(std::launch::async,
                                  [&]
                                  {
                                      item = q->recv_one(ms{700});
                                      if (item)
                                          q->send_ack(0);
                                      return item != nullptr;
                                  });
            std::this_thread::sleep_for(ms{30});
            // Historically this call hung forever: the denied handshake tore
            // down the DEALER's only pipe, and libzmq's default SNDTIMEO of -1
            // parked the caller.  `InboxClient::send` now transmits with
            // `dontwait`, so a peer with no writable pipe fails fast.
            // BackPressureIsBoundedAndEdgeLogged pins that directly.
            const uint8_t ack = c->send(ms{500});

            // The ZAP gate denies bob's CURVE handshake, so NO message reaches
            // the ROUTER and the send gets no ACK.
            EXPECT_FALSE(fut.get()) << "unknown peer (bob not in roster) must NOT reach the inbox";
            EXPECT_EQ(item, nullptr);
            EXPECT_NE(ack, 0u) << "denied send must not receive a success ACK";

            c->stop();
            q->stop();
        },
        "hub_inbox_queue::inbox_curve_unknown_denied", PLH_INBOX_MODS);
}

// ─── Test: back-pressure is bounded, and its log is edge-triggered ──────────

int inbox_backpressure_bounded_and_edge_logged()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            // MustFire: the whole point is that the blocked path announces
            // itself exactly once.  A permissive ExpectLogWarn would pass even
            // if production stopped emitting it.
            log_cap.ExpectLogWarnMustFire("send:blocked");

            /// The captured blocked/recovered markers, in emission order.
            /// Edge-triggering is a claim about the SEQUENCE of lines, not
            /// about how many there are: back-pressure oscillates (the pipe
            /// partially drains between sends), so any fixed count would pin
            /// timing rather than design.  What the latch guarantees is that
            /// the two never repeat — every 'blocked' is separated from the
            /// next by a 'recovered'.
            auto edge_markers = [&]
            {
                pylabhub::utils::Logger::instance().flush();
                std::ifstream f(log_cap.log_path());
                std::string line;
                std::vector<char> seq;
                while (std::getline(f, line))
                {
                    if (line.find("send:blocked") != std::string::npos)
                        seq.push_back('B');
                    else if (line.find("send:recovered") != std::string::npos)
                        seq.push_back('R');
                }
                return seq;
            };
            auto count_of = [](const std::vector<char> &seq, char c)
            { return std::count(seq.begin(), seq.end(), c); };

            // rcvhwm=1: a receiver that never calls recv_one() reaches its
            // high-water mark almost at once.  This is the production
            // slow-consumer shape (a role busy in a handler while messages
            // arrive), just with the threshold turned down so the test does
            // not have to push a thousand messages to reach it.
            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema(), "aligned", 1);
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c =
                InboxClient::connect_to(q->actual_endpoint(), "flooder.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            uint32_t seq = 0;
            auto fire = [&]
            {
                void *buf = c->acquire();
                if (buf == nullptr)
                    return static_cast<uint8_t>(255);
                std::memcpy(buf, &seq, sizeof(seq));
                ++seq;
                return c->send(ms{0}); // fire-and-forget: bounded by the transmit alone
            };

            // ── The anti-hang pin ────────────────────────────────────────────
            // Before the dontwait fix this loop never terminated: at the
            // high-water mark libzmq's default SNDTIMEO of -1 parked the
            // caller forever.  A test that hangs here IS the regression
            // signal — the worker is killed and the exit code surfaces it.
            // The iteration cap only bounds the opposite failure (a send that
            // never reports back-pressure at all).
            constexpr uint32_t kMaxSends = 200000;
            uint8_t rc = 0;
            while (seq < kMaxSends)
            {
                rc = fire();
                if (rc == 255)
                    break;
            }
            ASSERT_EQ(rc, 255u) << "a receiver stuck at its high-water mark must make send() "
                                   "fail, not block the caller";
            ASSERT_LT(seq, kMaxSends) << "send() never reported back-pressure";
            EXPECT_GT(c->send_blocked_count(), 0u);
            EXPECT_EQ(edge_markers().size(), 1u) << "the blocked edge announces itself once";

            // ── Keep pushing, then let the receiver catch up ─────────────────
            // Back-pressure oscillates: the outbound pipe partially drains
            // between sends, so refusals and successes interleave.  Each
            // full -> normal -> full cycle is a genuine event and SHOULD be
            // logged.  Draining the receiver at the end guarantees at least
            // one recovery edge regardless of how the middle went.
            const uint64_t blocked_before = c->send_blocked_count();
            int refused = 0;
            for (int i = 0; i < 2000; ++i)
                if (fire() == 255u)
                    ++refused;
            EXPECT_EQ(c->send_blocked_count(), blocked_before + static_cast<uint64_t>(refused))
                << "the counter records EVERY refusal, unlike the log";

            for (int i = 0; i < 8192; ++i)
            {
                const InboxItem *item = q->recv_one(ms{20});
                if (item == nullptr)
                    break;
                q->send_ack(0);
            }
            ASSERT_TRUE(pylabhub::tests::helper::poll_until([&] { return fire() != 255u; },
                                                            std::chrono::seconds{10}))
                << "once the receiver drains, sending must become possible again";

            // ── The anti-flood pin ───────────────────────────────────────────
            // The latch's guarantee is about ORDER, not count: blocked and
            // recovered strictly alternate, starting with blocked.  A repeat
            // of either would mean the latch failed and a stuck peer could
            // emit one line per send — thousands here.
            const auto seqm = edge_markers();
            ASSERT_FALSE(seqm.empty());
            EXPECT_EQ(seqm.front(), 'B') << "the first edge must be the block, not a recovery";
            for (std::size_t i = 1; i < seqm.size(); ++i)
                EXPECT_NE(seqm[i], seqm[i - 1])
                    << "edge " << i << " repeats '" << seqm[i]
                    << "' — the latch re-fired without the opposite transition, so a peer that "
                       "stays blocked would flood the log";
            EXPECT_GE(count_of(seqm, 'R'), 1) << "the drain must produce a recovery edge";
            EXPECT_LE(static_cast<uint64_t>(count_of(seqm, 'B')), c->send_blocked_count())
                << "there cannot be more block lines than block events";

            c->stop();
            q->stop();
        },
        "hub_inbox_queue::inbox_backpressure_bounded_and_edge_logged", PLH_INBOX_MODS);
}

// ─── Test: a late ACK is never reported as the next message's result ────────

int inbox_stale_ack_not_attributed_to_next_send()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            log_cap.ExpectLogWarnMustFire("ACK timeout");

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            // No unarmed inbox exists — start() PANICs without an identity.
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c =
                InboxClient::connect_to(q->actual_endpoint(), "slowpoke.uid00000001",
                                        uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys);
            ASSERT_TRUE(c->start());

            auto put = [&](uint32_t v)
            {
                void *buf = c->acquire();
                ASSERT_NE(buf, nullptr);
                std::memcpy(buf, &v, sizeof(v));
            };

            // ── Message 1: the receiver is busy, so the caller gives up ──────
            // This is the ordinary slow-handler case, not a contrived one: the
            // message arrives, but the caller's ACK budget expires before the
            // handler gets to it.
            put(0x1111);
            EXPECT_EQ(c->send(ms{80}), 255u) << "no ACK within the budget must read as failure";

            // The receiver now catches up and acknowledges message 1 — LATE.
            // Its receipt is in flight toward a caller that has stopped
            // waiting for it.
            const InboxItem *first = q->recv_one(ms{1000});
            ASSERT_NE(first, nullptr);
            q->send_ack(0); // code 0 == success, the ONLY code production emits

            // ── Message 2: distinguishable answer ────────────────────────────
            // The receiver answers message 2 with code 3.  If the stale
            // receipt for message 1 were attributed to this send, the caller
            // would see 0 — a stale SUCCESS for work that had not happened.
            // Seeing 3 proves the receipt was matched to its own message.
            put(0x2222);
            auto responder = std::async(std::launch::async,
                                        [&]
                                        {
                                            const InboxItem *second = q->recv_one(ms{2000});
                                            if (second == nullptr)
                                                return false;
                                            q->send_ack(3);
                                            return true;
                                        });
            const uint8_t rc = c->send(ms{2000});
            ASSERT_TRUE(responder.get()) << "receiver never saw message 2";

            EXPECT_EQ(rc, 3u) << "the caller must be given ITS OWN message's ACK code; a 0 here "
                                 "is message 1's stale receipt reported as message 2's result";
            EXPECT_EQ(c->ack_stale_count(), 1u)
                << "exactly one late receipt should have been recognised and discarded";

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::inbox_stale_ack_not_attributed_to_next_send", PLH_INBOX_MODS);
}

} // namespace hub_inbox_queue
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct HubInboxQueueRegistrar
{
    HubInboxQueueRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "hub_inbox_queue")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_inbox_queue;

                if (sc == "bind_and_connect_basic")
                    return bind_and_connect_basic();
                if (sc == "recv_one_timeout_returns_null")
                    return recv_one_timeout_returns_null();
                if (sc == "multiple_messages")
                    return multiple_messages();
                if (sc == "double_stop_no_throw")
                    return double_stop_no_throw();
                if (sc == "sender_uid_is_preserved")
                    return sender_uid_is_preserved();
                if (sc == "bad_magic_drops")
                    return bad_magic_drops();
                if (sc == "replay_and_skew_dropped")
                    return replay_and_skew_dropped();
                if (sc == "ack_code_3_handler_error")
                    return ack_code_3_handler_error();
                if (sc == "not_started_recv_returns_null")
                    return not_started_recv_returns_null();
                if (sc == "empty_schema_factory_fails")
                    return empty_schema_factory_fails();
                if (sc == "empty_schema_client_factory_fails")
                    return empty_schema_client_factory_fails();
                if (sc == "item_size_matches_schema")
                    return item_size_matches_schema();
                if (sc == "schema_mismatch_different_type_drops_frame")
                    return schema_mismatch_different_type_drops_frame();
                if (sc == "schema_mismatch_different_size_drops_frame")
                    return schema_mismatch_different_size_drops_frame();
                if (sc == "checksum_enforced_roundtrip")
                    return checksum_enforced_roundtrip();
                if (sc == "checksum_manual_no_stamp_receiver_rejects")
                    return checksum_manual_no_stamp_receiver_rejects();
                if (sc == "checksum_none_roundtrip")
                    return checksum_none_roundtrip();
                if (sc == "inbox_curve_authorized_delivers")
                    return inbox_curve_authorized_delivers();
                if (sc == "inbox_curve_unknown_denied")
                    return inbox_curve_unknown_denied();
                if (sc == "inbox_backpressure_bounded_and_edge_logged")
                    return inbox_backpressure_bounded_and_edge_logged();
                if (sc == "inbox_stale_ack_not_attributed_to_next_send")
                    return inbox_stale_ack_not_attributed_to_next_send();
                return -1;
            });
    }
} g_registrar;

} // namespace
