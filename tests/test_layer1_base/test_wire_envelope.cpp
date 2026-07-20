// tests/test_layer1_base/test_wire_envelope.cpp
//
// L1 tests for the typed wire envelope + body classes.
// Locks:
//   - I-WIRE-ENVELOPE:      5-frame skeleton + typed body
//   - I-CORRELATION-STABLE: empty correlation_id rejected on REQ, allowed on _NOTIFY
//   - I-ENVELOPE-BODY-BINDING: envelope_hash stamped/validated
//   - I-DEALER-IDENTITY:    identity contributes to envelope_hash
//   - I-REPLAY-BOUND:       REG-family body classes require nonce + wall_ts
//
// Round-trip build → parse recovers all skeleton fields; tampering with
// any of {identity, msg_type, correlation_id, body} at parse time surfaces
// as envelope_hash_mismatch.

#include "utils/wire_envelope.hpp"
#include "utils/wire_bodies.hpp"

#include "cppzmq/zmq_addon.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

using pylabhub::wire::WireEnvelope;
using pylabhub::wire::ParseError;
using pylabhub::wire::WireBodyError;
using pylabhub::wire::is_notify_msg_type;

namespace
{

// Build a minimal RegReqBody-shaped json (all required fields present).
// The envelope_hash is filled by build_dealer_send — callers do not
// pre-stamp it.
nlohmann::json make_reg_req_body()
{
    nlohmann::json body;
    body["channel_name"]    = "lab.test.channel";
    body["role_uid"]        = "prod.test.uid1";
    body["role_type"]       = "producer";
    body["role_name"]       = "prod-1";
    body["channel_topology"] = "one-to-one";
    body["data_transport"]  = "zmq";
    body["zmq_pubkey"]      = "abcdefghij0123456789abcdefghij0123456789";
    body["schema_hash"]     = "deadbeef";
    body["schema_id"]       = "";
    body["schema_blds"]     = "";
    body["schema_owner"]    = "";
    body["abi_fingerprint"] = nlohmann::json::object();
    body["client_nonce"]    = "0123456789abcdef0123456789abcdef";
    body["client_wall_ts"]  = static_cast<std::uint64_t>(1234567890000ULL);
    return body;
}

}  // namespace

// ── Notify-suffix detector ────────────────────────────────────────────

TEST(WireEnvelope, IsNotifyMsgType)
{
    EXPECT_TRUE(is_notify_msg_type("CHANNEL_AUTH_CHANGED_NOTIFY"));
    EXPECT_TRUE(is_notify_msg_type("BAND_JOIN_NOTIFY"));
    EXPECT_FALSE(is_notify_msg_type("REG_REQ"));
    EXPECT_FALSE(is_notify_msg_type("REG_ACK"));
    EXPECT_FALSE(is_notify_msg_type(""));
    EXPECT_FALSE(is_notify_msg_type("_NOTIFY"));  // only-suffix, no head
}

// ── Envelope hash determinism ─────────────────────────────────────────

TEST(WireEnvelope, EnvelopeHashDeterministic)
{
    auto h1 = WireEnvelope::compute_envelope_hash("id-1", "REG_REQ", "cid-42");
    auto h2 = WireEnvelope::compute_envelope_hash("id-1", "REG_REQ", "cid-42");
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 64U);  // 32-byte BLAKE2b-256 → 64 hex chars
}

TEST(WireEnvelope, EnvelopeHashDiffersOnAnyInputChange)
{
    auto base = WireEnvelope::compute_envelope_hash("id-1", "REG_REQ", "cid-42");
    EXPECT_NE(base,
              WireEnvelope::compute_envelope_hash("id-2", "REG_REQ", "cid-42"));
    EXPECT_NE(base,
              WireEnvelope::compute_envelope_hash("id-1", "REG_ACK", "cid-42"));
    EXPECT_NE(base,
              WireEnvelope::compute_envelope_hash("id-1", "REG_REQ", "cid-43"));
}

// ── Round-trip: build → parse (ROUTER side) ───────────────────────────

TEST(WireEnvelope, RouterRoundTripRecoversAllSkeletonFields)
{
    auto body = make_reg_req_body();
    auto frames = WireEnvelope::build_router_send(
        /*target_identity=*/"prod.test.uid1", /*msg_type=*/"REG_REQ",
        /*correlation_id=*/"cid-1", body);

    ASSERT_EQ(frames.size(), 5U);

    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value())
        << "parse failed: err=" << static_cast<int>(err);

    EXPECT_EQ(env->identity(),       "prod.test.uid1");
    EXPECT_EQ(env->msg_type(),       "REG_REQ");
    EXPECT_EQ(env->correlation_id(), "cid-1");
    EXPECT_FALSE(env->is_notify());
    EXPECT_TRUE(env->body().is_object());
    EXPECT_EQ(env->body().at("channel_name").get<std::string>(),
              "lab.test.channel");
}

// ── Round-trip: DEALER side (Frame 0 is not in the multipart) ─────────

TEST(WireEnvelope, DealerRoundTripUsesDealersOwnRoutingId)
{
    auto body = make_reg_req_body();
    // Per I-DEALER-IDENTITY the DEALER's routing_id equals its owning
    // role's role_uid.  build_dealer_send stamps envelope_hash over
    // that value; parse_dealer_recv reconstructs the hash over the
    // SAME value (which the DEALER knows because it set its own
    // routing_id at connect()).  In production the DEALER caller
    // passes its own role_uid.  Here we pin the round-trip.
    const std::string dealer_own_routing_id = "prod.test.uid1";

    auto frames = WireEnvelope::build_dealer_send(
        dealer_own_routing_id, /*msg_type=*/"REG_ACK",
        /*correlation_id=*/"cid-1", body);
    ASSERT_EQ(frames.size(), 4U);

    ParseError err{};
    auto env = WireEnvelope::parse_dealer_recv(std::move(frames),
                                                 dealer_own_routing_id, &err);
    ASSERT_TRUE(env.has_value())
        << "parse failed: err=" << static_cast<int>(err);
    EXPECT_EQ(env->identity(),       dealer_own_routing_id);
    EXPECT_EQ(env->msg_type(),       "REG_ACK");
    EXPECT_EQ(env->correlation_id(), "cid-1");
}

// ── I-CORRELATION-STABLE: empty correlation_id policy ─────────────────

TEST(WireEnvelope, EmptyCorrelationRejectedOnReq)
{
    auto body = make_reg_req_body();
    EXPECT_THROW(WireEnvelope::build_dealer_send(
                     "prod.test.uid1", "REG_REQ", "", body),
                 std::invalid_argument);
}

TEST(WireEnvelope, EmptyCorrelationAllowedOnNotify)
{
    nlohmann::json body;
    body["channel_name"]    = "lab.test.channel";
    body["role_uid"]        = "prod.test.uid1";
    body["role_type"]       = "producer";
    body["phase"]           = "admitted";
    body["channel_version"] = 1ULL;

    // Build with empty correlation_id — legal for NOTIFY per
    // I-CORRELATION-STABLE / I-MSG-TYPE-TAXONOMY.
    auto frames = WireEnvelope::build_router_send(
        /*target=*/"cons.test.uid1", /*msg_type=*/"CHANNEL_AUTH_CHANGED_NOTIFY",
        /*correlation_id=*/"", body);

    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value())
        << "parse failed: err=" << static_cast<int>(err);
    EXPECT_TRUE(env->is_notify());
    EXPECT_EQ(env->correlation_id(), "");
}

// ── I-ENVELOPE-BODY-BINDING: tamper detection ─────────────────────────

TEST(WireEnvelope, TamperingIdentityFailsHashCheck)
{
    // Build with identity A, then pretend at parse time the frame is
    // identity B — envelope_hash was computed over A, so the receiver
    // (which recomputes over B) sees a mismatch.
    auto body = make_reg_req_body();
    auto frames = WireEnvelope::build_router_send("identity-A", "REG_REQ",
                                                    "cid-1", body);
    ASSERT_EQ(frames.size(), 5U);

    // Overwrite Frame 0 to a different identity.
    const std::string tampered = "identity-B";
    frames[0].rebuild(tampered.data(), tampered.size());

    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::envelope_hash_mismatch);
}

TEST(WireEnvelope, TamperingBodyFailsHashCheck)
{
    // Body field can be tampered without touching skeleton frames, but
    // the envelope_hash the body carries was stamped for the original
    // body; parse recomputes envelope_hash over the skeleton frames and
    // compares to body.envelope_hash — mismatch surfaces if the tamperer
    // ALSO modifies envelope_hash to match, which requires the private
    // key material.  Simulating a modify-body-only tamper here (attacker
    // splices a different body but leaves envelope_hash intact from
    // some other message):
    auto body_a = make_reg_req_body();
    auto frames_a = WireEnvelope::build_router_send("identity-A", "REG_REQ",
                                                      "cid-1", body_a);

    // Build a second message with a different msg_type; its body's
    // envelope_hash is stamped for that msg_type.  Steal the body from
    // message B and put it into frames_a's body frame.
    auto body_b = make_reg_req_body();
    auto frames_b = WireEnvelope::build_router_send("identity-A", "REG_ACK",
                                                      "cid-1", body_b);
    // frames_b[4] is the stolen body (has envelope_hash stamped for REG_ACK).
    zmq::message_t stolen_body;
    stolen_body.copy(frames_b[4]);
    frames_a[4] = std::move(stolen_body);

    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames_a), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::envelope_hash_mismatch);
}

// ── Frame-count / marker enforcement ──────────────────────────────────

TEST(WireEnvelope, WrongFrameCountRejected)
{
    zmq::multipart_t frames;
    frames.addstr("only-one-frame");
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::frame_count);
}

TEST(WireEnvelope, WrongMarkerRejected)
{
    zmq::multipart_t frames;
    frames.addstr("identity-A");
    frames.addstr("X");  // should be 'C'
    frames.addstr("REG_REQ");
    frames.addstr("cid-1");
    frames.addstr("{}");
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::frame_type_marker);
}

// ── Body-class shape validation ───────────────────────────────────────

TEST(WireBodies, RegReqBodyValidatesRequiredFields)
{
    // Pins every §14.3 RegReqBody field accessor.  Prior version only
    // spot-checked 8 of 15 fields, letting an accessor rename or
    // wrong-field-mapping slip past review.  Design mandate: every
    // §14.3 field is accessible via a typed accessor.
    auto body_json = make_reg_req_body();
    body_json["envelope_hash"] =
        WireEnvelope::compute_envelope_hash("id-x", "REG_REQ", "cid-1");

    pylabhub::wire::ProducerRegReqBody body(body_json);
    EXPECT_EQ(body.channel_name(),     "lab.test.channel");
    EXPECT_EQ(body.role_uid(),         "prod.test.uid1");
    EXPECT_EQ(body.role_type(),        "producer");
    EXPECT_EQ(body.role_name(),        "prod-1");
    EXPECT_EQ(body.channel_topology(), "one-to-one");
    EXPECT_EQ(body.data_transport(),   "zmq");
    EXPECT_EQ(body.zmq_pubkey(),
              "abcdefghij0123456789abcdefghij0123456789");
    // broker_proto retired per C3.
    EXPECT_EQ(body.schema_hash(),      "deadbeef");
    // schema_version retired per C2 — version rides inside schema_id.
    EXPECT_EQ(body.schema_id(),        "");
    EXPECT_EQ(body.schema_blds(),      "");
    EXPECT_EQ(body.schema_owner(),     "");
    EXPECT_TRUE(body.abi_fingerprint().is_object());
    EXPECT_EQ(body.client_nonce(),
              "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(body.client_wall_ts(), 1234567890000ULL);
    EXPECT_FALSE(body.envelope_hash().empty());
}

TEST(WireBodies, RegReqBodyRejectsMissingClientNonce)
{
    // Design mandate (I-REPLAY-BOUND): REG-family bodies MUST carry
    // client_nonce.  A missing field is a wire-shape violation.
    auto body_json = make_reg_req_body();
    body_json["envelope_hash"] = "deadbeef";
    body_json.erase("client_nonce");
    EXPECT_THROW(pylabhub::wire::ProducerRegReqBody{std::move(body_json)},
                 WireBodyError);
}

TEST(WireBodies, RegReqBodyRejectsMissingClientWallTs)
{
    // Design mandate (I-REPLAY-BOUND): REG-family bodies MUST carry
    // client_wall_ts.  Prior version only tested the missing-nonce
    // path, leaving missing-wall_ts uncovered.
    auto body_json = make_reg_req_body();
    body_json["envelope_hash"] = "deadbeef";
    body_json.erase("client_wall_ts");
    EXPECT_THROW(pylabhub::wire::ProducerRegReqBody{std::move(body_json)},
                 WireBodyError);
}

TEST(WireBodies, RegReqBodyRejectsMissingEnvelopeHash)
{
    auto body_json = make_reg_req_body();  // has security triple, no envelope_hash
    EXPECT_THROW(pylabhub::wire::ProducerRegReqBody{std::move(body_json)},
                 WireBodyError);
}

TEST(WireBodies, ChannelAuthChangedNotifyBodyValidates)
{
    nlohmann::json body;
    body["channel_name"]    = "lab.x";
    body["role_uid"]        = "prod.x";
    body["role_type"]       = "producer";
    body["phase"]           = "admitted";
    body["channel_version"] = 5ULL;
    body["envelope_hash"]   = "deadbeef";

    pylabhub::wire::ChannelAuthChangedNotifyBody parsed(std::move(body));
    EXPECT_EQ(parsed.channel_name(),   "lab.x");
    EXPECT_EQ(parsed.role_uid(),       "prod.x");
    EXPECT_EQ(parsed.role_type(),      "producer");
    EXPECT_EQ(parsed.phase(),          "admitted");
    EXPECT_EQ(parsed.channel_version(), 5ULL);
}

// ── Design-derived tests: RegAckBody (§14.3) ──────────────────────────
//
// Design mandates fields: status, error_code, message, channel_name,
// instance_id, snapshot_version, heartbeat, initial_allowlist,
// broker_abi_fingerprint, broker_build_id, broker_observer_pubkey_z85
// + envelope_hash only (no security triple — ACK doesn't mutate state).

TEST(WireBodies, RegAckBodyValidatesAllFields)
{
    nlohmann::json body;
    body["status"]                     = "success";
    body["error_code"]                 = "";
    body["message"]                    = "";
    body["channel_name"]               = "lab.x";
    body["instance_id"]                = 3ULL;
    body["snapshot_version"]           = 7ULL;
    body["heartbeat"]                  = nlohmann::json::object();
    body["initial_allowlist"]          = nlohmann::json::array();
    body["broker_abi_fingerprint"]     = nlohmann::json::object();
    body["broker_build_id"]            = "build-abc";
    body["broker_observer_pubkey_z85"] = "";
    body["envelope_hash"]              = "deadbeef";

    pylabhub::wire::RegAckBody ack(std::move(body));
    EXPECT_EQ(ack.status(),           "success");
    EXPECT_EQ(ack.error_code(),       "");
    EXPECT_EQ(ack.message(),          "");
    EXPECT_EQ(ack.channel_name(),     "lab.x");
    EXPECT_EQ(ack.instance_id(),      3ULL);
    EXPECT_EQ(ack.snapshot_version(), 7ULL);
    EXPECT_TRUE(ack.heartbeat().is_object());
    EXPECT_TRUE(ack.initial_allowlist().is_array());
    EXPECT_TRUE(ack.broker_abi_fingerprint().is_object());
    EXPECT_EQ(ack.broker_build_id(),  "build-abc");
    EXPECT_EQ(ack.envelope_hash(),    "deadbeef");
}

TEST(WireBodies, RegAckBodyRejectsMissingStatus)
{
    // Design: status is required (success | error).
    nlohmann::json body;
    body["channel_name"]           = "lab.x";
    body["heartbeat"]              = nlohmann::json::object();
    body["initial_allowlist"]      = nlohmann::json::array();
    body["broker_abi_fingerprint"] = nlohmann::json::object();
    body["envelope_hash"]          = "deadbeef";
    // status omitted intentionally
    EXPECT_THROW(pylabhub::wire::RegAckBody{std::move(body)},
                 WireBodyError);
}

TEST(WireBodies, RegAckBodyRejectsMissingInitialAllowlist)
{
    // Design: initial_allowlist is a required array on RegAckBody.
    nlohmann::json body;
    body["status"]                 = "success";
    body["channel_name"]           = "lab.x";
    body["heartbeat"]              = nlohmann::json::object();
    body["broker_abi_fingerprint"] = nlohmann::json::object();
    body["envelope_hash"]          = "deadbeef";
    // initial_allowlist omitted
    EXPECT_THROW(pylabhub::wire::RegAckBody{std::move(body)},
                 WireBodyError);
}

// ── Design-derived tests: HeartbeatNotifyBody (§14.3) ─────────────────
//
// C5 renamed HEARTBEAT_REQ → HEARTBEAT_NOTIFY (fire-and-forget presence
// signal; no ACK).  Design mandates: channel_name + role_uid + role_type
// + envelope_hash (no security triple — presence maintenance, not state
// mutation).

TEST(WireBodies, HeartbeatNotifyBodyValidatesFields)
{
    nlohmann::json body;
    body["channel_name"]  = "lab.x";
    body["role_uid"]      = "prod.x";
    body["role_type"]     = "producer";
    body["envelope_hash"] = "deadbeef";

    pylabhub::wire::HeartbeatNotifyBody hb(std::move(body));
    EXPECT_EQ(hb.channel_name(),  "lab.x");
    EXPECT_EQ(hb.role_uid(),      "prod.x");
    EXPECT_EQ(hb.role_type(),     "producer");
    EXPECT_EQ(hb.envelope_hash(), "deadbeef");
}

TEST(WireBodies, HeartbeatNotifyBodyRejectsMissingChannelName)
{
    nlohmann::json body;
    body["role_uid"]      = "prod.x";
    body["role_type"]     = "producer";
    body["envelope_hash"] = "deadbeef";
    EXPECT_THROW(pylabhub::wire::HeartbeatNotifyBody{std::move(body)},
                 WireBodyError);
}

// ── Design-derived parse-error path tests (§14.1 length limits;
//    §14.5 gate 1 correlation + envelope_hash mandates) ──────────────
//
// ParseError variants: frame_count, frame_type_marker, msg_type_empty,
// msg_type_too_long, correlation_too_long, correlation_missing,
// body_not_json, body_not_object, envelope_hash_missing,
// envelope_hash_mismatch.  Prior tests covered 4; the remaining 6 are
// all direct §14.1 / §14.5 gate 1 mandates.

TEST(WireEnvelope, ParseRejectsCorrelationMissingOnNonNotify)
{
    // §14.5 gate 1 / I-CORRELATION-STABLE: empty correlation_id on
    // non-NOTIFY msg_type is a wire violation.  The test constructs
    // frames by hand (bypassing build_router_send's validation) to
    // exercise the PARSE-side check.
    zmq::multipart_t frames;
    frames.addstr("prod.uid1");
    const char marker = 'C';
    frames.addmem(&marker, 1);
    frames.addstr("REG_REQ");     // not a NOTIFY
    frames.addstr("");            // empty correlation_id — illegal here
    frames.addstr("{}");
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::correlation_missing);
}

TEST(WireEnvelope, ParseRejectsBodyNotJson)
{
    // §14.1 Frame 4 must be JSON.  Malformed JSON should surface as
    // body_not_json, not silently swallowed.
    zmq::multipart_t frames;
    frames.addstr("prod.uid1");
    const char marker = 'C';
    frames.addmem(&marker, 1);
    frames.addstr("REG_REQ");
    frames.addstr("cid-1");
    frames.addstr("not-json-at-all-{");
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::body_not_json);
}

TEST(WireEnvelope, ParseRejectsBodyNotObject)
{
    // §14.1 body is a JSON object (per §14.3 body class contract).
    // A JSON array or scalar is not an object → distinct reject.
    zmq::multipart_t frames;
    frames.addstr("prod.uid1");
    const char marker = 'C';
    frames.addmem(&marker, 1);
    frames.addstr("REG_REQ");
    frames.addstr("cid-1");
    frames.addstr("[1,2,3]");  // valid JSON but not an object
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::body_not_object);
}

TEST(WireEnvelope, ParseRejectsEnvelopeHashMissing)
{
    // §14.5 gate 1 / I-ENVELOPE-BODY-BINDING: body must carry
    // envelope_hash.  A body without the field is a wire violation.
    zmq::multipart_t frames;
    frames.addstr("prod.uid1");
    const char marker = 'C';
    frames.addmem(&marker, 1);
    frames.addstr("REG_REQ");
    frames.addstr("cid-1");
    frames.addstr("{\"foo\":\"bar\"}");  // valid object, no envelope_hash
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::envelope_hash_missing);
}

TEST(WireEnvelope, ParseRejectsMsgTypeEmpty)
{
    // §14.1 Frame 2: msg_type is non-empty ASCII.
    zmq::multipart_t frames;
    frames.addstr("prod.uid1");
    const char marker = 'C';
    frames.addmem(&marker, 1);
    frames.addstr("");            // empty msg_type
    frames.addstr("cid-1");
    frames.addstr("{}");
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::msg_type_empty);
}

TEST(WireEnvelope, ParseRejectsMsgTypeTooLong)
{
    // §14.1 Frame 2: msg_type ≤ 64 bytes.  A 65-char msg_type is a
    // wire violation.
    zmq::multipart_t frames;
    frames.addstr("prod.uid1");
    const char marker = 'C';
    frames.addmem(&marker, 1);
    frames.addstr(std::string(65, 'A'));   // 65 chars — exceeds limit
    frames.addstr("cid-1");
    frames.addstr("{}");
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::msg_type_too_long);
}

TEST(WireEnvelope, ParseRejectsCorrelationTooLong)
{
    // §14.1 Frame 3: correlation_id ≤ 64 bytes.
    zmq::multipart_t frames;
    frames.addstr("prod.uid1");
    const char marker = 'C';
    frames.addmem(&marker, 1);
    frames.addstr("REG_REQ");
    frames.addstr(std::string(65, 'A'));   // 65 chars — exceeds limit
    frames.addstr("{}");
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    EXPECT_FALSE(env.has_value());
    EXPECT_EQ(err, ParseError::correlation_too_long);
}

// ══════════════════════════════════════════════════════════════════════
// Design-derived tests: remaining §14.3 body class catalog.
// Each test's assertion set is derived from the §14.3 field list for
// that body class, NOT from the wire_bodies.hpp accessor list.  A test
// that fails after a field renames or a schema drift is a bug in the
// code; a test that passes on a mangled catalog would be a bug in the
// test — hence design-first.
// ══════════════════════════════════════════════════════════════════════

// ── EndpointUpdateReqBody: channel_name, endpoint_type, endpoint +
//    security triple (REG-family: mutates data_endpoint state).
TEST(WireBodies, EndpointUpdateReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["channel_name"]   = "lab.x";
    body["endpoint_type"]  = "zmq_node";
    body["endpoint"]       = "tcp://127.0.0.1:5555";
    body["client_nonce"]   = "0123456789abcdef0123456789abcdef";
    body["client_wall_ts"] = static_cast<std::uint64_t>(1'700'000'000'000ULL);
    body["envelope_hash"]  = "deadbeef";

    pylabhub::wire::EndpointUpdateReqBody b(std::move(body));
    EXPECT_EQ(b.channel_name(),  "lab.x");
    EXPECT_EQ(b.endpoint_type(), "zmq_node");
    EXPECT_EQ(b.endpoint(),      "tcp://127.0.0.1:5555");
    EXPECT_EQ(b.client_nonce(),  "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(b.client_wall_ts(), 1'700'000'000'000ULL);
    EXPECT_EQ(b.envelope_hash(), "deadbeef");
}

TEST(WireBodies, EndpointUpdateReqBodyRejectsMissingEndpoint)
{
    nlohmann::json body;
    body["channel_name"]   = "lab.x";
    body["endpoint_type"]  = "zmq_node";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1ULL;
    body["envelope_hash"]  = "deadbeef";
    // endpoint intentionally omitted
    EXPECT_THROW(pylabhub::wire::EndpointUpdateReqBody{std::move(body)},
                 WireBodyError);
}

// ── EndpointUpdateAckBody: status, message + envelope_hash only.
TEST(WireBodies, EndpointUpdateAckBodyValidatesAllFields)
{
    nlohmann::json body;
    body["status"]        = "success";
    body["message"]       = "endpoint stored";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::EndpointUpdateAckBody b(std::move(body));
    EXPECT_EQ(b.status(),  "success");
    EXPECT_EQ(b.message(), "endpoint stored");
}

// ── GetChannelAuthReqBody: channel_name, role_uid + envelope_hash.
TEST(WireBodies, GetChannelAuthReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["channel_name"]  = "lab.x";
    body["role_uid"]      = "cons.uid1";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::GetChannelAuthReqBody b(std::move(body));
    EXPECT_EQ(b.channel_name(), "lab.x");
    EXPECT_EQ(b.role_uid(),     "cons.uid1");
}

// ── GetChannelAuthAckBody: status, allowlist, channel_version.
TEST(WireBodies, GetChannelAuthAckBodyValidatesAllFields)
{
    nlohmann::json allowlist_arr = nlohmann::json::array();
    allowlist_arr.push_back("pubkey-A-40chars");
    allowlist_arr.push_back("pubkey-B-40chars");
    nlohmann::json body;
    body["status"]          = "success";
    body["allowlist"]       = allowlist_arr;
    body["channel_version"] = 42ULL;
    body["envelope_hash"]   = "deadbeef";
    pylabhub::wire::GetChannelAuthAckBody b(std::move(body));
    EXPECT_EQ(b.status(),          "success");
    EXPECT_EQ(b.channel_version(), 42ULL);
    EXPECT_TRUE(b.allowlist().is_array());
    EXPECT_EQ(b.allowlist().size(), 2U);
}

// ── ChannelAuthAppliedReqBody: channel_name, role_uid, applied_version,
//    instance_id + security triple.
TEST(WireBodies, ChannelAuthAppliedReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["channel_name"]    = "lab.x";
    body["role_uid"]        = "cons.uid1";
    body["applied_version"] = 5ULL;
    body["instance_id"]     = 3ULL;
    body["client_nonce"]    = "n1";
    body["client_wall_ts"]  = 1ULL;
    body["envelope_hash"]   = "deadbeef";
    pylabhub::wire::ChannelAuthAppliedReqBody b(std::move(body));
    EXPECT_EQ(b.channel_name(),    "lab.x");
    EXPECT_EQ(b.role_uid(),        "cons.uid1");
    EXPECT_EQ(b.applied_version(), 5ULL);
    EXPECT_EQ(b.instance_id(),     3ULL);
    EXPECT_EQ(b.client_nonce(),    "n1");
    EXPECT_EQ(b.client_wall_ts(),  1ULL);
}

TEST(WireBodies, ChannelAuthAppliedReqBodyRejectsMissingAppliedVersion)
{
    nlohmann::json body;
    body["channel_name"]   = "lab.x";
    body["role_uid"]       = "cons.uid1";
    body["instance_id"]    = 3ULL;
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1ULL;
    body["envelope_hash"]  = "deadbeef";
    // applied_version omitted
    EXPECT_THROW(pylabhub::wire::ChannelAuthAppliedReqBody{std::move(body)},
                 WireBodyError);
}

// ── ChannelAuthAppliedAckBody: status, confirmed_version.
TEST(WireBodies, ChannelAuthAppliedAckBodyValidatesAllFields)
{
    nlohmann::json body;
    body["status"]             = "success";
    body["confirmed_version"]  = 7ULL;
    body["envelope_hash"]      = "deadbeef";
    pylabhub::wire::ChannelAuthAppliedAckBody b(std::move(body));
    EXPECT_EQ(b.status(),            "success");
    EXPECT_EQ(b.confirmed_version(), 7ULL);
}

// ── DeregReqBody: channel_name, role_uid + security triple.
TEST(WireBodies, DeregReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["channel_name"]   = "lab.x";
    body["role_uid"]       = "prod.uid1";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::DeregReqBody b(std::move(body));
    EXPECT_EQ(b.channel_name(),   "lab.x");
    EXPECT_EQ(b.role_uid(),       "prod.uid1");
    EXPECT_EQ(b.client_nonce(),   "n1");
    EXPECT_EQ(b.client_wall_ts(), 1ULL);
}

TEST(WireBodies, DeregReqBodyRejectsMissingSecurityTriple)
{
    // Design (I-REPLAY-BOUND): REG-family bodies MUST carry security
    // triple.  DeregReqBody is REG-family (mutates admission state).
    nlohmann::json body;
    body["channel_name"]  = "lab.x";
    body["role_uid"]      = "prod.uid1";
    body["envelope_hash"] = "deadbeef";
    // client_nonce + client_wall_ts omitted
    EXPECT_THROW(pylabhub::wire::DeregReqBody{std::move(body)},
                 WireBodyError);
}

// ── DeregAckBody: status only.
TEST(WireBodies, DeregAckBodyValidatesAllFields)
{
    nlohmann::json body;
    body["status"]        = "success";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::DeregAckBody b(std::move(body));
    EXPECT_EQ(b.status(), "success");
}

// ── DiscReqBody: channel_name only.
TEST(WireBodies, DiscReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["channel_name"]  = "lab.x";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::DiscReqBody b(std::move(body));
    EXPECT_EQ(b.channel_name(), "lab.x");
}

// ── DiscAckBody: status + discovery payload.
TEST(WireBodies, DiscAckBodyValidatesAllFields)
{
    nlohmann::json body;
    body["status"]         = "success";
    body["envelope_hash"]  = "deadbeef";
    body["discovery"]      = nlohmann::json::object();  // payload extension
    pylabhub::wire::DiscAckBody b(std::move(body));
    EXPECT_EQ(b.status(), "success");
    // raw_body() is the DiscAckBody accessor for extensible payload.
    EXPECT_TRUE(b.raw_body().is_object());
}

// ── ChannelClosingNotifyBody: channel_name, reason.
TEST(WireBodies, ChannelClosingNotifyBodyValidatesAllFields)
{
    nlohmann::json body;
    body["channel_name"]  = "lab.x";
    body["reason"]        = "heartbeat_timeout";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::ChannelClosingNotifyBody b(std::move(body));
    EXPECT_EQ(b.channel_name(), "lab.x");
    EXPECT_EQ(b.reason(),       "heartbeat_timeout");
}

// ── ConsumerDiedNotifyBody: channel_name, role_uid, reason, target_role.
TEST(WireBodies, ConsumerDiedNotifyBodyValidatesAllFields)
{
    nlohmann::json body;
    body["channel_name"]  = "lab.x";
    body["role_uid"]      = "cons.uid1";
    body["reason"]        = "heartbeat_timeout";
    body["target_role"]   = "prod.uid1";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::ConsumerDiedNotifyBody b(std::move(body));
    EXPECT_EQ(b.channel_name(), "lab.x");
    EXPECT_EQ(b.role_uid(),     "cons.uid1");
    EXPECT_EQ(b.reason(),       "heartbeat_timeout");
    EXPECT_EQ(b.target_role(),  "prod.uid1");
}

// ── BandJoinNotifyBody / BandLeaveNotifyBody: band, role_uid, role_name.
TEST(WireBodies, BandJoinNotifyBodyValidatesAllFields)
{
    nlohmann::json body;
    body["band"]          = "band-a";
    body["role_uid"]      = "prod.uid1";
    body["role_name"]     = "prod-a";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::BandJoinNotifyBody b(std::move(body));
    EXPECT_EQ(b.band(),      "band-a");
    EXPECT_EQ(b.role_uid(),  "prod.uid1");
    EXPECT_EQ(b.role_name(), "prod-a");
}

TEST(WireBodies, BandLeaveNotifyBodyValidatesAllFields)
{
    nlohmann::json body;
    body["band"]          = "band-a";
    body["role_uid"]      = "prod.uid1";
    body["role_name"]     = "prod-a";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::BandLeaveNotifyBody b(std::move(body));
    EXPECT_EQ(b.band(),      "band-a");
    EXPECT_EQ(b.role_uid(),  "prod.uid1");
    EXPECT_EQ(b.role_name(), "prod-a");
}

// ── Admin console family (HEP-CORE-0033 §11) — typed body validation ──

TEST(WireBodies, AdminHelloReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["token"]         = "0123456789abcdef";
    body["label"]         = "alice-laptop";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::AdminHelloReqBody b(std::move(body));
    EXPECT_EQ(b.token(), "0123456789abcdef");
    EXPECT_EQ(b.label(), "alice-laptop");
    EXPECT_FALSE(b.envelope_hash().empty());
}

TEST(WireBodies, AdminHelloReqBodyRejectsMissingToken)
{
    nlohmann::json body;
    body["label"]         = "alice-laptop";
    body["envelope_hash"] = "deadbeef";
    EXPECT_THROW(pylabhub::wire::AdminHelloReqBody{std::move(body)},
                 WireBodyError);
}

TEST(WireBodies, AdminHelloReqBodyRejectsMissingEnvelopeHash)
{
    nlohmann::json body;
    body["token"] = "t";
    body["label"] = "alice-laptop";
    EXPECT_THROW(pylabhub::wire::AdminHelloReqBody{std::move(body)},
                 WireBodyError);
}

TEST(WireBodies, AdminHelloAckBodyValidatesSessionId)
{
    nlohmann::json body;
    body["session_id"]    = "abcdef0123";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::AdminHelloAckBody b(std::move(body));
    EXPECT_EQ(b.session_id(), "abcdef0123");
}

TEST(WireBodies, AdminPingReqBodyValidatesSessionId)
{
    nlohmann::json body;
    body["session_id"]     = "abcdef0123";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1721000000000ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::AdminPingReqBody b(std::move(body));
    EXPECT_EQ(b.session_id(), "abcdef0123");
}

TEST(WireBodies, AdminCloseChannelReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["session_id"]     = "abcdef0123";
    body["channel"]        = "lab.sensor.temp";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1721000000000ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::AdminCloseChannelReqBody b(std::move(body));
    EXPECT_EQ(b.session_id(), "abcdef0123");
    EXPECT_EQ(b.channel(),    "lab.sensor.temp");
}

TEST(WireBodies, AdminCloseChannelReqBodyRejectsMissingChannel)
{
    nlohmann::json body;
    body["session_id"]    = "abcdef0123";
    body["envelope_hash"] = "deadbeef";
    EXPECT_THROW(pylabhub::wire::AdminCloseChannelReqBody{std::move(body)},
                 WireBodyError);
}

TEST(WireBodies, AdminCloseChannelAckBodyValidatesStatus)
{
    nlohmann::json body;
    body["status"]        = "ok";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::AdminCloseChannelAckBody b(std::move(body));
    EXPECT_EQ(b.status(), "ok");
}

TEST(WireBodies, AdminSessionReqBodyValidatesSessionId)
{
    nlohmann::json body;
    body["session_id"]     = "sid-1";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1721000000000ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::AdminSessionReqBody b(std::move(body));
    EXPECT_EQ(b.session_id(), "sid-1");
}

TEST(WireBodies, AdminNamedReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["session_id"]     = "sid-1";
    body["name"]           = "lab.sensor.temp";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1721000000000ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::AdminNamedReqBody b(std::move(body));
    EXPECT_EQ(b.session_id(), "sid-1");
    EXPECT_EQ(b.name(),       "lab.sensor.temp");
}

TEST(WireBodies, AdminNamedReqBodyRejectsMissingName)
{
    nlohmann::json body;
    body["session_id"]    = "sid-1";
    body["envelope_hash"] = "deadbeef";
    EXPECT_THROW(pylabhub::wire::AdminNamedReqBody{std::move(body)}, WireBodyError);
}

TEST(WireBodies, AdminBroadcastChannelReqBodyValidatesAllFields)
{
    nlohmann::json body;
    body["session_id"]    = "sid-1";
    body["channel"]       = "lab.sensor.temp";
    body["message"]       = "from-operator";
    body["data"]           = "extra";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1721000000000ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::AdminBroadcastChannelReqBody b(std::move(body));
    EXPECT_EQ(b.channel(), "lab.sensor.temp");
    EXPECT_EQ(b.message(), "from-operator");
    EXPECT_EQ(b.data(),    "extra");
}

TEST(WireBodies, AdminBroadcastChannelReqBodyDataOptional)
{
    nlohmann::json body;
    body["session_id"]    = "sid-1";
    body["channel"]       = "c";
    body["message"]        = "m";
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1721000000000ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::AdminBroadcastChannelReqBody b(std::move(body));
    EXPECT_EQ(b.data(), ""); // absent → empty
}

TEST(WireBodies, AdminQueryMetricsReqBodyRequiresFilterObject)
{
    nlohmann::json body;
    body["session_id"]    = "sid-1";
    body["filter"]         = nlohmann::json::object();
    body["client_nonce"]   = "n1";
    body["client_wall_ts"] = 1721000000000ULL;
    body["envelope_hash"]  = "deadbeef";
    pylabhub::wire::AdminQueryMetricsReqBody b(std::move(body));
    EXPECT_TRUE(b.filter().is_object());
}

TEST(WireBodies, AdminResultAckBodyValidatesResultObject)
{
    nlohmann::json body;
    body["result"]        = nlohmann::json{{"channels", nlohmann::json::object()}};
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::AdminResultAckBody b(std::move(body));
    EXPECT_TRUE(b.result().is_object());
    EXPECT_TRUE(b.result().contains("channels"));
}

TEST(WireBodies, AdminErrorBodyValidatesCodeAndMessage)
{
    nlohmann::json body;
    body["code"]          = "unauthorized";
    body["message"]       = "bad session";
    body["envelope_hash"] = "deadbeef";
    pylabhub::wire::AdminErrorBody b(std::move(body));
    EXPECT_EQ(b.code(),    "unauthorized");
    EXPECT_EQ(b.message(), "bad session");
}
