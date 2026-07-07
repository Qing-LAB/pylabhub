/**
 * @file test_zmq_attach_channel.cpp
 * @brief L2 tests for `ZmqAttachChannel` — the ZMQ ROUTER/DEALER
 *        binding of `IAttachChannel` (Phase 4a).
 *
 * These tests exercise the transport itself (send/recv semantics,
 * deadline discipline, DoS cap, routing-identity verification) —
 * NOT the full AttachProtocol handshake (that's Phase 4b, once
 * `run_producer_handshake` is extracted as a transport-agnostic
 * helper).
 *
 * Pattern 1+: binary-wide LifecycleGuard for Logger.  ZMQ context is
 * created per-test via `zmq::context_t` — no ZMQContext lifecycle
 * module needed since we don't share the context across tests.
 */
#include "binary_lifecycle.h"
#include "utils/logger.hpp"
#include "utils/security/attach_channel_zmq.hpp"
#include "utils/security/attach_protocol.hpp"  // AttachProtocolTimeout

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "cppzmq/zmq.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// Binary-wide LifecycleGuard for Logger (no SMS needed — the channel
// only exchanges JSON frames, doesn't touch KeyStore).
PLH_BINARY_LIFECYCLE_MODULES(pylabhub::utils::Logger::GetLifecycleModule())

using pylabhub::utils::security::IAttachChannel;
using pylabhub::utils::security::ZmqAttachChannel;
using pylabhub::utils::security::AttachProtocolTimeout;

namespace
{

// Convenience: build an inproc endpoint unique to each test.
std::string
inproc_endpoint(const char *tag)
{
    static std::atomic<int> ctr{0};
    return "inproc://zmq_attach_channel_test_" + std::string(tag) + "_" +
           std::to_string(ctr.fetch_add(1));
}

// Read the routing identity a ROUTER socket assigns to an incoming
// DEALER peer — we recv the first frame of a "wake" message to
// extract it.
std::string
peek_dealer_identity(zmq::socket_t &router)
{
    zmq::message_t rid;
    auto           rc = router.recv(rid);
    if (!rc.has_value()) throw std::runtime_error("peek: empty recv");
    // Drain the body frame the dealer sent for wake-up.
    zmq::message_t body;
    (void)router.recv(body);
    return std::string(static_cast<const char *>(rid.data()), rid.size());
}

// Standard test rig: create a DEALER-ROUTER inproc pair, ROUTER binds
// then DEALER connects then DEALER sends a hello so ROUTER learns the
// identity.
struct DealerRouterPair
{
    zmq::context_t ctx{1};
    zmq::socket_t  router{ctx, zmq::socket_type::router};
    zmq::socket_t  dealer{ctx, zmq::socket_type::dealer};
    std::string    endpoint;
    std::string    dealer_identity;

    explicit DealerRouterPair(const char *tag) : endpoint(inproc_endpoint(tag))
    {
        // ZMQ_ROUTER_MANDATORY = 1 so sends to unknown identities
        // raise EAGAIN instead of silently dropping.  Matches the
        // ZmqAttachChannel contract documented in attach_channel_zmq.hpp.
        const int mandatory = 1;
        router.set(zmq::sockopt::router_mandatory, mandatory);
        router.bind(endpoint);
        dealer.connect(endpoint);

        // DEALER sends an initial "hello" frame; ROUTER recvs and
        // records the routing identity.
        const std::string hello = "hello";
        dealer.send(zmq::message_t(hello.data(), hello.size()),
                    zmq::send_flags::none);
        dealer_identity = peek_dealer_identity(router);
    }
};

} // namespace

class ZmqAttachChannelTest : public ::testing::Test
{
};

// ── happy-path roundtrip ────────────────────────────────────────────────────

TEST_F(ZmqAttachChannelTest, RoundtripSingleFrame)
{
    DealerRouterPair pair("roundtrip_single");
    ZmqAttachChannel prod_ch(pair.router, pair.dealer_identity, "producer");
    ZmqAttachChannel cons_ch(pair.dealer, "" /*DEALER side*/, "consumer");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};

    // Producer sends a JSON challenge.
    nlohmann::json challenge;
    challenge["kind"]  = "challenge";
    challenge["nonce"] = "abcd1234";
    prod_ch.send_frame(challenge, deadline);

    // Consumer receives it, sends a JSON response.
    const nlohmann::json got = cons_ch.recv_frame(deadline);
    EXPECT_EQ(got.value("kind", std::string{}), "challenge");
    EXPECT_EQ(got.value("nonce", std::string{}), "abcd1234");

    nlohmann::json response;
    response["kind"]      = "response";
    response["signature"] = "deadbeef";
    cons_ch.send_frame(response, deadline);

    const nlohmann::json reply = prod_ch.recv_frame(deadline);
    EXPECT_EQ(reply.value("kind", std::string{}), "response");
    EXPECT_EQ(reply.value("signature", std::string{}), "deadbeef");
}

// ── deadline discipline: recv times out cleanly ─────────────────────────────

TEST_F(ZmqAttachChannelTest, RecvTimesOutOnQuietPeer)
{
    DealerRouterPair pair("timeout_recv");
    ZmqAttachChannel prod_ch(pair.router, pair.dealer_identity, "producer");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{100};

    EXPECT_THROW(prod_ch.recv_frame(deadline), AttachProtocolTimeout);
}

// ── DoS cap: oversized send is rejected before hitting the wire ─────────────

TEST_F(ZmqAttachChannelTest, OversizedSendRejected)
{
    DealerRouterPair pair("oversize");
    ZmqAttachChannel prod_ch(pair.router, pair.dealer_identity, "producer");

    // Build a JSON frame whose SERIALIZED size exceeds IAttachChannel::kMaxAttachFrameBytes.
    // A JSON object with one huge string value hits the cap cleanly.
    std::string huge(IAttachChannel::kMaxAttachFrameBytes + 100, 'x');
    nlohmann::json frame;
    frame["payload"] = std::move(huge);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};

    EXPECT_THROW(prod_ch.send_frame(frame, deadline), std::runtime_error);
}

// ── DoS cap: oversized incoming frame is rejected ───────────────────────────

TEST_F(ZmqAttachChannelTest, OversizedRecvRejected)
{
    DealerRouterPair pair("oversize_recv");
    ZmqAttachChannel prod_ch(pair.router, pair.dealer_identity, "producer");

    // Consumer sends a raw oversized body (bypassing ZmqAttachChannel
    // so we test the recv-side cap).
    std::string huge(IAttachChannel::kMaxAttachFrameBytes + 100, 'y');
    pair.dealer.send(zmq::message_t(huge.data(), huge.size()),
                     zmq::send_flags::none);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};

    EXPECT_THROW(prod_ch.recv_frame(deadline), std::runtime_error);
}

// ── cross-talk: ROUTER delivers frame from an UNEXPECTED peer ──────────────
//
// D1: `ZmqAttachChannel::recv_frame` rejects frames whose routing
// identity doesn't match the channel's stored `routing_identity_`.
// Simulate: two DEALER peers on the same ROUTER, channel is bound to
// peer A's identity, peer B sends a message → channel should throw
// `std::runtime_error` (NOT AttachProtocolTimeout — the frame arrived,
// it just doesn't belong to us).

TEST_F(ZmqAttachChannelTest, RejectsCrossTalkFromDifferentPeer)
{
    zmq::context_t ctx{1};
    zmq::socket_t  router{ctx, zmq::socket_type::router};
    zmq::socket_t  dealer_a{ctx, zmq::socket_type::dealer};
    zmq::socket_t  dealer_b{ctx, zmq::socket_type::dealer};

    // Force distinct routing identities so the cross-talk is unambiguous.
    const std::string id_a = "peer-a-identity";
    const std::string id_b = "peer-b-identity";
    dealer_a.set(zmq::sockopt::routing_id, id_a);
    dealer_b.set(zmq::sockopt::routing_id, id_b);

    const int mandatory = 1;
    router.set(zmq::sockopt::router_mandatory, mandatory);
    const std::string endpoint = inproc_endpoint("cross_talk");
    router.bind(endpoint);
    dealer_a.connect(endpoint);
    dealer_b.connect(endpoint);

    // Channel is bound to peer-a's identity.
    ZmqAttachChannel prod_ch(router, id_a, "producer");

    // Peer B sends a frame (unexpected from our channel's POV).
    const std::string msg = "{\"greetings\":\"from B\"}";
    dealer_b.send(zmq::message_t(msg.data(), msg.size()),
                  zmq::send_flags::none);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};

    try
    {
        prod_ch.recv_frame(deadline);
        FAIL() << "cross-talk should raise runtime_error";
    }
    catch (const AttachProtocolTimeout &)
    {
        FAIL() << "cross-talk should NOT surface as AttachProtocolTimeout";
    }
    catch (const std::runtime_error &e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("unexpected peer"), std::string::npos)
            << "runtime_error should tag cross-talk detection; got: " << what;
    }
}

// ── trailing multipart parts rejected ──────────────────────────────────────
//
// D2: a well-formed frame is `[routing_id, body]` (two parts).  If a
// peer sends `[routing_id, body, extra]`, the channel MUST reject via
// `if (incoming_body.more())` (attach_channel_zmq.cpp line 201).

TEST_F(ZmqAttachChannelTest, RejectsTrailingMultipartParts)
{
    DealerRouterPair pair("trailing_parts");
    ZmqAttachChannel prod_ch(pair.router, pair.dealer_identity, "producer");

    // DEALER sends body + extra frame (multipart).
    const std::string body = "{\"kind\":\"ok\"}";
    const std::string extra = "extra_bytes";
    pair.dealer.send(zmq::message_t(body.data(), body.size()),
                     zmq::send_flags::sndmore);
    pair.dealer.send(zmq::message_t(extra.data(), extra.size()),
                     zmq::send_flags::none);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};

    try
    {
        prod_ch.recv_frame(deadline);
        FAIL() << "trailing multipart should raise runtime_error";
    }
    catch (const AttachProtocolTimeout &)
    {
        FAIL() << "trailing multipart should NOT be AttachProtocolTimeout";
    }
    catch (const std::runtime_error &e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("trailing frames"), std::string::npos)
            << "runtime_error should tag trailing-frames; got: " << what;
    }
}

// ── invalid JSON raises a runtime_error (not AttachProtocolTimeout) ────────

TEST_F(ZmqAttachChannelTest, MalformedJsonRaisesRuntimeError)
{
    DealerRouterPair pair("bad_json");
    ZmqAttachChannel prod_ch(pair.router, pair.dealer_identity, "producer");

    // Consumer sends malformed JSON as the body frame.
    const std::string bad = "this is not json {[";
    pair.dealer.send(zmq::message_t(bad.data(), bad.size()),
                     zmq::send_flags::none);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};

    try
    {
        prod_ch.recv_frame(deadline);
        FAIL() << "expected runtime_error for malformed JSON";
    }
    catch (const AttachProtocolTimeout &)
    {
        FAIL() << "malformed JSON should NOT surface as AttachProtocolTimeout";
    }
    catch (const std::runtime_error &e)
    {
        // Expected — message references JSON parse failure.
        const std::string what = e.what();
        EXPECT_NE(what.find("JSON parse failed"), std::string::npos)
            << "runtime_error message should tag JSON parse failure; got: "
            << what;
    }
}
