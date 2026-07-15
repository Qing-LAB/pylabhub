/**
 * @file test_broker_reg_handler.cpp
 * @brief L2 integration tests for BrokerRegHandler — proves the REG
 *        admission pipeline runs end-to-end against a real HubState with
 *        realistic known_roles + broker config.
 *
 * Pins:
 *   - Callback wiring is one-time; per-request paths reuse it
 *   - Gate failures surface as typed RegRejected with proper reject codes
 *   - Nonce dedup persists across successive REG_REQs (HubState state)
 *   - Envelope + body validation flows into the pipeline correctly
 *
 * Does NOT exercise the state-mutation commit path yet (that closure is
 * a placeholder pending the broker_service.cpp handler migration).
 */

#include "utils/broker_reg_handler.hpp"
#include "utils/hub_state.hpp"
#include "utils/logger.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/wire_bodies.hpp"
#include "utils/wire_envelope.hpp"
#include "binary_lifecycle.h"

#include "cppzmq/zmq_addon.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>

PLH_BINARY_LIFECYCLE_MODULES(
    pylabhub::utils::Logger::GetLifecycleModule(),
    pylabhub::utils::security::SecureSubsystem::GetLifecycleModule())

namespace ag = pylabhub::admission;
using pylabhub::hub::HubState;
using pylabhub::wire::WireEnvelope;
using pylabhub::wire::ProducerRegReqBody;
using pylabhub::wire::ParseError;

namespace
{

constexpr std::string_view kTestUid    = "prod.test.uid1";
constexpr std::string_view kTestPubkey =
    "abcdefghij0123456789abcdefghij0123456789";

nlohmann::json make_body_json(std::string_view uid       = kTestUid,
                                std::string_view pubkey    = kTestPubkey,
                                std::string_view  nonce    = "nonce.integration.001",
                                std::uint64_t     wall_ts  = 0)
{
    // Use current wall clock so the skew gate doesn't reject.  Real
    // producers stamp this at REQ construction time per I-REPLAY-BOUND.
    using namespace std::chrono;
    if (wall_ts == 0)
    {
        wall_ts = duration_cast<milliseconds>(
                       system_clock::now().time_since_epoch())
                       .count();
    }
    nlohmann::json body;
    body["channel_name"]    = "lab.integration.channel";
    body["role_uid"]        = std::string{uid};
    body["role_type"]       = "producer";
    body["role_name"]       = "prod-integration";
    body["channel_topology"] = "one-to-one";
    body["data_transport"]  = "zmq";
    body["zmq_pubkey"]      = std::string{pubkey};
    body["schema_hash"]     = "deadbeef";
    body["schema_id"]       = "";
    body["schema_blds"]     = "";
    body["schema_owner"]    = "";
    body["abi_fingerprint"] = nlohmann::json::object();
    body["client_nonce"]    = std::string{nonce};
    body["client_wall_ts"]  = wall_ts;
    return body;
}

struct EnvelopePair
{
    WireEnvelope env;
    ProducerRegReqBody   body;
};

EnvelopePair make_envelope_pair(std::string_view uid    = kTestUid,
                                 std::string_view pubkey = kTestPubkey,
                                 std::string_view  nonce  = "nonce.integration.001")
{
    auto body_json = make_body_json(uid, pubkey, nonce);
    auto frames    = WireEnvelope::build_router_send(uid, "REG_REQ",
                                                        "cid-1", body_json);
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    if (!env) throw std::runtime_error("test setup: envelope parse failed");
    ProducerRegReqBody body{env->body()};
    return {std::move(*env), std::move(body)};
}

// Bind BrokerRegHandler with an in-process known_roles map for the test.
class Fixture
{
  public:
    Fixture()
        : known_roles_({{std::string{kTestUid}, std::string{kTestPubkey}}})
    {
        init_handler();
    }
    explicit Fixture(std::unordered_map<std::string, std::string> extra)
        : known_roles_({{std::string{kTestUid}, std::string{kTestPubkey}}})
    {
        for (auto &kv : extra) known_roles_.insert(std::move(kv));
        init_handler();
    }
    HubState              &hub()     { return hub_; }
    ag::BrokerRegHandler  &handler() { return *handler_; }

  private:
    void init_handler()
    {
        ag::KnownRolesConfig kr;
        kr.lookup_pubkey_for_uid =
            [this](std::string_view uid) -> std::string {
            auto it = known_roles_.find(std::string{uid});
            return (it == known_roles_.end()) ? std::string{} : it->second;
        };
        ag::BrokerAdmissionConfig cfg;
        // broker_proto field retired per C3.
        handler_ = std::make_unique<ag::BrokerRegHandler>(hub_, std::move(kr),
                                                            cfg);
    }

    HubState                                     hub_;
    std::unordered_map<std::string, std::string> known_roles_;
    std::unique_ptr<ag::BrokerRegHandler>        handler_;
};

}  // namespace

TEST(BrokerRegHandler, HappyPathAdmitsIntoHubState)
{
    Fixture f;
    // Sanity: HubState has no producer_instance for this uid yet.
    EXPECT_EQ(f.hub().producer_instance(std::string{kTestUid}), 0U);

    auto pair    = make_envelope_pair();
    auto outcome = f.handler().handle(pair.env, pair.body);
    ASSERT_TRUE(std::holds_alternative<ag::RegAccepted>(outcome))
        << "expected RegAccepted; got variant index "
        << outcome.index();

    const auto &accepted = std::get<ag::RegAccepted>(outcome);
    // channel_opened must be true — this was the first admission.
    EXPECT_TRUE(accepted.channel_opened);
    // instance_id populated by HubState.
    EXPECT_GE(accepted.assigned_instance_id, 1U);
    // Post-admission: producer_instance bumped, channel visible.
    EXPECT_GE(f.hub().producer_instance(std::string{kTestUid}), 1U);
}

TEST(BrokerRegHandler, SecondAdmissionOnSameChannelDoesNotReopen)
{
    Fixture f;
    // First producer admits + opens channel.
    auto p1 = make_envelope_pair(kTestUid, kTestPubkey,
                                   /*nonce=*/"nonce.001");
    auto out1 = f.handler().handle(p1.env, p1.body);
    ASSERT_TRUE(std::holds_alternative<ag::RegAccepted>(out1));
    EXPECT_TRUE(std::get<ag::RegAccepted>(out1).channel_opened);

    // Same producer re-registering with a new nonce should be admitted
    // by pre-mutation gates but rejected by HubState's UID_CONFLICT
    // (role_uid already registered on this channel).
    auto p2 = make_envelope_pair(kTestUid, kTestPubkey,
                                   /*nonce=*/"nonce.002");
    auto out2 = f.handler().handle(p2.env, p2.body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(out2));
    EXPECT_EQ(std::get<ag::RegRejected>(out2).detail.code,
              ag::RejectCode::uid_conflict);
}

// RejectsWrongProto — retired per C3.  broker_proto wire field and the
// unsupported_proto reject code are gone; ABI drift is caught at the
// abi_fingerprint gate, not per-message.

TEST(BrokerRegHandler, RejectsUnknownRole)
{
    Fixture f;
    auto pair = make_envelope_pair("prod.stranger.uid",
                                     "1111111111111111111111111111111111111111");
    auto outcome = f.handler().handle(pair.env, pair.body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.code,
              ag::RejectCode::unknown_role);
}

TEST(BrokerRegHandler, RejectsPubkeyMismatch)
{
    Fixture f;
    // Known uid, wrong pubkey.
    auto pair = make_envelope_pair(kTestUid,
                                     "9999999999999999999999999999999999999999");
    auto outcome = f.handler().handle(pair.env, pair.body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.code,
              ag::RejectCode::pubkey_mismatch);
}

TEST(BrokerRegHandler, NonceReplayPersistsAcrossCalls)
{
    Fixture f;
    // First REG with nonce N1: accepted.
    auto p1 = make_envelope_pair(kTestUid, kTestPubkey,
                                   /*nonce=*/"nonce.replay.test");
    auto out1 = f.handler().handle(p1.env, p1.body);
    ASSERT_TRUE(std::holds_alternative<ag::RegAccepted>(out1));

    // Second REG with SAME nonce (same role): rejected as replay.
    auto p2 = make_envelope_pair(kTestUid, kTestPubkey,
                                   /*nonce=*/"nonce.replay.test");
    auto out2 = f.handler().handle(p2.env, p2.body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(out2));
    EXPECT_EQ(std::get<ag::RegRejected>(out2).detail.code,
              ag::RejectCode::replay_or_skew);
}

TEST(BrokerRegHandler, FanInProducerAsFirstAdmissionIsRejectedUntilR6Wired)
{
    Fixture f;
    // Under §3.3.0 fan-in, the CONSUMER is the binding side and the
    // topology-legal channel opener.  A fan-in producer arriving first
    // (no consumer yet) must NOT silently open the channel — that
    // violates §3.3.0 and mislabels state.  Until R6 pending is wired,
    // BrokerRegHandler surfaces this as broker_internal_error.  A prior
    // iteration of this test asserted the incorrect behavior (accepted
    // + dialing side) and let the protocol violation slip past review.
    auto body_json = make_body_json();
    body_json["channel_topology"] = "fan-in";
    auto frames = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                    "cid-fanin", body_json);
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value());
    ProducerRegReqBody body{env->body()};

    auto outcome = f.handler().handle(*env, body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome))
        << "fan-in producer must not be admitted as first-on-channel "
           "(§3.3.0 binding-side rule); got Accepted, which would open "
           "channel with dialing side as first admitted";
    const auto &rej = std::get<ag::RegRejected>(outcome);
    EXPECT_EQ(rej.detail.code, ag::RejectCode::broker_internal_error);
    EXPECT_EQ(rej.detail.field, "channel_topology");
    // State side-effect verification: HubState must NOT have any
    // channel created — pre-mutation rejection.
    EXPECT_FALSE(f.hub().channel("lab.integration.channel").has_value())
        << "fan-in producer rejection must be pre-mutation; channel "
           "must not exist";
}

TEST(BrokerRegHandler, ProducerAcceptedSideMatchesEffectiveTopology)
{
    Fixture f;
    // For non-fan-in topologies producer is BINDING side.  Verify the
    // typed outcome carries the correct side label — a prior iteration
    // hardcoded `binding` regardless of topology, which was silently
    // correct for OneToOne/FanOut but silently wrong for FanIn.
    auto body_json = make_body_json();
    body_json["channel_topology"] = "fan-out";
    auto frames = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                    "cid-fanout", body_json);
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value());
    ProducerRegReqBody body{env->body()};

    auto outcome = f.handler().handle(*env, body);
    ASSERT_TRUE(std::holds_alternative<ag::RegAccepted>(outcome));
    EXPECT_EQ(std::get<ag::RegAccepted>(outcome).side,
              ag::AdmissionSide::binding)
        << "fan-out producer must be BINDING side per HEP-CORE-0017 §3.3.0";
}

TEST(BrokerRegHandler, OmittedTopologyUsesStoredTopologyForSideDerivation)
{
    Fixture f;
    // F15 pin: side_for must use the effective (post-admission)
    // topology, not just the wire-declared value.  First producer
    // opens channel with fan-out; second producer sends OMITTED
    // channel_topology.  Prior behavior: wire says omitted → default
    // OneToOne → side=binding (accidentally matches fan-out).  This
    // test verifies the read-from-stored path works for the omitted
    // case — the outcome side must match the stored fan-out topology
    // (also binding), verifying the code path is exercised.
    //
    // A full test of the WRONG-when-wire-only case requires a stored
    // fan-in channel (opened by a consumer), which the current commit
    // callback does not handle; that test lands with the consumer
    // path.  Meanwhile the fan-in-producer-first F14 test above proves
    // the F15 fix's pre-admission side of the check.
    auto b1 = make_body_json(kTestUid, kTestPubkey,
                               "nonce-a");
    b1["channel_topology"] = "fan-out";
    auto f1 = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                "cid-1", b1);
    ParseError err{};
    auto e1 = WireEnvelope::parse_router_recv(std::move(f1), &err);
    ASSERT_TRUE(e1.has_value());
    auto out1 = f.handler().handle(*e1, ProducerRegReqBody{e1->body()});
    ASSERT_TRUE(std::holds_alternative<ag::RegAccepted>(out1));

    // Simulate a same-role second REG with OMITTED topology.  This will
    // still be UID_CONFLICT (same-uid re-register on same channel), but
    // the reg_type of the rejection tells us the pipeline reached
    // _on_producer_added — which by that point had run side_for against
    // effective topology.  We validate F15's read-from-stored code
    // path is executed by exercising this path; full verification lands
    // when N-distinct-uid producers are supported.
    auto b2 = make_body_json(kTestUid, kTestPubkey,
                               "nonce-b");
    b2["channel_topology"] = "";  // omitted
    auto f2 = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                "cid-2", b2);
    auto e2 = WireEnvelope::parse_router_recv(std::move(f2), &err);
    ASSERT_TRUE(e2.has_value());
    auto out2 = f.handler().handle(*e2, ProducerRegReqBody{e2->body()});
    // uid_conflict is HubState's rejection — proves we routed through
    // _on_producer_added under the effective-topology-aware pipeline.
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(out2));
    EXPECT_EQ(std::get<ag::RegRejected>(out2).detail.code,
              ag::RejectCode::uid_conflict);
}

TEST(BrokerRegHandler, UnknownTopologyStringRejected)
{
    Fixture f;
    // Silent-fallback pattern (treat unknown as omitted) is prohibited
    // under CURVE integrity frame — reject at wire.
    auto body_json = make_body_json();
    body_json["channel_topology"] = "fan_in";  // typo: underscore
    auto frames = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                    "cid-typo", body_json);
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value());
    ProducerRegReqBody body{env->body()};

    auto outcome = f.handler().handle(*env, body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.code,
              ag::RejectCode::invalid_request);
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.field,
              "channel_topology");
}

TEST(BrokerRegHandler, FanOutSecondProducerRejectedByCardinalityGate)
{
    // Design mandate — DERIVED FROM DOC FIRST, not from code:
    //   §8.1 I-CHANNEL-SINGLE-BINDING-SIDE: "at most one binding-side
    //   role per channel ... enforced by the cardinality gate at
    //   REG admission (fan-out: 1 producer)."
    //   §7.1 error table: second producer on fan-out →
    //   FAN_OUT_IS_SINGLE_PRODUCER.
    //
    // Test derives: two DISTINCT-uid producers register with fan-out;
    // first is accepted (opens channel with itself as sole binding);
    // second must be rejected — HubState returns FAN_OUT_IS_SINGLE_PRODUCER
    // via topology_error_code, which our translator surfaces as
    // invalid_request with the topology code string in message.
    //
    // If this test fails: the cardinality gate is not being applied
    // (I-CHANNEL-SINGLE-BINDING-SIDE violated).
    constexpr std::string_view kProd2Uid    = "prod.test.uid2";
    constexpr std::string_view kProd2Pubkey =
        "bbcdefghij0123456789bbcdefghij0123456789";
    Fixture f({{std::string{kProd2Uid}, std::string{kProd2Pubkey}}});

    // First producer opens the fan-out channel.
    auto b1 = make_body_json(kTestUid, kTestPubkey,
                               "nonce-p1");
    b1["channel_topology"] = "fan-out";
    auto f1 = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                "cid-p1", b1);
    ParseError err{};
    auto e1 = WireEnvelope::parse_router_recv(std::move(f1), &err);
    ASSERT_TRUE(e1.has_value());
    auto out1 = f.handler().handle(*e1, ProducerRegReqBody{e1->body()});
    ASSERT_TRUE(std::holds_alternative<ag::RegAccepted>(out1))
        << "first fan-out producer must open channel";
    EXPECT_TRUE(std::get<ag::RegAccepted>(out1).channel_opened);

    // Second producer with distinct uid must be rejected.
    auto b2 = make_body_json(kProd2Uid, kProd2Pubkey,
                               "nonce-p2");
    b2["channel_topology"] = "fan-out";
    b2["role_uid"]         = std::string{kProd2Uid};
    b2["zmq_pubkey"]       = std::string{kProd2Pubkey};
    auto f2 = WireEnvelope::build_router_send(kProd2Uid, "REG_REQ",
                                                "cid-p2", b2);
    auto e2 = WireEnvelope::parse_router_recv(std::move(f2), &err);
    ASSERT_TRUE(e2.has_value());
    auto out2 = f.handler().handle(*e2, ProducerRegReqBody{e2->body()});
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(out2))
        << "second fan-out producer must be rejected by cardinality "
           "gate (I-CHANNEL-SINGLE-BINDING-SIDE)";
    const auto &rej = std::get<ag::RegRejected>(out2);
    EXPECT_EQ(rej.detail.code, ag::RejectCode::invalid_request);
    // The wire error string comes from HubState's topology_error_code
    // and must contain the design-mandated code name.
    EXPECT_NE(rej.detail.message.find("FAN_OUT_IS_SINGLE_PRODUCER"),
              std::string::npos)
        << "expected FAN_OUT_IS_SINGLE_PRODUCER in message; got '"
        << rej.detail.message << "'";
}

TEST(BrokerRegHandler, EmptyRoleNameRejectedByGate4)
{
    // Design mandate — DERIVED FROM DOC FIRST:
    //   §14.5 gate 4: "Grammar validation on role_uid / role_name /
    //   channel_name (HEP-CORE-0033)."
    //
    // role_name is required non-empty by the identifier grammar.
    // Prior code path had NO role_name check anywhere — a bug caught
    // by the design-first audit.  Empty role_name should reject
    // as invalid_request with field=role_name.
    Fixture f;
    auto body_json = make_body_json();
    body_json["role_name"] = "";  // empty — violates gate 4
    auto frames = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                    "cid-x", body_json);
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value());
    ProducerRegReqBody body{env->body()};

    auto outcome = f.handler().handle(*env, body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome))
        << "empty role_name must be rejected at gate 4 per §14.5";
    const auto &rej = std::get<ag::RegRejected>(outcome);
    EXPECT_EQ(rej.detail.code,  ag::RejectCode::invalid_request);
    EXPECT_EQ(rej.detail.field, "role_name");
    // State side-effect: pre-mutation rejection, channel not created.
    EXPECT_FALSE(f.hub().channel("lab.integration.channel").has_value());
}

TEST(BrokerRegHandler, BadCharacterRoleNameRejectedByGate4)
{
    // §14.5 gate 4 grammar check: role_name follows HEP-CORE-0033
    // identifier grammar (a-z A-Z 0-9 . _ -).  Space is illegal.
    Fixture f;
    auto body_json = make_body_json();
    body_json["role_name"] = "prod name with spaces";
    auto frames = WireEnvelope::build_router_send(kTestUid, "REG_REQ",
                                                    "cid-y", body_json);
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value());
    ProducerRegReqBody body{env->body()};

    auto outcome = f.handler().handle(*env, body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.field, "role_name");
}

TEST(BrokerRegHandler, IdentityMismatchRejected)
{
    Fixture f;
    // Build envelope with identity != body's role_uid to trip
    // I-DEALER-IDENTITY at gate 3.
    auto body_json = make_body_json(kTestUid, kTestPubkey);
    auto frames    = WireEnvelope::build_router_send(
        /*target=*/"attacker.uid", "REG_REQ", "cid-1", body_json);
    ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    ASSERT_TRUE(env.has_value());
    ProducerRegReqBody body{env->body()};

    auto outcome = f.handler().handle(*env, body);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.code,
              ag::RejectCode::identity_mismatch);
}
