// tests/test_layer1_base/test_admission_gates.cpp
//
// L1 tests for the reusable admission-gate pipeline.  Uses in-process
// stub callbacks (no HubState, no broker) to exercise each gate's state
// transitions in isolation, per §14.5 of the REG protocol design.

#include "utils/admission_gates.hpp"
#include "utils/wire_envelope.hpp"
#include "utils/wire_bodies.hpp"

#include "cppzmq/zmq_addon.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>

namespace ag = pylabhub::admission;
using pylabhub::wire::WireEnvelope;

namespace
{

// A tiny helper that builds an inbound router envelope for use in gate
// tests.  Returns the parsed WireEnvelope on the assumption its
// envelope_hash matches (gate 1 is out of scope for these tests).
WireEnvelope build_envelope(std::string_view identity, std::string_view msg_type,
                            std::string_view correlation_id, nlohmann::json body)
{
    auto frames =
        WireEnvelope::build_router_send(identity, msg_type, correlation_id, std::move(body));
    // Gate tests drive the gates directly with a hand-built envelope; the
    // socket carries no enforced ZAP domain, so the envelope arrives with
    // no attestation.  Cases that need a proven key set it explicitly.
    zmq::context_t ctx{1};
    zmq::socket_t unarmed{ctx, zmq::socket_type::router};
    pylabhub::wire::ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), unarmed, &err);
    if (!env)
    {
        throw std::runtime_error("build_envelope: parse failed");
    }
    return std::move(*env);
}

// Callback fixture with in-process state — a scripted claim verdict and a
// nonce dedup set.
//
// The verdict is scripted rather than computed because `AttestedKey` is
// unforgeable by construction: its only factory reads a proven key off a
// real ZAP-armed connection, so no L1 test can manufacture one.  That
// splits the work cleanly and each layer tests what it can actually
// observe:
//
//   this file  — the gate's own logic: every verdict maps to the right
//                reject code, and the gate forwards the envelope's
//                attestation plus the body's claimed uid / announced key
//   L2         — attestation → verdict, driven by four real CURVE
//                handshakes (workers/zap_router_workers.cpp)
//   L3/L4      — the whole path, live peer to admission decision
struct StubCallbacks
{
    using ClaimVerdict = pylabhub::utils::security::ClaimVerdict;

    ClaimVerdict verdict{ClaimVerdict::accepted};
    std::unordered_set<std::string> seen_nonces;
    std::uint64_t now_ms{1'000'000ULL};

    // What the gate actually handed the authority on the last call.
    std::string seen_uid;
    std::string seen_pubkey;
    bool seen_attestation{false};
    int call_count{0};

    // The name the scripted authority resolves a connection to, when the
    // verdict accepts one.
    std::string attributed_uid{"prod.test.uid1"};

    ag::AdmissionCallbacks make()
    {
        ag::AdmissionCallbacks cb;
        cb.check_registration =
            [this](const std::optional<pylabhub::utils::security::AttestedKey> &attested,
                   std::string_view uid, std::string_view pubkey) -> ClaimVerdict
        {
            seen_uid.assign(uid);
            seen_pubkey.assign(pubkey);
            seen_attestation = attested.has_value();
            ++call_count;
            return verdict;
        };
        cb.check_role_ownership =
            [this](const std::optional<pylabhub::utils::security::AttestedKey> &attested,
                   std::string_view uid) -> ClaimVerdict
        {
            seen_uid.assign(uid);
            seen_attestation = attested.has_value();
            ++call_count;
            return verdict;
        };
        cb.attribute_sender =
            [this](const std::optional<pylabhub::utils::security::AttestedKey> &attested)
            -> pylabhub::utils::security::AttributedSender
        {
            seen_attestation = attested.has_value();
            ++call_count;
            // Mirrors the authority's own contract: a name exists only when
            // the verdict accepted one.  A stub that returned a name anyway
            // would let a gate pass its out-param through on a refusal and
            // still look correct here.
            if (verdict != ClaimVerdict::accepted)
                return {verdict, {}};
            return {verdict, attributed_uid};
        };
        cb.record_and_check_nonce = [this](std::string_view uid, std::string_view nonce)
        {
            std::string key{uid};
            key.push_back('|');
            key.append(nonce);
            auto [it, inserted] = seen_nonces.insert(std::move(key));
            (void)it;
            return inserted;
        };
        cb.wall_now_ms = [this]() { return now_ms; };
        return cb;
    }
};

// Build a body view + context in one place; individual tests tweak fields.
struct Fixture
{
    StubCallbacks stub;
    ag::AdmissionCallbacks cb;

    Fixture() { cb = stub.make(); }

    ag::AdmissionContext ctx() const
    {
        ag::AdmissionContext c;
        c.cb = &cb;
        // broker_proto retired per C3.
        c.skew_tolerance_ms = 30'000ULL;
        c.nonce_window_ms = 10'000ULL;
        return c;
    }

    ag::RegFamilyBodyView body(std::string_view uid = "prod.test.uid1",
                               std::string_view pubkey = "abcdefghij0123456789abcdefghij0123456789",
                               std::string_view nonce = "n1", std::uint64_t ts = 1'000'000ULL,
                               std::string_view channel_name = "lab.test.channel") const
    {
        ag::RegFamilyBodyView v;
        v.role_uid = uid;
        v.channel_name = channel_name;
        v.zmq_pubkey = pubkey;
        // broker_proto retired per C3.
        v.client_nonce = nonce;
        v.client_wall_ts = ts;
        return v;
    }
};

} // namespace

// ── Reject-code wire-string stability ─────────────────────────────────

TEST(AdmissionGates, RejectCodeWireStrings)
{
    // Design (addendum §4): stable wire strings across protocol lifetime.
    // Every RejectCode variant gets a stability pin — renaming one on
    // the wire is a breaking change for every deployed client, so
    // covering all of them here catches accidental drift.
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::envelope_tampered), "ENVELOPE_TAMPERED");
    // unsupported_proto retired per C3.
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::body_schema_violation), "BODY_SCHEMA_VIOLATION");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::identity_mismatch), "IDENTITY_MISMATCH");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::invalid_request), "INVALID_REQUEST");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::unknown_role), "UNKNOWN_ROLE");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::pubkey_mismatch), "PUBKEY_MISMATCH");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::uid_conflict), "UID_CONFLICT");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::unauthenticated), "UNAUTHENTICATED");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::wrong_peer_kind), "WRONG_PEER_KIND");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::replay_or_skew), "REPLAY_OR_SKEW");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::invalid_role_tag), "INVALID_ROLE_TAG");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::broker_internal_error), "BROKER_INTERNAL_ERROR");
}

// ── Per-msg-type role-tag policy (HEP-CORE-0033 §G2.2.0b.8) ───────────

TEST(AdmissionGate_RoleTagPolicy, RegReqAllowsProdAndProc)
{
    EXPECT_EQ(ag::gate_role_tag_policy("REG_REQ", "prod.foo.uid1", ""), std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("REG_REQ", "proc.bar.uid2", ""), std::nullopt);
}

TEST(AdmissionGate_RoleTagPolicy, RegReqRejectsConsumerUid)
{
    auto r = ag::gate_role_tag_policy("REG_REQ", "cons.bad.uid3", "");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_role_tag);
    EXPECT_EQ(r->field, "role_uid");
}

TEST(AdmissionGate_RoleTagPolicy, ConsumerRegReqAllowsConsAndProc)
{
    EXPECT_EQ(ag::gate_role_tag_policy("CONSUMER_REG_REQ", "cons.foo.uid1", ""), std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("CONSUMER_REG_REQ", "proc.bar.uid2", ""), std::nullopt);
}

TEST(AdmissionGate_RoleTagPolicy, ConsumerRegReqRejectsProducerUid)
{
    auto r = ag::gate_role_tag_policy("CONSUMER_REG_REQ", "prod.bad.uid3", "");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_role_tag);
}

TEST(AdmissionGate_RoleTagPolicy, DeregReqAllowsProdAndProc)
{
    EXPECT_EQ(ag::gate_role_tag_policy("DEREG_REQ", "prod.foo.uid1", ""), std::nullopt);
    auto r = ag::gate_role_tag_policy("DEREG_REQ", "cons.bad.uid3", "");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_role_tag);
}

TEST(AdmissionGate_RoleTagPolicy, HeartbeatNotifyRequiresTagMatchRoleType)
{
    // Correct: producer role_type + prod.* uid.
    EXPECT_EQ(ag::gate_role_tag_policy("HEARTBEAT_NOTIFY", "prod.foo.uid1", "producer"),
              std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("HEARTBEAT_NOTIFY", "cons.bar.uid2", "consumer"),
              std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("HEARTBEAT_NOTIFY", "proc.baz.uid3", "processor"),
              std::nullopt);
}

TEST(AdmissionGate_RoleTagPolicy, HeartbeatNotifyRejectsRoleTypeMismatch)
{
    // consumer role_type carrying a prod.* uid — impersonation attempt.
    auto r = ag::gate_role_tag_policy("HEARTBEAT_NOTIFY", "prod.foo.uid1", "consumer");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_role_tag);
}

TEST(AdmissionGate_RoleTagPolicy, HeartbeatNotifyEmptyRoleTypeRejected)
{
    auto r = ag::gate_role_tag_policy("HEARTBEAT_NOTIFY", "prod.foo.uid1", "");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
    EXPECT_EQ(r->field, "role_type");
}

TEST(AdmissionGate_RoleTagPolicy, UniversalSetAllowsAllRecognizedTags)
{
    // Msg_types outside the table (e.g. ROLE_PRESENCE_REQ) allow all
    // three recognized tags.
    for (auto uid : {"prod.a.uid1", "cons.b.uid2", "proc.c.uid3"})
    {
        EXPECT_EQ(ag::gate_role_tag_policy("ROLE_PRESENCE_REQ", uid, ""), std::nullopt);
    }
}

// ── Gate 2 (broker_proto) retired per C3 ──────────────────────────────
//
// The per-message broker_proto gate was removed; ABI/protocol drift is
// now caught by the abi_fingerprint gate at REG-family admission.

// ── Gate 3: identity match ────────────────────────────────────────────

TEST(AdmissionGate_Identity, MatchPasses)
{
    Fixture f;
    nlohmann::json body = nlohmann::json::object();
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", std::move(body));
    auto b = f.body();
    EXPECT_EQ(ag::gate_dealer_identity_consistency(env, b), std::nullopt);
}

TEST(AdmissionGate_Identity, MismatchRejects)
{
    Fixture f;
    nlohmann::json body = nlohmann::json::object();
    auto env = build_envelope("attacker.uid", "REG_REQ", "cid-1", std::move(body));
    auto b = f.body();
    b.role_uid = "prod.test.uid1"; // body claims prod.test.uid1, but
                                   // envelope carries attacker.uid
    auto r = ag::gate_dealer_identity_consistency(env, b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::identity_mismatch);
}

// ── Gate 4: grammar ───────────────────────────────────────────────────

TEST(AdmissionGate_Grammar, ValidUidPasses)
{
    Fixture f;
    EXPECT_EQ(ag::gate_grammar(f.body("prod.abc.uid1")), std::nullopt);
}

TEST(AdmissionGate_Grammar, EmptyUidRejects)
{
    Fixture f;
    auto b = f.body("");
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
    EXPECT_EQ(r->field, "role_uid");
}

TEST(AdmissionGate_Grammar, BadCharacterRejects)
{
    Fixture f;
    auto b = f.body("prod space.uid"); // contains space, illegal
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
}

TEST(AdmissionGate_Grammar, WrongPubkeyLengthRejects)
{
    Fixture f;
    auto b = f.body("prod.test.uid1", "short"); // 5 chars, need 40
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
    EXPECT_EQ(r->field, "zmq_pubkey");
}

TEST(AdmissionGate_Grammar, EmptyChannelNameRejects)
{
    Fixture f;
    auto b = f.body("prod.test.uid1", "abcdefghij0123456789abcdefghij0123456789", "n1",
                    1'000'000ULL, /*channel_name*/ "");
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
    EXPECT_EQ(r->field, "channel_name");
}

TEST(AdmissionGate_Grammar, BadChannelNameCharacterRejects)
{
    Fixture f;
    auto b =
        f.body("prod.test.uid1", "abcdefghij0123456789abcdefghij0123456789", "n1", 1'000'000ULL,
               /*channel_name*/ "lab space.name");
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
    EXPECT_EQ(r->field, "channel_name");
}

TEST(AdmissionGate_Grammar, ValidPubkeyWithZ85CharsPassesLengthCheck)
{
    Fixture f;
    // 40-char Z85 pubkey with symbols outside the role_uid grammar
    // (%, /, ?, etc.) — was previously incorrectly rejected by a
    // spurious grammar_ok check on the pubkey.  Length is the only
    // wire-boundary guard here; ZAP proves cryptographic validity.
    auto b = f.body("prod.test.uid1", "dUlk%.Gm0vO/n?wBYbGWfPB0cXXnCAaIl%nAqQvV");
    EXPECT_EQ(ag::gate_grammar(b), std::nullopt);
}

// ── Gate 5: attested binding ──────────────────────────────────────────

using ClaimVerdict = pylabhub::utils::security::ClaimVerdict;

TEST(AdmissionGate_AttestedBinding, AcceptedVerdictPasses)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::accepted;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    EXPECT_EQ(ag::gate_attested_binding(env, f.body(), f.ctx()), std::nullopt);
}

// The gate must hand the authority what the BODY claims and what the
// ENVELOPE proved.  If it forwarded, say, the envelope's routing id in
// place of the claimed uid, every verdict test above would still pass
// while the gate decided the wrong question.
TEST(AdmissionGate_AttestedBinding, ForwardsClaimedUidAnnouncedKeyAndAttestation)
{
    Fixture f;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    auto b = f.body("prod.test.uid1", "abcdefghij0123456789abcdefghij0123456789");

    (void)ag::gate_attested_binding(env, b, f.ctx());

    EXPECT_EQ(f.stub.call_count, 1) << "gate must consult the authority exactly once";
    EXPECT_EQ(f.stub.seen_uid, "prod.test.uid1") << "gate must forward the CLAIMED role_uid";
    EXPECT_EQ(f.stub.seen_pubkey, "abcdefghij0123456789abcdefghij0123456789")
        << "gate must forward the ANNOUNCED zmq_pubkey";
    EXPECT_FALSE(f.stub.seen_attestation)
        << "build_envelope parses off an unarmed socket, so the envelope carries "
           "no attestation and the gate must pass that absence through rather "
           "than substituting anything";
}

// One case per verdict.  A verdict that fell through to `accepted` would
// admit a registration the authority refused, so every arm is pinned.

TEST(AdmissionGate_AttestedBinding, NoAttestationRejectsUnauthenticated)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::no_attestation;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_binding(env, f.body(), f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::unauthenticated);
}

TEST(AdmissionGate_AttestedBinding, UnknownKeyRejectsUnknownRole)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::unknown_key;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_binding(env, f.body(), f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::unknown_role);
}

TEST(AdmissionGate_AttestedBinding, PubkeyMismatchRejects)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::pubkey_mismatch;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_binding(env, f.body(), f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::pubkey_mismatch);
}

TEST(AdmissionGate_AttestedBinding, FederationPeerRejectsWrongPeerKind)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::kind_not_permitted;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_binding(env, f.body(), f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::wrong_peer_kind);
}

// THE defect: a peer holding a valid key of its own, proven at handshake,
// registering under a DIFFERENT role's uid.
TEST(AdmissionGate_AttestedBinding, ImpersonationRejectsIdentityMismatch)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::identity_mismatch;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_binding(env, f.body(), f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::identity_mismatch);
}

// An unbound callback must reject, never admit.  This is the failure mode
// that would silently disable the gate for an entire broker.
TEST(AdmissionGate_AttestedBinding, UnboundCallbackRejectsRatherThanAdmits)
{
    Fixture f;
    ag::AdmissionCallbacks empty;
    ag::AdmissionContext c;
    c.cb = &empty;
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_binding(env, f.body(), c);
    ASSERT_TRUE(r.has_value()) << "an unbound authority callback must NOT admit";
    EXPECT_EQ(r->code, ag::RejectCode::broker_internal_error);
}

// ── Gate 5b: attested role ownership (post-registration family) ───────
//
// DEREG / ENDPOINT_UPDATE / CHANNEL_AUTH_APPLIED bodies carry no announced
// key, so this asks ownership alone.  The verdict → reject mapping is
// shared with gate_attested_binding and pinned above; what these cases add
// is that this gate reaches it, forwards the right arguments, and fails
// CLOSED when unbound.

TEST(AdmissionGate_AttestedOwnership, AcceptedVerdictPasses)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::accepted;
    auto env = build_envelope("prod.test.uid1", "DEREG_REQ", "cid-1", nlohmann::json::object());
    EXPECT_EQ(ag::gate_attested_role_ownership(env, "prod.test.uid1", f.ctx()), std::nullopt);
}

// The channel-teardown attack: any admitted principal naming a victim's
// uid in both the routing id and the body.  The consistency gate cannot
// see it — both values are the attacker's — so this gate must.
TEST(AdmissionGate_AttestedOwnership, ForeignRoleRejectsIdentityMismatch)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::identity_mismatch;
    auto env = build_envelope("prod.victim.uid", "DEREG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_role_ownership(env, "prod.victim.uid", f.ctx());
    ASSERT_TRUE(r.has_value()) << "a peer must not act on a role its key does not own";
    EXPECT_EQ(r->code, ag::RejectCode::identity_mismatch);
}

TEST(AdmissionGate_AttestedOwnership, ForwardsTargetUidAndAttestation)
{
    Fixture f;
    auto env = build_envelope("prod.test.uid1", "DEREG_REQ", "cid-1", nlohmann::json::object());

    (void)ag::gate_attested_role_ownership(env, "prod.test.uid1", f.ctx());

    EXPECT_EQ(f.stub.call_count, 1) << "gate must consult the authority exactly once";
    EXPECT_EQ(f.stub.seen_uid, "prod.test.uid1") << "gate must forward the role being acted on";
    EXPECT_FALSE(f.stub.seen_attestation)
        << "build_envelope parses off an unarmed socket, so the gate must pass "
           "that absence through rather than substituting anything";
}

TEST(AdmissionGate_AttestedOwnership, UnboundCallbackRejectsRatherThanAdmits)
{
    ag::AdmissionCallbacks empty;
    ag::AdmissionContext c;
    c.cb = &empty;
    auto env = build_envelope("prod.test.uid1", "DEREG_REQ", "cid-1", nlohmann::json::object());
    auto r = ag::gate_attested_role_ownership(env, "prod.test.uid1", c);
    ASSERT_TRUE(r.has_value()) << "an unbound ownership callback must NOT admit";
    EXPECT_EQ(r->code, ag::RejectCode::broker_internal_error);
}

// ── Gate 5c: attributed sender (channel broadcast) ────────────────────
//
// The broadcast body names nobody, so there is no claim to check — the
// gate's job is to NAME the connection or refuse the message.  The verdict
// → reject mapping is shared with the two gates above and pinned there;
// what these add is that a name only ever escapes on acceptance.

TEST(AdmissionGate_AttributedSender, AcceptedVerdictYieldsTheProvenName)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::accepted;
    f.stub.attributed_uid = "prod.alice.uid1";
    auto env = build_envelope("prod.someone.else", "CHANNEL_BROADCAST_SEND_NOTIFY", "cid-1",
                              nlohmann::json::object());

    std::string sender = "untouched";
    EXPECT_EQ(ag::gate_attributed_sender(env, f.ctx(), sender), std::nullopt);
    EXPECT_EQ(sender, "prod.alice.uid1")
        << "the gate must yield the name the authority resolved, not the "
           "routing id the client chose";
}

TEST(AdmissionGate_AttributedSender, UnknownKeyRejectsAndLeavesNoName)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::unknown_key;
    auto env = build_envelope("prod.test.uid1", "CHANNEL_BROADCAST_SEND_NOTIFY", "cid-1",
                              nlohmann::json::object());

    std::string sender = "untouched";
    auto r = ag::gate_attributed_sender(env, f.ctx(), sender);
    ASSERT_TRUE(r.has_value()) << "a connection this hub cannot name must not broadcast";
    EXPECT_EQ(r->code, ag::RejectCode::unknown_role);
    EXPECT_EQ(sender, "untouched")
        << "a refused attribution must not write an out-param — a caller that "
           "ignored the rejection would then stamp an empty sender";
}

TEST(AdmissionGate_AttributedSender, FederationPeerRejectsWrongPeerKind)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::kind_not_permitted;
    auto env = build_envelope("hub.peer.uid1", "CHANNEL_BROADCAST_SEND_NOTIFY", "cid-1",
                              nlohmann::json::object());

    std::string sender;
    auto r = ag::gate_attributed_sender(env, f.ctx(), sender);
    ASSERT_TRUE(r.has_value()) << "a peer hub relays identities other than its own and is "
                                  "never a local author";
    EXPECT_EQ(r->code, ag::RejectCode::wrong_peer_kind);
}

TEST(AdmissionGate_AttributedSender, UnboundCallbackRejectsRatherThanAdmits)
{
    ag::AdmissionCallbacks empty;
    ag::AdmissionContext c;
    c.cb = &empty;
    auto env = build_envelope("prod.test.uid1", "CHANNEL_BROADCAST_SEND_NOTIFY", "cid-1",
                              nlohmann::json::object());

    std::string sender;
    auto r = ag::gate_attributed_sender(env, c, sender);
    ASSERT_TRUE(r.has_value()) << "an unbound attribution callback must NOT admit";
    EXPECT_EQ(r->code, ag::RejectCode::broker_internal_error);
}

// ── Control-tier runner: the claim must be OWNED, not merely consistent ──
//
// The runner's other checks compare values the client chose against each
// other.  These pin that it also reaches the authority — the difference
// between "your two strings agree" and "that role is you".

TEST(AdmissionRunner_ControlTier, ConsistentButUnownedRoleUidRejects)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::identity_mismatch;
    // Routing id and body role_uid AGREE, and both name the victim.  This is
    // the shape the identity-consistency check cannot see through, and it is
    // what a forged heartbeat or band join looks like on the wire.
    auto env =
        build_envelope("prod.victim.uid", "HEARTBEAT_NOTIFY", "cid-1", nlohmann::json::object());
    ag::ControlBodyView v;
    v.role_uid = "prod.victim.uid";
    v.role_type = "producer";

    auto r = ag::run_control_gates(env, v, f.ctx());
    ASSERT_TRUE(r.has_value()) << "a control message may not act under a role the connection "
                                  "does not own";
    EXPECT_EQ(r->code, ag::RejectCode::identity_mismatch);
}

TEST(AdmissionRunner_ControlTier, OwnedRoleUidPasses)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::accepted;
    auto env =
        build_envelope("prod.test.uid1", "HEARTBEAT_NOTIFY", "cid-1", nlohmann::json::object());
    ag::ControlBodyView v;
    v.role_uid = "prod.test.uid1";
    v.role_type = "producer";

    EXPECT_EQ(ag::run_control_gates(env, v, f.ctx()), std::nullopt);
    EXPECT_EQ(f.stub.seen_uid, "prod.test.uid1")
        << "the runner must ask about the body's role_uid, not the routing id";
}

TEST(AdmissionRunner_ControlTier, EmptyRoleUidAsksTheAuthorityNothing)
{
    Fixture f;
    f.stub.verdict = ClaimVerdict::identity_mismatch; // would reject if consulted
    auto env = build_envelope("prod.test.uid1", "DISC_REQ", "cid-1", nlohmann::json::object());
    ag::ControlBodyView v;
    v.channel_name = "lab.test.channel";

    EXPECT_EQ(ag::run_control_gates(env, v, f.ctx()), std::nullopt)
        << "a body carrying no role_uid claims nothing, so there is nothing to own";
    EXPECT_EQ(f.stub.call_count, 0) << "the authority must not be consulted about an absent claim";
}

// Key rotation (HEP-0046 I-KEY-ROTATION-VIA-DEREG): there is no
// separate key-rotation gate.  A role's CURVE pubkey is immutable for
// the broker's lifetime; an on-the-fly re-REG under a different key
// cannot match what the connection proved, so it lands as
// PUBKEY_MISMATCH (covered by
// AdmissionGate_AttestedBinding.PubkeyMismatchRejects above).

// ── Gate 7: anti-replay ───────────────────────────────────────────────

TEST(AdmissionGate_Replay, FreshNoncePasses)
{
    Fixture f;
    EXPECT_EQ(ag::gate_replay_bound(f.body(/*uid*/ "prod.test.uid1",
                                           /*pk*/ "abcdefghij0123456789abcdefghij0123456789",
                                           /*nonce*/ "n1",
                                           /*ts*/ 1'000'000ULL),
                                    f.ctx()),
              std::nullopt);
}

TEST(AdmissionGate_Replay, ReusedNonceRejects)
{
    Fixture f;
    auto b =
        f.body("prod.test.uid1", "abcdefghij0123456789abcdefghij0123456789", "n1", 1'000'000ULL);
    // First call records; second call surfaces the collision.
    EXPECT_EQ(ag::gate_replay_bound(b, f.ctx()), std::nullopt);
    auto r = ag::gate_replay_bound(b, f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::replay_or_skew);
    EXPECT_EQ(r->field, "client_nonce");
}

TEST(AdmissionGate_Replay, ClockSkewRejects)
{
    Fixture f;
    f.stub.now_ms = 1'000'000ULL;
    f.cb = f.stub.make();
    auto b = f.body("prod.test.uid1", "abcdefghij0123456789abcdefghij0123456789", "n1",
                    /*ts=*/500'000ULL); // 500 s off from broker
    auto r = ag::gate_replay_bound(b, f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::replay_or_skew);
    EXPECT_EQ(r->field, "client_wall_ts");
}

// The pre-#67 "gate must pass broker time, not client_wall_ts" regression is
// obsolete BY CONSTRUCTION: record_and_check_nonce no longer takes a timestamp
// (the ReplayGuard owns its trusted monotonic clock), so there is no client
// stamp for the dedup path to mis-handle.  The window/clock behavior is pinned
// directly on the primitive in test_replay_guard.cpp.

// ── Full pipeline: happy path + first-failing-gate short-circuit ──────

TEST(AdmissionPipeline, HappyPathPasses)
{
    Fixture f;
    nlohmann::json body = nlohmann::json::object();
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1", std::move(body));
    EXPECT_EQ(ag::run_reg_family_gates(env, f.body(), f.ctx()), std::nullopt);
}

TEST(AdmissionPipeline, FirstFailingGateWins)
{
    // Multiple gates would fail: identity mismatch AND unknown role.
    // Order of gates is identity → grammar → known_role → ..., so
    // identity failure wins.
    Fixture f;
    nlohmann::json body = nlohmann::json::object();
    auto env = build_envelope("attacker.uid", "REG_REQ", "cid-1", std::move(body));
    auto b = f.body();
    b.role_uid = "prod.test.uid1"; // body claims known uid, envelope
                                   // carries attacker.uid → identity
                                   // mismatch fires before known_role
    auto r = ag::run_reg_family_gates(env, b, f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::identity_mismatch);
}
