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
WireEnvelope build_envelope(std::string_view identity,
                             std::string_view msg_type,
                             std::string_view correlation_id,
                             nlohmann::json    body)
{
    auto frames = WireEnvelope::build_router_send(identity, msg_type,
                                                    correlation_id,
                                                    std::move(body));
    pylabhub::wire::ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    if (!env)
    {
        throw std::runtime_error("build_envelope: parse failed");
    }
    return std::move(*env);
}

// Callback fixture with in-process state — a single-role known_roles,
// tunable rotation state, and a nonce dedup set.
struct StubCallbacks
{
    std::string known_uid;
    std::string known_pubkey;
    std::string current_pubkey;    // "" if not currently registered
    std::unordered_set<std::string> seen_nonces;
    std::uint64_t                    now_ms{1'000'000ULL};

    ag::AdmissionCallbacks make() {
        ag::AdmissionCallbacks cb;
        cb.lookup_known_role = [this](std::string_view uid,
                                        std::string_view pubkey)
                                    -> ag::KnownRoleLookup {
            if (uid != known_uid)
                return ag::KnownRoleLookup::uid_unknown;
            if (pubkey != known_pubkey)
                return ag::KnownRoleLookup::pubkey_mismatch;
            return ag::KnownRoleLookup::binding_matches;
        };
        cb.check_key_rotation = [this](std::string_view uid,
                                         std::string_view pubkey)
                                     -> ag::KeyRotationCheck {
            (void)uid;
            if (current_pubkey.empty())
                return ag::KeyRotationCheck::not_yet_registered;
            if (pubkey == current_pubkey)
                return ag::KeyRotationCheck::matches_current;
            return ag::KeyRotationCheck::rotation_attempted;
        };
        cb.record_and_check_nonce = [this](std::string_view uid,
                                            std::string_view nonce) {
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

    Fixture()
    {
        stub.known_uid    = "prod.test.uid1";
        stub.known_pubkey = "abcdefghij0123456789abcdefghij0123456789";
        cb                = stub.make();
    }

    ag::AdmissionContext ctx() const
    {
        ag::AdmissionContext c;
        c.cb                = &cb;
        // broker_proto retired per C3.
        c.skew_tolerance_ms = 30'000ULL;
        c.nonce_window_ms   = 10'000ULL;
        return c;
    }

    ag::RegFamilyBodyView body(std::string_view uid = "prod.test.uid1",
                                 std::string_view pubkey =
                                     "abcdefghij0123456789abcdefghij0123456789",
                                 std::string_view  nonce = "n1",
                                 std::uint64_t     ts    = 1'000'000ULL,
                                 std::string_view  channel_name =
                                     "lab.test.channel") const
    {
        ag::RegFamilyBodyView v;
        v.role_uid       = uid;
        v.channel_name   = channel_name;
        v.zmq_pubkey     = pubkey;
        // broker_proto retired per C3.
        v.client_nonce   = nonce;
        v.client_wall_ts = ts;
        return v;
    }
};

}  // namespace

// ── Reject-code wire-string stability ─────────────────────────────────

TEST(AdmissionGates, RejectCodeWireStrings)
{
    // Design (addendum §4): stable wire strings across protocol lifetime.
    // Every RejectCode variant gets a stability pin — renaming one on
    // the wire is a breaking change for every deployed client, so
    // covering all of them here catches accidental drift.
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::envelope_tampered),
              "ENVELOPE_TAMPERED");
    // unsupported_proto retired per C3.
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::body_schema_violation),
              "BODY_SCHEMA_VIOLATION");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::identity_mismatch),
              "IDENTITY_MISMATCH");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::invalid_request),
              "INVALID_REQUEST");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::unknown_role),
              "UNKNOWN_ROLE");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::pubkey_mismatch),
              "PUBKEY_MISMATCH");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::uid_conflict),
              "UID_CONFLICT");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::key_rotation_required),
              "KEY_ROTATION_REQUIRES_DEREG");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::replay_or_skew),
              "REPLAY_OR_SKEW");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::invalid_role_tag),
              "INVALID_ROLE_TAG");
    EXPECT_EQ(ag::to_wire_string(ag::RejectCode::broker_internal_error),
              "BROKER_INTERNAL_ERROR");
}

// ── Per-msg-type role-tag policy (HEP-CORE-0033 §G2.2.0b.8) ───────────

TEST(AdmissionGate_RoleTagPolicy, RegReqAllowsProdAndProc)
{
    EXPECT_EQ(ag::gate_role_tag_policy("REG_REQ", "prod.foo.uid1", ""),
              std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("REG_REQ", "proc.bar.uid2", ""),
              std::nullopt);
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
    EXPECT_EQ(ag::gate_role_tag_policy("CONSUMER_REG_REQ",
                                          "cons.foo.uid1", ""),
              std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("CONSUMER_REG_REQ",
                                          "proc.bar.uid2", ""),
              std::nullopt);
}

TEST(AdmissionGate_RoleTagPolicy, ConsumerRegReqRejectsProducerUid)
{
    auto r = ag::gate_role_tag_policy("CONSUMER_REG_REQ",
                                        "prod.bad.uid3", "");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_role_tag);
}

TEST(AdmissionGate_RoleTagPolicy, DeregReqAllowsProdAndProc)
{
    EXPECT_EQ(ag::gate_role_tag_policy("DEREG_REQ",
                                          "prod.foo.uid1", ""),
              std::nullopt);
    auto r = ag::gate_role_tag_policy("DEREG_REQ", "cons.bad.uid3", "");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_role_tag);
}

TEST(AdmissionGate_RoleTagPolicy, HeartbeatNotifyRequiresTagMatchRoleType)
{
    // Correct: producer role_type + prod.* uid.
    EXPECT_EQ(ag::gate_role_tag_policy("HEARTBEAT_NOTIFY",
                                          "prod.foo.uid1", "producer"),
              std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("HEARTBEAT_NOTIFY",
                                          "cons.bar.uid2", "consumer"),
              std::nullopt);
    EXPECT_EQ(ag::gate_role_tag_policy("HEARTBEAT_NOTIFY",
                                          "proc.baz.uid3", "processor"),
              std::nullopt);
}

TEST(AdmissionGate_RoleTagPolicy, HeartbeatNotifyRejectsRoleTypeMismatch)
{
    // consumer role_type carrying a prod.* uid — impersonation attempt.
    auto r = ag::gate_role_tag_policy("HEARTBEAT_NOTIFY",
                                        "prod.foo.uid1", "consumer");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_role_tag);
}

TEST(AdmissionGate_RoleTagPolicy, HeartbeatNotifyEmptyRoleTypeRejected)
{
    auto r = ag::gate_role_tag_policy("HEARTBEAT_NOTIFY",
                                        "prod.foo.uid1", "");
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
        EXPECT_EQ(ag::gate_role_tag_policy("ROLE_PRESENCE_REQ", uid, ""),
                  std::nullopt);
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
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1",
                                std::move(body));
    auto b = f.body();
    EXPECT_EQ(ag::gate_identity_match(env, b), std::nullopt);
}

TEST(AdmissionGate_Identity, MismatchRejects)
{
    Fixture f;
    nlohmann::json body = nlohmann::json::object();
    auto env = build_envelope("attacker.uid", "REG_REQ", "cid-1",
                                std::move(body));
    auto b = f.body();
    b.role_uid = "prod.test.uid1";  // body claims prod.test.uid1, but
                                     // envelope carries attacker.uid
    auto r = ag::gate_identity_match(env, b);
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
    auto b = f.body("prod space.uid");  // contains space, illegal
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
}

TEST(AdmissionGate_Grammar, WrongPubkeyLengthRejects)
{
    Fixture f;
    auto b = f.body("prod.test.uid1", "short");  // 5 chars, need 40
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
    EXPECT_EQ(r->field, "zmq_pubkey");
}

TEST(AdmissionGate_Grammar, EmptyChannelNameRejects)
{
    Fixture f;
    auto b = f.body("prod.test.uid1",
                     "abcdefghij0123456789abcdefghij0123456789",
                     "n1", 1'000'000ULL, /*channel_name*/"");
    auto r = ag::gate_grammar(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::invalid_request);
    EXPECT_EQ(r->field, "channel_name");
}

TEST(AdmissionGate_Grammar, BadChannelNameCharacterRejects)
{
    Fixture f;
    auto b = f.body("prod.test.uid1",
                     "abcdefghij0123456789abcdefghij0123456789",
                     "n1", 1'000'000ULL,
                     /*channel_name*/"lab space.name");
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
    auto b = f.body("prod.test.uid1",
                     "dUlk%.Gm0vO/n?wBYbGWfPB0cXXnCAaIl%nAqQvV");
    EXPECT_EQ(ag::gate_grammar(b), std::nullopt);
}

// ── Gate 5: known-roles binding ───────────────────────────────────────

TEST(AdmissionGate_KnownRole, MatchPasses)
{
    Fixture f;
    EXPECT_EQ(ag::gate_known_role_binding(f.body(), f.ctx()), std::nullopt);
}

TEST(AdmissionGate_KnownRole, UnknownUidRejects)
{
    Fixture f;
    auto b = f.body("prod.stranger.uid");
    auto r = ag::gate_known_role_binding(b, f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::unknown_role);
}

TEST(AdmissionGate_KnownRole, PubkeyMismatchRejects)
{
    Fixture f;
    auto b = f.body("prod.test.uid1",
                     "zzzzzzzzzz0000000000zzzzzzzzzz0000000000");
    auto r = ag::gate_known_role_binding(b, f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::pubkey_mismatch);
}

// ── Gate 6: key rotation ──────────────────────────────────────────────

TEST(AdmissionGate_KeyRotation, FreshRegistrationPasses)
{
    Fixture f;
    // stub.current_pubkey is empty → not_yet_registered path
    EXPECT_EQ(ag::gate_key_rotation(f.body(), f.ctx()), std::nullopt);
}

TEST(AdmissionGate_KeyRotation, SamePubkeyPasses)
{
    Fixture f;
    f.stub.current_pubkey = f.stub.known_pubkey;
    f.cb = f.stub.make();
    EXPECT_EQ(ag::gate_key_rotation(f.body(), f.ctx()), std::nullopt);
}

TEST(AdmissionGate_KeyRotation, DifferentPubkeyRejects)
{
    Fixture f;
    f.stub.current_pubkey = "old0old0old0old0old0old0old0old0old0old0";
    f.cb = f.stub.make();
    auto r = ag::gate_key_rotation(f.body(), f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::key_rotation_required);
}

// ── Gate 7: anti-replay ───────────────────────────────────────────────

TEST(AdmissionGate_Replay, FreshNoncePasses)
{
    Fixture f;
    EXPECT_EQ(ag::gate_replay_bound(f.body(/*uid*/"prod.test.uid1",
                                             /*pk*/"abcdefghij0123456789abcdefghij0123456789",
                                             /*nonce*/"n1",
                                             /*ts*/1'000'000ULL),
                                       f.ctx()),
              std::nullopt);
}

TEST(AdmissionGate_Replay, ReusedNonceRejects)
{
    Fixture f;
    auto b = f.body("prod.test.uid1",
                     "abcdefghij0123456789abcdefghij0123456789",
                     "n1", 1'000'000ULL);
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
    f.cb          = f.stub.make();
    auto b = f.body("prod.test.uid1",
                     "abcdefghij0123456789abcdefghij0123456789",
                     "n1", /*ts=*/500'000ULL);  // 500 s off from broker
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
    auto env = build_envelope("prod.test.uid1", "REG_REQ", "cid-1",
                                std::move(body));
    EXPECT_EQ(ag::run_reg_family_gates(env, f.body(), f.ctx()),
              std::nullopt);
}

TEST(AdmissionPipeline, FirstFailingGateWins)
{
    // Multiple gates would fail: identity mismatch AND unknown role.
    // Order of gates is identity → grammar → known_role → ..., so
    // identity failure wins.
    Fixture f;
    nlohmann::json body = nlohmann::json::object();
    auto env = build_envelope("attacker.uid", "REG_REQ", "cid-1",
                                std::move(body));
    auto b = f.body();
    b.role_uid = "prod.test.uid1";  // body claims known uid, envelope
                                     // carries attacker.uid → identity
                                     // mismatch fires before known_role
    auto r = ag::run_reg_family_gates(env, b, f.ctx());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->code, ag::RejectCode::identity_mismatch);
}
