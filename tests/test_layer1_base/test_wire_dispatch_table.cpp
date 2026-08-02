// tests/test_layer1_base/test_wire_dispatch_table.cpp
//
// L1 regression pin for wire_dispatch::kDispatchTable.
//
// Purpose: guard against silent row-drop or accidental msg_type/tier
// reassignment.  A dropped row loses envelope-hash gating for that
// msg_type on the receive side (falls through to UNKNOWN_MSG_TYPE);
// a wrong tier routes a msg to the wrong gate set.  Both are
// regression-invisible in end-to-end tests when the affected feature
// is exercised on a happy path.
//
// If this test fails: someone changed kDispatchTable.  Verify the
// change is intentional; update the pinned map below to match.
// The C11 attempt (2026-07-14) to drop CHANNEL_BROADCAST_SEND_NOTIFY
// is the incident that motivated this pin.

#include "utils/wire_dispatch.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <unordered_map>

namespace wd = pylabhub::wire::dispatch;

namespace
{

// Expected msg_type → tier-name mapping.  Every row in kDispatchTable
// must appear here; adding a row without updating this map is a test
// failure by construction (size mismatch below).
const std::unordered_map<std::string_view, std::string_view> kExpectedTiers = {
    // Full-gate REG-family
    {"REG_REQ", "RegReq"},
    {"CONSUMER_REG_REQ", "ConsumerRegReq"},

    // Authenticated REG-family
    {"DEREG_REQ", "AuthReg_Dereg"},
    {"CONSUMER_DEREG_REQ", "AuthReg_ConsumerDereg"},
    {"ENDPOINT_UPDATE_REQ", "AuthReg_EndpointUpdate"},
    {"CHANNEL_AUTH_APPLIED_REQ", "AuthReg_ChanAuthApplied"},

    // Control tier
    {"HEARTBEAT_NOTIFY", "Control_HeartbeatNotify"},
    {"GET_CHANNEL_AUTH_REQ", "Control_GetChannelAuth"},
    {"DISC_REQ", "Control_Disc"},

    // Control_EnvelopeWithRoleUid — body role_uid = caller's own uid;
    // identity_match + grammar + role-tag policy.  A row slipping to
    // plain EnvelopeOnly loses role_uid grammar + identity match —
    // the 2026-07-14 regression this pin now catches.
    {"CHECK_PEER_READY_REQ", "Control_EnvelopeWithRoleUid"},
    {"BAND_JOIN_REQ", "Control_EnvelopeWithRoleUid"},
    {"BAND_LEAVE_REQ", "Control_EnvelopeWithRoleUid"},
    {"BAND_BROADCAST_SEND_NOTIFY", "Control_EnvelopeWithRoleUid"},

    // Control_EnvelopeWithQueryRoleUid — body role_uid = queried
    // subject.  Grammar + role-tag policy only; NO identity_match
    // (probes legitimately ask about other roles).
    {"ROLE_PRESENCE_REQ", "Control_EnvelopeWithQueryRoleUid"},
    {"ROLE_INFO_REQ", "Control_EnvelopeWithQueryRoleUid"},

    // EnvelopeOnly tier — body has no identity fields (or
    // CHANNEL_BROADCAST_SEND_NOTIFY's legacy `sender_uid` naming).
    {"SCHEMA_REQ", "Control_EnvelopeWithRoleUid"},
    {"CHANNEL_LIST_REQ", "EnvelopeOnly"},
    {"METRICS_REQ", "Control_EnvelopeWithRoleUid"},
    {"SHM_BLOCK_QUERY_REQ", "EnvelopeOnly"},
    {"BAND_MEMBERS_REQ", "EnvelopeOnly"},
    {"CHANNEL_BROADCAST_SEND_NOTIFY", "EnvelopeOnly"},
};

} // namespace

TEST(WireDispatchTable, SizeMatchesExpectedMap)
{
    EXPECT_EQ(wd::dispatch_table_size(), kExpectedTiers.size())
        << "kDispatchTable row count changed.  If intentional, update "
           "kExpectedTiers in this test to match.  If unintentional, "
           "a msg_type row was silently dropped (loses envelope-hash "
           "gating for that msg_type — C11 incident).";
}

TEST(WireDispatchTable, EveryExpectedMsgTypeMapsToExpectedTier)
{
    for (const auto &[msg_type, expected_tier] : kExpectedTiers)
    {
        auto actual = wd::tier_for_msg_type(msg_type);
        ASSERT_TRUE(actual.has_value())
            << "msg_type '" << msg_type << "' absent from kDispatchTable "
            << "(broker will reply UNKNOWN_MSG_TYPE for it)";
        EXPECT_EQ(*actual, expected_tier) << "msg_type '" << msg_type << "' routes to tier '"
                                          << *actual << "', expected '" << expected_tier << "'";
    }
}

TEST(WireDispatchTable, RenamedBroadcastMsgTypesAreLive)
{
    // Direct pin for the SEND_NOTIFY rename (Group 5 broadcast rename).
    // Both new names must resolve; both old names must not.
    EXPECT_TRUE(wd::tier_for_msg_type("CHANNEL_BROADCAST_SEND_NOTIFY").has_value())
        << "CHANNEL_BROADCAST_SEND_NOTIFY missing from dispatch table";
    EXPECT_TRUE(wd::tier_for_msg_type("BAND_BROADCAST_SEND_NOTIFY").has_value())
        << "BAND_BROADCAST_SEND_NOTIFY missing from dispatch table";
    EXPECT_FALSE(wd::tier_for_msg_type("CHANNEL_BROADCAST_REQ").has_value())
        << "old CHANNEL_BROADCAST_REQ literal still resolves — the "
           "SEND_NOTIFY rename did not land completely";
    EXPECT_FALSE(wd::tier_for_msg_type("BAND_BROADCAST_REQ").has_value())
        << "old BAND_BROADCAST_REQ literal still resolves — the "
           "SEND_NOTIFY rename did not land completely";
}

TEST(WireDispatchTable, UnknownMsgTypeReturnsNullopt)
{
    EXPECT_FALSE(wd::tier_for_msg_type("").has_value());
    EXPECT_FALSE(wd::tier_for_msg_type("NOT_A_REAL_MSG_TYPE").has_value());
    EXPECT_FALSE(wd::tier_for_msg_type("REG_ACK").has_value())
        << "ACK msg_types are broker-outbound, never received; must not "
           "appear in the receive dispatch table";
}

// ═══════════════════════════════════════════════════════════════════════
// receive_and_validate — direct unit pins for the single ingress
// (HEP-0046 §14.5; the Phase B drift guard).
//
// Every broker-inbound message flows through this ONE function; until
// these pins landed it was covered only indirectly via L3.  The suite
// drives real 5-frame envelopes end to end through parse → typed body →
// gates and pins both the success variant and the per-gate rejects —
// including the embedded-JSON drift guard: a doubly-encoded field
// (`inbox_schema_json`) with malformed CONTENT must die HERE, at the
// boundary, not in a downstream hand-parse.
// ═══════════════════════════════════════════════════════════════════════

#include "utils/admission_gates.hpp"
#include "utils/wire_envelope.hpp"

#include "cppzmq/zmq_addon.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace
{

namespace ag = pylabhub::admission;
using pylabhub::wire::WireEnvelope;

constexpr const char *kUid = "prod.test.uid1";
constexpr const char *kPubkey = "abcdefghij0123456789abcdefghij0123456789";
constexpr std::uint64_t kNowMs = 1'000'000ULL;

struct RavFixture
{
    ag::AdmissionCallbacks cb;
    std::set<std::string> seen_nonces;

    // These tests hand the pipeline frames they built themselves, so there
    // is no connection behind them.  An unarmed socket has no enforced ZAP
    // domain, so parse mints no attestation — which is what these cases
    // assert on: body schema, correlation echo, nonce replay, msg_type
    // routing.  Identity provenance is pinned separately against real
    // handshakes.
    zmq::context_t zmq_ctx{1};
    zmq::socket_t unarmed_sock{zmq_ctx, zmq::socket_type::router};

    RavFixture()
    {
        // Admit the expected (uid, key) pair so the non-identity cases
        // below reach the gate they are actually about.  A real authority
        // would also require the connection to have PROVEN kPubkey; these
        // frames are hand-built with no connection behind them, so that
        // half is pinned where handshakes are real (L2 zap_router_workers,
        // L1 test_admission_gates for the gate's own mapping).
        cb.check_registration =
            [](const std::optional<pylabhub::utils::security::AttestedKey> &, std::string_view uid,
               std::string_view pubkey) -> pylabhub::utils::security::ClaimVerdict
        {
            using CV = pylabhub::utils::security::ClaimVerdict;
            if (uid != kUid)
                return CV::identity_mismatch;
            if (pubkey != kPubkey)
                return CV::pubkey_mismatch;
            return CV::accepted;
        };
        cb.record_and_check_nonce = [this](std::string_view uid, std::string_view nonce)
        {
            std::string key{uid};
            key.push_back('|');
            key.append(nonce);
            return seen_nonces.insert(std::move(key)).second;
        };
        cb.wall_now_ms = []() { return kNowMs; };
    }

    [[nodiscard]] const zmq::socket_t &sock() const { return unarmed_sock; }

    [[nodiscard]] ag::AdmissionContext ctx() const
    {
        ag::AdmissionContext c;
        c.cb = &cb;
        return c;
    }

    /// Minimal §7.1-required REG_REQ body + security stamp fields.
    static nlohmann::json reg_body()
    {
        nlohmann::json b;
        b["channel_name"] = "lab.test.channel";
        b["role_uid"] = kUid;
        b["role_type"] = "producer";
        b["data_transport"] = "zmq";
        b["zmq_pubkey"] = kPubkey;
        b["abi_fingerprint"] = nlohmann::json::object();
        b["client_wall_ts"] = kNowMs;
        return b;
    }

    /// Wire a REG_REQ as the broker's ROUTER sees it: 5 frames with
    /// Frame 0 = sender identity, envelope_hash stamped over the
    /// skeleton.  `identity` defaults to the body's role_uid
    /// (I-DEALER-IDENTITY conformant); pass another value to violate it.
    static zmq::multipart_t wire_reg(nlohmann::json body, const std::string &nonce,
                                     const std::string &identity = kUid)
    {
        body["client_nonce"] = nonce;
        return WireEnvelope::build_router_send(identity, "REG_REQ", "cid-rav-1", body);
    }
};

} // namespace

TEST(ReceiveAndValidate, HappyPathRegReqYieldsTypedVariant)
{
    RavFixture f;
    auto received = wd::receive_and_validate(RavFixture::wire_reg(RavFixture::reg_body(), "n-1"),
                                             f.sock(), f.ctx());
    auto *v = std::get_if<wd::ValidatedRegReq>(&received);
    ASSERT_NE(v, nullptr) << "expected ValidatedRegReq variant";
    EXPECT_EQ(v->body.channel_name(), "lab.test.channel");
    EXPECT_EQ(v->body.role_uid(), kUid);
    EXPECT_EQ(v->identity(), kUid);
    EXPECT_EQ(v->correlation_id(), "cid-rav-1");
}

TEST(ReceiveAndValidate, MissingRequiredFieldRejectsBodySchemaViolation)
{
    RavFixture f;
    auto body = RavFixture::reg_body();
    body.erase("data_transport");
    auto received =
        wd::receive_and_validate(RavFixture::wire_reg(std::move(body), "n-2"), f.sock(), f.ctx());
    auto *r = std::get_if<wd::RejectedMessage>(&received);
    ASSERT_NE(r, nullptr) << "expected RejectedMessage variant";
    EXPECT_EQ(r->code, ag::RejectCode::body_schema_violation);
    EXPECT_EQ(r->correlation_id, "cid-rav-1");
}

// THE embedded-JSON drift guard (HEP-0046 B.2/B.4): the doubly-encoded
// field's CONTENT is validated at this single ingress.  Before B.2, a
// malformed inner blob sailed through here and each downstream reader
// hand-parsed it — the parses silently diverged (the array-vs-object
// incident).  Now it cannot pass the boundary.
TEST(ReceiveAndValidate, MalformedEmbeddedInboxSchemaRejectedAtBoundary)
{
    RavFixture f;
    auto body = RavFixture::reg_body();
    body["inbox_schema_json"] = "not-json";
    auto received =
        wd::receive_and_validate(RavFixture::wire_reg(std::move(body), "n-3"), f.sock(), f.ctx());
    auto *r = std::get_if<wd::RejectedMessage>(&received);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->code, ag::RejectCode::body_schema_violation);

    // Well-formed JSON, non-canonical shape (bare array): same fate.
    auto body2 = RavFixture::reg_body();
    body2["inbox_schema_json"] = R"([{"name":"v","type":"float64"}])";
    auto received2 =
        wd::receive_and_validate(RavFixture::wire_reg(std::move(body2), "n-4"), f.sock(), f.ctx());
    auto *r2 = std::get_if<wd::RejectedMessage>(&received2);
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ(r2->code, ag::RejectCode::body_schema_violation);
}

TEST(ReceiveAndValidate, IdentityMismatchRejected)
{
    RavFixture f;
    auto received = wd::receive_and_validate(
        RavFixture::wire_reg(RavFixture::reg_body(), "n-5", /*identity=*/"prod.other.uid9"),
        f.sock(), f.ctx());
    auto *r = std::get_if<wd::RejectedMessage>(&received);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->code, ag::RejectCode::identity_mismatch);
}

TEST(ReceiveAndValidate, ReplayedNonceRejected)
{
    RavFixture f;
    auto first = wd::receive_and_validate(RavFixture::wire_reg(RavFixture::reg_body(), "n-6"),
                                          f.sock(), f.ctx());
    ASSERT_NE(std::get_if<wd::ValidatedRegReq>(&first), nullptr);

    auto replay = wd::receive_and_validate(RavFixture::wire_reg(RavFixture::reg_body(), "n-6"),
                                           f.sock(), f.ctx());
    auto *r = std::get_if<wd::RejectedMessage>(&replay);
    ASSERT_NE(r, nullptr) << "identical nonce must be rejected";
    EXPECT_EQ(r->code, ag::RejectCode::replay_or_skew);
}

TEST(ReceiveAndValidate, UnknownMsgTypeYieldsRawControlForErrorReply)
{
    // Unknown msg_type still parses the envelope (hash validated) and
    // surfaces as ValidatedRawControl so the broker can address the
    // UNKNOWN_MSG_TYPE ERROR reply with the proper correlation echo.
    RavFixture f;
    nlohmann::json body;
    body["anything"] = 1;
    auto frames = WireEnvelope::build_router_send(kUid, "NOT_A_REAL_MSG_TYPE", "cid-rav-2", body);
    auto received = wd::receive_and_validate(std::move(frames), f.sock(), f.ctx());
    auto *v = std::get_if<wd::ValidatedRawControl>(&received);
    ASSERT_NE(v, nullptr) << "unknown msg_type must surface as ValidatedRawControl";
    EXPECT_EQ(v->msg_type(), "NOT_A_REAL_MSG_TYPE");
    EXPECT_EQ(v->correlation_id(), "cid-rav-2");
}
