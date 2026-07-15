/**
 * @file test_wire_adapter_roundtrip.cpp
 * @brief L2 round-trip semantics tests for the wire adapter
 *        (HEP-CORE-0046 Phase B Tier 1).
 *
 * **What these tests prove.**  For every REG-family and adjacent msg_type,
 * a payload shaped exactly like what current `BrokerRequestComm::do_request`
 * produces, when routed through `encode_dealer_send` → wire multipart →
 * `decode_router_recv`, comes out the other side with:
 *
 *   - Byte-identical values for every field the encoder was given.
 *   - The security triple (`client_nonce`, `client_wall_ts`, `envelope_hash`)
 *     present on REG-family messages, matching the values the encoder saw
 *     (or synthesized from context for the two triple members that the
 *     encoder adds).
 *   - `envelope_hash` reconstructable from the envelope's own fields.
 *   - `env.identity()` equal to the encoder's `dealer_role_uid`.
 *   - `env.correlation_id()` equal to the encoder's `correlation_id`.
 *
 * The purpose: guarantee that when Tier 2 swaps BRC's `do_request` to use
 * `encode_dealer_send` and broker's dispatch to use `decode_router_recv`,
 * the LEGACY handler bodies see the SAME JSON they see today (plus the
 * envelope-added security fields).  If any semantic drift exists, these
 * tests catch it before wire code changes.
 *
 * See also: `test_wire_envelope.cpp` (envelope-layer edge cases),
 * `test_admission_gates.cpp` (security-triple validation).
 */

#include "utils/wire_adapter.hpp"
#include "utils/wire_bodies.hpp"
#include "utils/wire_envelope.hpp"
#include "utils/logger.hpp"

#include "binary_lifecycle.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "cppzmq/zmq_addon.hpp"

#include <cstdint>
#include <string>

PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule())

using pylabhub::wire::adapter::EncodeContext;
using pylabhub::wire::adapter::encode_dealer_send;
using pylabhub::wire::adapter::decode_router_recv;
using pylabhub::wire::adapter::msg_type_carries_security_triple;
using pylabhub::wire::WireBodyError;
using pylabhub::wire::WireEnvelope;

// ── Round-trip helpers ─────────────────────────────────────────────────

namespace
{

/// Encode a payload, then decode it — as if it went over a real ROUTER
/// hop.  Returns the decoded message; ASSERT_TRUE at the caller if you
/// want an early exit on decode failure.
[[nodiscard]] std::optional<pylabhub::wire::adapter::DecodedRouterMsg>
roundtrip(std::string_view                   msg_type,
          const EncodeContext               &ctx,
          nlohmann::json                     payload)
{
    zmq::multipart_t wire =
        encode_dealer_send(msg_type, ctx, std::move(payload));

    // On the actual ROUTER hop, libzmq attaches the DEALER's routing_id
    // as Frame 0.  We simulate that here — the encoder side does NOT
    // put Frame 0 on the wire (it's `build_dealer_send`); we prepend it
    // to match what parse_router_recv expects to see.
    zmq::multipart_t router_view;
    router_view.addstr(std::string(ctx.dealer_role_uid));
    while (!wire.empty())
    {
        router_view.add(wire.pop());
    }

    return decode_router_recv(std::move(router_view));
}

/// Default EncodeContext for REG-family tests.  All fields non-empty so
/// preconditions pass; individual tests override as needed.
EncodeContext default_reg_ctx()
{
    return EncodeContext{
        /*dealer_role_uid*/  "test.role.uid00000001",
        /*correlation_id*/   "test-correlation-00001",
        /*client_nonce*/     "test-nonce-abcdefghij",
        /*client_wall_ts*/   1'700'000'000'000ULL,
    };
}

/// Default context for non-REG-family messages (no security triple).
EncodeContext default_query_ctx()
{
    return EncodeContext{
        /*dealer_role_uid*/  "test.role.uid00000001",
        /*correlation_id*/   "test-correlation-00002",
        /*client_nonce*/     "",
        /*client_wall_ts*/   0,
    };
}

/// Build a REG_REQ payload that exactly matches what
/// `BrokerRequestComm::register_channel` produces at BRC-send time.
/// Field list drawn from broker_service.cpp handle_reg_req + HEP-CORE-0046
/// §14.3 RegReqBody schema.
nlohmann::json build_current_reg_req_payload()
{
    nlohmann::json p;
    p["channel_name"]     = "lab.test.channel";
    p["role_uid"]         = "test.role.uid00000001";
    p["role_type"]        = "producer";
    p["role_name"]        = "TestProducer";
    p["channel_topology"] = "fan-in";
    p["data_transport"]   = "zmq";
    p["zmq_pubkey"]       = "yg$m){l]+lK!u{CGx0n*vd17T1K4-Ky5Z9qXigWf1zZ";
    p["schema_hash"]      = "abcdef1234567890";
    p["schema_id"]        = "test.schema.v1";
    p["schema_blds"]      = "";
    p["schema_owner"]     = "";
    p["abi_fingerprint"]  = nlohmann::json::object();
    return p;
}

nlohmann::json build_current_endpoint_update_req_payload()
{
    nlohmann::json p;
    p["channel_name"]  = "lab.test.channel";
    p["endpoint_type"] = "zmq_node";
    p["endpoint"]      = "tcp://127.0.0.1:5581";
    return p;
}

nlohmann::json build_current_dereg_req_payload()
{
    nlohmann::json p;
    p["channel_name"] = "lab.test.channel";
    p["role_uid"]     = "test.role.uid00000001";
    return p;
}

nlohmann::json build_current_get_channel_auth_req_payload()
{
    nlohmann::json p;
    p["channel_name"] = "lab.test.channel";
    p["role_uid"]     = "test.role.uid00000001";
    return p;
}

nlohmann::json build_current_check_peer_ready_req_payload()
{
    nlohmann::json p;
    p["channel_name"]    = "lab.test.channel";
    p["role_uid"]        = "test.role.uid00000001";
    p["pubkey_z85"]      = "yg$m){l]+lK!u{CGx0n*vd17T1K4-Ky5Z9qXigWf1zZ";
    p["correlation_id"]  = "unused-legacy-field";
    return p;
}

}  // namespace

// ── msg_type_carries_security_triple ───────────────────────────────────

TEST(WireAdapterFamily, RegFamilyIdentifiedByMsgType)
{
    // HEP-CORE-0046 §I-REPLAY-BOUND — the 6 msg_types that MUST carry
    // (client_nonce, client_wall_ts, envelope_hash) per the invariant.
    // Pinned as a full list here so any silent drop or addition
    // surfaces at L1 before Tier 2 wire migration touches it.  The
    // pre-Tier-2 list omitted CHANNEL_AUTH_APPLIED_REQ and
    // CONSUMER_DEREG_REQ — that omission was a bug pinned by the
    // pre-Tier-2 negative EXPECT_FALSE line for CHANNEL_AUTH_APPLIED_REQ,
    // which retired 2026-07-13 when the list was corrected.
    EXPECT_TRUE(msg_type_carries_security_triple("REG_REQ"));
    EXPECT_TRUE(msg_type_carries_security_triple("CONSUMER_REG_REQ"));
    EXPECT_TRUE(msg_type_carries_security_triple("ENDPOINT_UPDATE_REQ"));
    EXPECT_TRUE(msg_type_carries_security_triple("CHANNEL_AUTH_APPLIED_REQ"));
    EXPECT_TRUE(msg_type_carries_security_triple("DEREG_REQ"));
    EXPECT_TRUE(msg_type_carries_security_triple("CONSUMER_DEREG_REQ"));

    EXPECT_FALSE(msg_type_carries_security_triple("GET_CHANNEL_AUTH_REQ"));
    EXPECT_FALSE(msg_type_carries_security_triple("CHECK_PEER_READY_REQ"));
    EXPECT_FALSE(msg_type_carries_security_triple(""));
    EXPECT_FALSE(msg_type_carries_security_triple("HEARTBEAT_NOTIFY"));
    // NOTIFYs and ACKs are not REG-family per §I-MSG-TYPE-TAXONOMY.
    EXPECT_FALSE(msg_type_carries_security_triple("CHANNEL_AUTH_CHANGED_NOTIFY"));
    EXPECT_FALSE(msg_type_carries_security_triple("REG_ACK"));
}

// ── REG-family round-trip: fields preserved + security triple injected ─

TEST(WireAdapterRoundtrip, RegReq_FieldsPreservedAndTripleInjected)
{
    const auto original = build_current_reg_req_payload();
    const auto ctx      = default_reg_ctx();

    auto decoded = roundtrip("REG_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value())
        << "encode/decode round-trip must succeed for a well-formed REG_REQ";

    EXPECT_EQ(decoded->msg_type, "REG_REQ");
    EXPECT_EQ(decoded->env.identity(), ctx.dealer_role_uid);
    EXPECT_EQ(decoded->env.correlation_id(), ctx.correlation_id);

    const auto &body = decoded->body_for_legacy_handler;

    // Every field the encoder was given is present byte-identical.  This
    // is the load-bearing semantic guarantee: LEGACY handlers, when Tier 2
    // swaps the wire, will see exactly what they see today.
    for (const auto &item : original.items())
    {
        ASSERT_TRUE(body.contains(item.key()))
            << "field '" << item.key() << "' lost in round-trip";
        EXPECT_EQ(body.at(item.key()), item.value())
            << "field '" << item.key() << "' value drift in round-trip";
    }

    // Security triple was injected by the encoder from the context.
    EXPECT_TRUE(body.contains("client_nonce"));
    EXPECT_EQ(body.at("client_nonce"), std::string(ctx.client_nonce));

    EXPECT_TRUE(body.contains("client_wall_ts"));
    EXPECT_EQ(body.at("client_wall_ts"), ctx.client_wall_ts);

    // envelope_hash was stamped by build_dealer_send and preserved on decode.
    ASSERT_TRUE(body.contains("envelope_hash"));
    const auto expected_hash = WireEnvelope::compute_envelope_hash(
        ctx.dealer_role_uid, "REG_REQ", ctx.correlation_id);
    EXPECT_EQ(body.at("envelope_hash").get<std::string>(), expected_hash);
}

TEST(WireAdapterRoundtrip, ConsumerRegReq_FieldsPreservedAndTripleInjected)
{
    auto original = build_current_reg_req_payload();
    original["role_type"] = "consumer";
    const auto ctx = default_reg_ctx();

    auto decoded = roundtrip("CONSUMER_REG_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->msg_type, "CONSUMER_REG_REQ");
    const auto &body = decoded->body_for_legacy_handler;
    for (const auto &item : original.items())
    {
        ASSERT_TRUE(body.contains(item.key()));
        EXPECT_EQ(body.at(item.key()), item.value());
    }
    EXPECT_TRUE(body.contains("client_nonce"));
    EXPECT_TRUE(body.contains("client_wall_ts"));
    EXPECT_TRUE(body.contains("envelope_hash"));
}

TEST(WireAdapterRoundtrip, DeregReq_FieldsPreservedAndTripleInjected)
{
    const auto original = build_current_dereg_req_payload();
    const auto ctx      = default_reg_ctx();

    auto decoded = roundtrip("DEREG_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value());

    const auto &body = decoded->body_for_legacy_handler;
    for (const auto &item : original.items())
    {
        ASSERT_TRUE(body.contains(item.key()));
        EXPECT_EQ(body.at(item.key()), item.value());
    }
    EXPECT_TRUE(body.contains("client_nonce"));
    EXPECT_TRUE(body.contains("client_wall_ts"));
    EXPECT_TRUE(body.contains("envelope_hash"));
}

TEST(WireAdapterRoundtrip, EndpointUpdateReq_FieldsPreservedAndTripleInjected)
{
    const auto original = build_current_endpoint_update_req_payload();
    const auto ctx      = default_reg_ctx();

    auto decoded = roundtrip("ENDPOINT_UPDATE_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value());

    const auto &body = decoded->body_for_legacy_handler;
    for (const auto &item : original.items())
    {
        ASSERT_TRUE(body.contains(item.key()));
        EXPECT_EQ(body.at(item.key()), item.value());
    }
    EXPECT_TRUE(body.contains("client_nonce"));
    EXPECT_TRUE(body.contains("client_wall_ts"));
    EXPECT_TRUE(body.contains("envelope_hash"));
}

// ── Non-REG-family round-trip: only envelope_hash added ────────────────

TEST(WireAdapterRoundtrip, GetChannelAuthReq_FieldsPreservedNoSecurityTriple)
{
    const auto original = build_current_get_channel_auth_req_payload();
    const auto ctx      = default_query_ctx();

    auto decoded = roundtrip("GET_CHANNEL_AUTH_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value());

    const auto &body = decoded->body_for_legacy_handler;
    for (const auto &item : original.items())
    {
        ASSERT_TRUE(body.contains(item.key()));
        EXPECT_EQ(body.at(item.key()), item.value());
    }
    // Non-REG-family: encoder must NOT inject security-triple fields.
    EXPECT_FALSE(body.contains("client_nonce"))
        << "encoder should not add client_nonce to non-REG-family msg_types";
    EXPECT_FALSE(body.contains("client_wall_ts"))
        << "encoder should not add client_wall_ts to non-REG-family msg_types";
    // envelope_hash is universal.
    EXPECT_TRUE(body.contains("envelope_hash"));
}

TEST(WireAdapterRoundtrip, CheckPeerReadyReq_FieldsPreservedNoSecurityTriple)
{
    const auto original = build_current_check_peer_ready_req_payload();
    const auto ctx      = default_query_ctx();

    auto decoded = roundtrip("CHECK_PEER_READY_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value());

    const auto &body = decoded->body_for_legacy_handler;
    for (const auto &item : original.items())
    {
        ASSERT_TRUE(body.contains(item.key()));
        EXPECT_EQ(body.at(item.key()), item.value());
    }
    EXPECT_FALSE(body.contains("client_nonce"));
    EXPECT_FALSE(body.contains("client_wall_ts"));
    EXPECT_TRUE(body.contains("envelope_hash"));
}

// ── Idempotence: caller-supplied triple values preserved on retry ─────

TEST(WireAdapterRoundtrip, RegReq_CallerSuppliedTriplePreservedOnRetry)
{
    auto original = build_current_reg_req_payload();
    // Caller pre-populates the triple (simulates a retry using the SAME
    // client_nonce so the broker's replay-bound gate lets it through).
    original["client_nonce"]   = "caller-supplied-nonce";
    original["client_wall_ts"] = 1'700'000'999'999ULL;

    // Context has DIFFERENT triple values.  Encoder must PREFER the
    // caller's pre-populated values (idempotent retry semantics).
    auto ctx           = default_reg_ctx();
    ctx.client_nonce   = "context-nonce-different";
    ctx.client_wall_ts = 1'700'000'000'000ULL;

    auto decoded = roundtrip("REG_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value());

    const auto &body = decoded->body_for_legacy_handler;
    EXPECT_EQ(body.at("client_nonce").get<std::string>(),
              "caller-supplied-nonce")
        << "encoder must preserve caller-supplied client_nonce on retry";
    EXPECT_EQ(body.at("client_wall_ts").get<std::uint64_t>(),
              1'700'000'999'999ULL);
}

// ── Envelope-layer failure modes surface via decode_router_recv ──────

TEST(WireAdapterRoundtrip, DecodeRejectsTamperedEnvelopeHash)
{
    const auto original = build_current_reg_req_payload();
    const auto ctx      = default_reg_ctx();

    zmq::multipart_t wire =
        encode_dealer_send("REG_REQ", ctx, original);
    // Simulate the ROUTER hop.
    zmq::multipart_t router_view;
    router_view.addstr(std::string(ctx.dealer_role_uid));
    while (!wire.empty()) router_view.add(wire.pop());

    // Pull the body frame (Frame 4 = last), tamper the envelope_hash, put it back.
    auto body_msg = router_view.remove();  // takes the tail
    auto body_json = nlohmann::json::parse(body_msg.to_string_view());
    body_json["envelope_hash"] =
        std::string(64, '0');  // 64 zero chars — clearly wrong
    const std::string re = body_json.dump();
    router_view.addstr(re);

    pylabhub::wire::ParseError err{};
    auto decoded = decode_router_recv(std::move(router_view), &err);
    EXPECT_FALSE(decoded.has_value())
        << "tampered envelope_hash MUST be rejected by decode_router_recv";
    EXPECT_EQ(err, pylabhub::wire::ParseError::envelope_hash_mismatch);
}

TEST(WireAdapterRoundtrip, EncodeThrowsOnEmptyDealerIdentity)
{
    auto ctx = default_reg_ctx();
    ctx.dealer_role_uid = std::string_view{};

    EXPECT_THROW(
        encode_dealer_send("REG_REQ", ctx, build_current_reg_req_payload()),
        WireBodyError);
}

TEST(WireAdapterRoundtrip, EncodeThrowsOnEmptyCorrelationForNonNotify)
{
    auto ctx = default_reg_ctx();
    ctx.correlation_id = std::string_view{};

    EXPECT_THROW(
        encode_dealer_send("REG_REQ", ctx, build_current_reg_req_payload()),
        WireBodyError);
}

TEST(WireAdapterRoundtrip, EncodeThrowsOnMissingNonceForRegFamily)
{
    auto ctx = default_reg_ctx();
    ctx.client_nonce = std::string_view{};

    EXPECT_THROW(
        encode_dealer_send("REG_REQ", ctx, build_current_reg_req_payload()),
        WireBodyError);
}

TEST(WireAdapterRoundtrip, EncodeThrowsOnZeroWallTsForRegFamily)
{
    auto ctx = default_reg_ctx();
    ctx.client_wall_ts = 0;

    EXPECT_THROW(
        encode_dealer_send("REG_REQ", ctx, build_current_reg_req_payload()),
        WireBodyError);
}

// ── Envelope identity matches encoder's dealer_role_uid ─────────────────

TEST(WireAdapterRoundtrip, EnvelopeIdentityEqualsDealerRoleUid)
{
    const auto original = build_current_reg_req_payload();
    auto ctx = default_reg_ctx();
    ctx.dealer_role_uid = "distinct.role.uid00000042";

    auto decoded = roundtrip("REG_REQ", ctx, original);
    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->env.identity(), std::string_view("distinct.role.uid00000042"))
        << "envelope identity MUST equal DEALER's routing_id (I-DEALER-IDENTITY)";
}
