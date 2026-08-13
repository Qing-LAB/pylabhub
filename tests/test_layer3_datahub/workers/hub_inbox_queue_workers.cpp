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
 * production lifecycle module.  Test #6 (`wrong_frame_count_drops`) uses
 * raw `zmq_ctx_new()` + `zmq_socket()` to fabricate a malformed wire
 * envelope — that is NOT a mock; it is the same underlying ZMQ library
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
#include "queue_activation.h" // bound_endpoint_or_fail

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
///
/// Stands in for the role, which is what binds the authority in production
/// (`RoleAPIBase::set_inbox_queue`) and answers out of its replicated roster.
/// @param sender_uid  the uid the sender's key resolves to.  Required, and it
///        must be the uid that sender really uses: the receiver now names its
///        sender from the proven key (HEP-CORE-0027 §3.7), so a made-up value
///        here would make `InboxItem::sender_id` agree with the harness rather
///        than with the sender.
inline void admit_authority(InboxQueue &q, const std::string &sender_pub,
                            const std::string &sender_uid)
{
    namespace sec = pylabhub::utils::security;

    // A real PeerAuthority over one entry — the same object a role builds from
    // REG_ACK.known_roles, answering both questions from one table.  A pair of
    // hand-rolled lambdas would be a second implementation of the combine,
    // free to admit a key the naming half cannot resolve.
    sec::PeerAuthority::Builder builder;
    builder.add_local_role(sec::RosterEntry{sender_uid, sender_pub});
    auto authority = std::make_shared<const sec::PeerAuthority>(std::move(builder).build(1));

    q.set_admission_authority(InboxQueue::InboxAuthority{
        [authority](const std::string &pubkey_z85)
        {
            try
            {
                return authority->admits(sec::Z85PublicKey::validate(pubkey_z85));
            }
            catch (const std::invalid_argument &)
            {
                return false;
            }
        },
        [authority](const std::optional<sec::AttestedKey> &attested)
        { return authority->attribute_sender(attested); }});
}

inline void admit_sender(InboxQueue &q, const InboxCurve &k, const std::string &sender_uid)
{
    admit_authority(q, k.send_pub, sender_uid);
}

inline void admit_and_arm_client(InboxQueue &q, InboxClient &c, const InboxCurve &k,
                                 const std::string &sender_uid)
{
    admit_sender(q, k, sender_uid);
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

            const std::string ep = ::pylabhub::tests::bound_endpoint_or_fail(*q);
            EXPECT_FALSE(ep.empty());
            EXPECT_NE(ep.find("tcp://"), std::string::npos);

            auto c = InboxClient::connect_to(ep, "prod.test.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "prod.test.uid00000001");
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "prod.multi.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "prod.multi.uid00000001");
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             kSenderId, uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, kSenderId);
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

// ─── Test #5b: SenderNameComesFromTheKey_NotTheRoutingId ────────────────────
//
// The distinguishing case for HEP-CORE-0027 §3.7.  Every other inbox test
// uses a sender whose routing id and uid are the same string, so all of them
// pass whether the receiver reads the frame or resolves the key — they prove
// nothing broke, not that the identity is derived.
//
// Here the two DISAGREE.  The sender proves its own key and presents another
// role's uid as its ZMQ identity, which is a label it chooses and nothing
// stops it choosing.  The receiver must report the uid that key belongs to.
//
// Reading the frame instead yields the impersonated name, which is both the
// attribution the receiving script would act on and the key the replay guard
// and the sequence map would use — so this one assertion covers all three
// uses §3.7 lists.
int sender_name_comes_from_the_key()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            // What the sender's key really belongs to, and the label it lies
            // with.  The victim name is a plausible peer, not a marker
            // string: the point is that it would be believed.
            const std::string kTrueUid = "prod.true.uid12345678";
            const std::string kClaimedUid = "prod.victim.uid87654321";

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            // The routing id is the CLAIMED uid — InboxClient sets ZMQ_IDENTITY
            // from this argument, so a caller picks it freely.
            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             kClaimedUid, uint32_schema());
            ASSERT_NE(c, nullptr);
            // The roster names this key as kTrueUid.  The receiver's only
            // route to a name.
            admit_and_arm_client(*q, *c, inbox_keys, kTrueUid);
            ASSERT_TRUE(c->start());

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t v = 7;
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
            const uint8_t ack = c->send(ms{1500});
            EXPECT_EQ(ack, 0u) << "send timed out or got non-zero ack=" << static_cast<int>(ack);

            ASSERT_TRUE(fut.get());
            ASSERT_NE(item, nullptr);
            EXPECT_EQ(item->sender_id, kTrueUid)
                << "the receiver named its sender from the routing frame, so a peer can be "
                   "attributed as any role it cares to name (HEP-CORE-0027 §3.7)";
            EXPECT_NE(item->sender_id, kClaimedUid)
                << "the impersonated name reached the application";

            c->stop();
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::sender_name_comes_from_the_key", PLH_INBOX_MODS);
}

// ─── Test #6: WrongFrameCount_Drops ─────────────────────────────────────────
//
// Scope: the receiver's ENVELOPE-SHAPE guard, not the payload codec.
// `recv_one` requires the four-frame envelope (identity, delimiter, replay
// metadata, payload); anything else is dropped and counted before any byte
// of the payload is parsed.
//
// Frame MAGIC is deliberately NOT tested here.  Magic lives in
// `wire_detail::decode_frame`, the single decoder the inbox and the data
// plane share, and it is pinned at L1 by `ZmqWireFrameTest.RejectsWrongMagic`
// — directly on that function, where a wrong magic is the only variable.
// Re-testing it through a live CURVE socket would duplicate that pin while
// being unable to observe it: the shape guard below rejects a fabricated
// frame long before the decoder runs.
int wrong_frame_count_drops()
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

            // Raw ZMQ context + DEALER to send a wrongly-shaped envelope.
            // Not a mock — same ZMQ library, non-production wire shape.
            void *ctx = zmq_ctx_new();
            ASSERT_NE(ctx, nullptr);

            void *sock = zmq_socket(ctx, ZMQ_DEALER);
            ASSERT_NE(sock, nullptr);

            const std::string id = "MALFORMED-FRAME-SENDER";
            admit_sender(*q, inbox_keys, id);
            zmq_setsockopt(sock, ZMQ_IDENTITY, id.c_str(), id.size());
            // The receiver is CURVE-only.  This worker fabricates a bad
            // ENVELOPE; it is not testing an unauthenticated peer, so it
            // presents a genuine admitted identity and lets the frame itself
            // be the malformed thing.
            zmq_setsockopt(sock, ZMQ_CURVE_PUBLICKEY, inbox_keys.send_pub.c_str(), 40);
            zmq_setsockopt(sock, ZMQ_CURVE_SECRETKEY, inbox_keys.send_sec.c_str(), 40);
            zmq_setsockopt(sock, ZMQ_CURVE_SERVERKEY, inbox_keys.recv_pub.c_str(), 40);
            int linger = 0;
            zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));

            const std::string ep = ::pylabhub::tests::bound_endpoint_or_fail(*q);
            ASSERT_EQ(zmq_connect(sock, ep.c_str()), 0);

            std::this_thread::sleep_for(ms{50}); // let connect establish

            // One frame from a DEALER reaches the ROUTER as [identity,
            // payload] — two frames, where the envelope requires four
            // (identity, delimiter, replay metadata, payload).  The
            // production sender never emits this shape; only a hand-rolled
            // socket can.
            const char lone_frame[] = "BAAD";
            zmq_send(sock, lone_frame, sizeof(lone_frame) - 1, 0);

            const auto *item = q->recv_one(ms{200});
            EXPECT_EQ(item, nullptr) << "a two-frame envelope was accepted as a message";
            EXPECT_GT(q->recv_frame_error_count(), uint64_t{0})
                << "the wrongly-shaped envelope was dropped without being counted";

            zmq_close(sock);
            zmq_ctx_term(ctx);
            q->stop();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::wrong_frame_count_drops", PLH_INBOX_MODS);
}

// ─── recv_gap_count: a slow receiver makes the sender drop, and the ─────────
//     receiver learns exactly how many it missed.
//
// This is the production scenario the counter exists for, reproduced with
// production classes only: the receiver stops draining, its queue fills, and
// `InboxClient::send` then DROPS rather than parking the caller (its
// documented contract — 255 on "receiver's queue is at its high-water mark").
//
// The hole is what makes the gap observable.  `send()` consumes a sequence
// number BEFORE it attempts the write, so a dropped send burns a seq that
// never reaches the wire.  When the receiver drains and the next message
// lands, its seq is ahead of what the receiver expected by exactly the number
// of drops — which is why this test can assert an exact count rather than
// "greater than zero".
//
// No frames are fabricated here.  An earlier reading of this gap concluded it
// was untestable because a hand-built frame cannot carry a valid schema tag
// (`compute_inbox_schema_tag` is file-static).  That was the wrong question:
// the counter is not reached by forging a frame, it is reached by causing a
// real loss.

int gap_count_tracks_dropped_sends()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            // The drop path is edge-triggered WARN by design ("send:blocked").
            log_cap.ExpectLogWarn("send:blocked");

            // A small receive backlog is what makes the sender start dropping
            // within a bounded number of sends; nothing about the contract
            // depends on the exact value.  Do NOT lower this to 1 — libzmq
            // carries the ZMTP handshake through the same pipe, and a
            // one-message backlog starves it, so the connection never
            // establishes and the test measures nothing (observed 2026-08-02).
            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema(), "aligned",
                                         /*rcvhwm=*/16);
            ASSERT_NE(q, nullptr);
            const auto inbox_keys = arm_inbox_queue(*q, "test.inbox");
            ASSERT_TRUE(q->start());
            pylabhub::utils::security::ZapPumpThread inbox_pump;

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "prod.gap.uid000000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "prod.gap.uid000000001");
            ASSERT_TRUE(c->start());

            // Fire-and-forget so the loop is bounded by the socket, not by ACK
            // round-trips.  The receiver deliberately never calls recv_one
            // here, so the transport backs up and sends start failing.
            auto send_value = [&](uint32_t v) -> uint8_t
            {
                void *buf = c->acquire();
                if (buf == nullptr)
                    return 255;
                std::memcpy(buf, &v, sizeof(v));
                return c->send(ms{0});
            };

            // Establish the link and PROVE it before measuring anything.
            // `send_blocked_count` rises for two different reasons — no
            // writable peer yet, and peer's queue full — and only the second
            // one is this test's subject.  Blasting straight after start()
            // trips the first: the CURVE handshake has not completed, every
            // send fails, and the loop below would exit having measured
            // nothing.  (It did exactly that on the first run.)
            const InboxItem *first = nullptr;
            uint32_t warmup = 0;
            while (first == nullptr && warmup < 200)
            {
                (void)send_value(warmup);
                first = q->recv_one(ms{100});
                ++warmup;
            }
            ASSERT_NE(first, nullptr) << "the link never came up; nothing can be measured";
            q->send_ack(0);
            uint64_t last_seq = first->seq;

            // From here the receiver stops draining, so a refused send can
            // only mean the receiver's queue is full — the condition under
            // test.  The cap is a safety bound (sender + receiver buffering
            // is ~1k frames), not an expected count.
            const uint64_t blocked_before = c->send_blocked_count();
            constexpr uint32_t kMaxAttempts = 20000;
            uint32_t attempts = 0;
            while (c->send_blocked_count() == blocked_before && attempts < kMaxAttempts)
            {
                (void)send_value(attempts);
                ++attempts;
            }
            ASSERT_GT(c->send_blocked_count(), blocked_before)
                << "the receiver's queue never filled, so no message was lost and "
                   "there is no gap to observe (attempts="
                << attempts << ")";

            // Keep pushing briefly so the drops are unambiguously in the middle
            // of the stream rather than only at its tail.
            for (uint32_t i = 0; i < 50; ++i)
                (void)send_value(kMaxAttempts + i);

            // A gap is reported ON THE MESSAGE FOLLOWING the hole
            // (`gap = seq - expected`), so it has to be read off every
            // message as it arrives.  Where the burned sequence numbers sit
            // is not controllable: they interleave with delivered messages
            // when the receiver drains between refusals, and form one run at
            // the tail when it does not.  Checking only one of those places
            // makes the test depend on which happened — it asserted the gap
            // on a trailing recovery message alone, and failed with a
            // legitimate gap of 0 on a run where the last send succeeded.
            uint64_t observed_gap_total = 0;
            auto account = [&](const InboxItem *item)
            {
                const uint64_t hole = item->seq > last_seq ? item->seq - (last_seq + 1) : 0;
                EXPECT_GT(item->seq, last_seq) << "the stream must arrive in order";
                EXPECT_EQ(item->gap, hole)
                    << "a message's gap must name exactly the sequence numbers missing "
                       "before it — that is what tells a handler WHICH state it lost "
                       "(seq="
                    << item->seq << " prev=" << last_seq << ")";
                observed_gap_total += item->gap;
                last_seq = item->seq;
                q->send_ack(0);
            };

            while (const InboxItem *item = q->recv_one(ms{200}))
                account(item);

            // Covers the tail case: with the queue drained this send lands, and
            // its seq exposes every seq burned after the last delivered message.
            const uint8_t ack = send_value(0xFEEDFACE);
            ASSERT_NE(ack, 255) << "the recovery send was also dropped";

            const InboxItem *recovered = q->recv_one(ms{2000});
            ASSERT_NE(recovered, nullptr) << "the recovery message never arrived";
            account(recovered);

            // Sends were provably refused above, and a refusal burns a sequence
            // number, so the loss must be visible somewhere in the two places
            // just checked.
            EXPECT_GT(observed_gap_total, uint64_t{0})
                << "sends were refused, so sequence numbers were burned, but no "
                   "delivered message reported them; a handler that never sees a gap "
                   "cannot tell what it is missing";
            EXPECT_GE(q->recv_gap_count(), observed_gap_total)
                << "the process-wide counter must account for every per-message gap";

            c->stop();
            q->stop();
            log_cap.Uninstall();
        },
        "hub_inbox_queue::gap_count_tracks_dropped_sends", PLH_INBOX_MODS);
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
            admit_sender(*q, inbox_keys, id);
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
            ASSERT_EQ(zmq_connect(sock, ::pylabhub::tests::bound_endpoint_or_fail(*q).c_str()), 0);
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "prod.ackerr.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "prod.ackerr.uid00000001");
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
            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "MISMATCH-01", float64_schema);
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "MISMATCH-01");
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
            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "MISMATCH-02", uint64_schema);
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "MISMATCH-02");
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "CKSUM-ENF", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_checksum_policy(ChecksumPolicy::Enforced);
            admit_and_arm_client(*q, *c, inbox_keys, "CKSUM-ENF");
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "CKSUM-MAN", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_checksum_policy(ChecksumPolicy::Manual); // no auto-stamp
            admit_and_arm_client(*q, *c, inbox_keys, "CKSUM-MAN");
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "CKSUM-NONE", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_checksum_policy(ChecksumPolicy::None);
            admit_and_arm_client(*q, *c, inbox_keys, "CKSUM-NONE");
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
// ZAP domain and admits ONLY pubkeys its hub-wide known_roles roster vouches
// for.  The roster lives on the role; the gate reaches it through the
// authority bound by set_admission_authority.  These two workers pin both
// sides of that gate against the real InboxQueue + InboxClient + ZapRouter —
// no mocks.

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
            // Stand in for the role's roster: alice is authorized (the role
            // builds this answer out of REG_ACK.known_roles).
            admit_authority(*q, alice_pub, "alice.uid00000001");

            sec::ZapPumpThread pump; // authorizes CURVE handshakes via ZapRouter

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "alice.uid00000001", uint32_schema());
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

// Deny-all until a roster is bound (HEP-CORE-0035 §4.9.6, §4.8.4).
//
// `InboxQueue::start()` registers its ZAP domain BEFORE bind precisely so no
// handshake can arrive un-gated — but a gate with nothing to ask is only safe
// if "nothing to ask" means DENY.  The sibling test above proves a peer in the
// roster is admitted and the one below proves a peer outside it is refused;
// neither can tell those apart from a gate that admits whoever it is asked
// about, because both bind an authority first.  This one binds none.
//
// The peer here holds a perfectly good keypair.  It is refused for the only
// reason under test: the receiver has not yet been told who it may admit.
int inbox_curve_no_authority_denies()
{
    return run_gtest_worker(
        []
        {
            namespace sec = pylabhub::utils::security;
            const auto [recv_pub, recv_sec] = make_keypair();
            const auto [peer_pub, peer_sec] = make_keypair();
            sec::secure().keys().add_identity_from_z85("inbox_recv_id", recv_pub, recv_sec);
            sec::secure().keys().add_identity_from_z85("peer_id", peer_pub, peer_sec);

            auto q = InboxQueue::bind_at("tcp://127.0.0.1:0", uint32_schema());
            ASSERT_NE(q, nullptr);
            q->set_curve_server_identity("inbox_recv_id", "test.inbox.curve.noauth");
            ASSERT_TRUE(q->start());
            // Deliberately NO set_admission_authority.  This is the state
            // between bind and the role adopting its first roster.

            sec::ZapPumpThread pump;

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "peer.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            c->set_curve_client_identity("peer_id", recv_pub);
            ASSERT_TRUE(c->start()); // socket-level connect succeeds; ZAP denies

            void *buf = c->acquire();
            ASSERT_NE(buf, nullptr);
            uint32_t val = 0xDEAD;
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
            const uint8_t ack = c->send(ms{500});

            EXPECT_FALSE(fut.get())
                << "an inbox with no authority bound must admit NOBODY — a gate that cannot say "
                   "who is allowed must not fall open (HEP-CORE-0035 §4.9.6)";
            EXPECT_EQ(item, nullptr);
            EXPECT_NE(ack, 0u) << "a denied send must not report success";

            c->stop();
            q->stop();
        },
        "hub_inbox_queue::inbox_curve_no_authority_denies", PLH_INBOX_MODS);
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
            admit_authority(*q, alice_pub, "alice.uid00000001");

            sec::ZapPumpThread pump;

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "bob.uid00000001", uint32_schema());
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "flooder.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "flooder.uid00000001");
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

            auto c = InboxClient::connect_to(::pylabhub::tests::bound_endpoint_or_fail(*q),
                                             "slowpoke.uid00000001", uint32_schema());
            ASSERT_NE(c, nullptr);
            admit_and_arm_client(*q, *c, inbox_keys, "slowpoke.uid00000001");
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
                if (sc == "sender_name_comes_from_the_key")
                    return sender_name_comes_from_the_key();
                if (sc == "wrong_frame_count_drops")
                    return wrong_frame_count_drops();
                if (sc == "gap_count_tracks_dropped_sends")
                    return gap_count_tracks_dropped_sends();
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
                if (sc == "inbox_curve_no_authority_denies")
                    return inbox_curve_no_authority_denies();
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
