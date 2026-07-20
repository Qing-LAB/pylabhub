// tests/test_layer1_base/test_reg_admission_pipeline.cpp
//
// ⚠ PICK-UP POINT for task #57 (HEP-0046 Phase B).  These L1 tests pin the
//   pipeline ORCHESTRATION (gates → commit → outcome) with stub commits; the
//   pipeline is not yet the live broker path.  When task #57 widens
//   `RegRequest` (it currently drops producer_pid / inbox_* /
//   shm_capability_endpoint / schema_packing / flexzone_* / consumer fields)
//   and adds the consumer path, add cases here for the new fields + the
//   ConsumerRegReqBody variant.  Full parity list: task #57.
//
// L1 tests for the REG-family admission pipeline.  Exercises the three
// outcome branches (Accepted / Rejected / Pended) with in-process stub
// commit callbacks — no HubState, no ZMQ traffic.  Validates that:
//   - Pre-mutation gate failures propagate as RegRejected verbatim
//   - Pre-mutation gates run BEFORE commit (commit not invoked on
//     rejection — no state mutation on a bad REQ)
//   - Typed RegRequest reaches commit populated from the ProducerRegReqBody
//   - Commit's variant outcome flows through unchanged
//
// Together with test_admission_gates.cpp this pins the state transitions
// diagrammed in reg_admission_pipeline.hpp: Received → gate pipeline →
// [Rejected] or [protocol commit] → [Accepted|Rejected|Pended].

#include "utils/reg_admission_pipeline.hpp"
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
using pylabhub::wire::ProducerRegReqBody;

namespace
{

nlohmann::json make_reg_req_body_json(std::string_view role_uid,
                                        std::string_view pubkey,
                                        std::string_view  nonce,
                                        std::uint64_t     wall_ts)
{
    nlohmann::json body;
    body["channel_name"]    = "lab.test.channel";
    body["role_uid"]        = std::string{role_uid};
    body["role_type"]       = "producer";
    body["role_name"]       = "prod-1";
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

// Build a fully-round-tripped WireEnvelope + ProducerRegReqBody the pipeline
// will accept.  Returns both by value; caller owns.
struct EnvelopePair
{
    WireEnvelope env;
    ProducerRegReqBody   body;
};

EnvelopePair make_valid_pair(
    std::string_view role_uid      = "prod.test.uid1",
    std::string_view pubkey        = "abcdefghij0123456789abcdefghij0123456789",
    std::string_view  nonce        = "nonce.test.001",
    std::uint64_t     wall_ts      = 1'000'000ULL)
{
    auto body_json = make_reg_req_body_json(role_uid, pubkey,
                                              nonce, wall_ts);

    // Build+parse a router envelope so envelope_hash is stamped and
    // validated on the resulting typed body.
    auto frames = WireEnvelope::build_router_send(role_uid, "REG_REQ",
                                                    "cid-1", body_json);
    pylabhub::wire::ParseError err{};
    auto env = WireEnvelope::parse_router_recv(std::move(frames), &err);
    if (!env) throw std::runtime_error("make_valid_pair: parse failed");
    ProducerRegReqBody body{env->body()};
    return {std::move(*env), std::move(body)};
}

// Stub known-role + no-rotation + fresh-nonce callback bundle.  Matches
// the pattern from test_admission_gates.cpp but reused here so both
// suites verify against the same gate contract.
ag::AdmissionCallbacks make_permissive_gate_callbacks(
    std::unordered_set<std::string> &nonce_dedup_out)
{
    ag::AdmissionCallbacks cb;
    cb.lookup_known_role = [](std::string_view uid, std::string_view pk)
                                -> ag::KnownRoleLookup {
        if (uid == "prod.test.uid1" &&
            pk  == "abcdefghij0123456789abcdefghij0123456789")
            return ag::KnownRoleLookup::binding_matches;
        return ag::KnownRoleLookup::uid_unknown;
    };
    cb.check_key_rotation = [](std::string_view, std::string_view)
                                 -> ag::KeyRotationCheck {
        return ag::KeyRotationCheck::not_yet_registered;
    };
    cb.record_and_check_nonce = [&nonce_dedup_out](std::string_view uid,
                                                    std::string_view nonce) {
        std::string key{uid};
        key.push_back('|');
        key.append(nonce);
        return nonce_dedup_out.insert(std::move(key)).second;
    };
    cb.wall_now_ms = []() { return std::uint64_t{1'000'000ULL}; };
    return cb;
}

ag::AdmissionContext make_ctx(const ag::AdmissionCallbacks &cb)
{
    ag::AdmissionContext c;
    c.cb                = &cb;
    c.skew_tolerance_ms = 30'000ULL;
    c.nonce_window_ms   = 10'000ULL;
    return c;
}

}  // namespace

// ── Accepted flow ─────────────────────────────────────────────────────

TEST(RegAdmissionPipeline, AcceptedPathReachesCommitWithTypedRequest)
{
    auto pair = make_valid_pair();
    std::unordered_set<std::string> nonces;
    auto gate_cb = make_permissive_gate_callbacks(nonces);
    auto ctx     = make_ctx(gate_cb);

    // Commit callback captures its input so we can assert the pipeline
    // populated RegRequest correctly from the ProducerRegReqBody accessors.
    ag::RegRequest captured;
    bool           commit_invoked = false;

    ag::RegCommitFn commit = [&](const ag::RegRequest &r) -> ag::RegOutcome {
        commit_invoked = true;
        captured       = r;
        ag::RegAccepted accepted;
        accepted.request              = r;
        accepted.side                 = ag::AdmissionSide::binding;
        accepted.channel_opened       = true;
        accepted.assigned_instance_id = 1U;
        accepted.channel_version      = 1U;
        accepted.pending_notifies     = {};
        return accepted;
    };

    auto outcome = ag::run_reg_admission(pair.env, pair.body, ctx,
                                                    commit);

    ASSERT_TRUE(commit_invoked);
    EXPECT_EQ(captured.channel_name,    "lab.test.channel");
    EXPECT_EQ(captured.role_uid,        "prod.test.uid1");
    EXPECT_EQ(captured.role_type,       "producer");
    EXPECT_EQ(captured.channel_topology,"one-to-one");
    EXPECT_EQ(captured.data_transport,  "zmq");
    EXPECT_EQ(captured.schema_hash,     "deadbeef");
    // schema_version retired per C2 — version rides inside schema_id.

    ASSERT_TRUE(std::holds_alternative<ag::RegAccepted>(outcome));
    auto &accepted = std::get<ag::RegAccepted>(outcome);
    EXPECT_EQ(accepted.side, ag::AdmissionSide::binding);
    EXPECT_TRUE(accepted.channel_opened);
    EXPECT_EQ(accepted.assigned_instance_id, 1U);
    EXPECT_EQ(accepted.channel_version, 1U);
}

// ── Rejected flow: pre-mutation gate fails, commit not called ─────────
//
// C3 retired the broker_proto gate — protocol drift is now caught by
// abi_fingerprint at the ABI layer, not per-message.  Nonce-replay gate
// remains and is the cheapest observable pre-mutation failure.

TEST(RegAdmissionPipeline, RejectedByGateStopsBeforeCommit)
{
    auto pair = make_valid_pair();
    std::unordered_set<std::string> nonces;
    // Pre-seed the dedup set so the pipeline's first look-up sees a replay.
    nonces.insert(std::string{"prod.test.uid1|nonce.test.001"});
    auto gate_cb = make_permissive_gate_callbacks(nonces);
    auto ctx     = make_ctx(gate_cb);

    bool commit_invoked = false;
    ag::RegCommitFn commit = [&](const ag::RegRequest &) -> ag::RegOutcome {
        commit_invoked = true;
        return ag::RegAccepted{};
    };

    auto outcome = ag::run_reg_admission(pair.env, pair.body, ctx,
                                                    commit);

    EXPECT_FALSE(commit_invoked)
        << "pre-mutation gate rejection must not invoke commit "
           "(no state mutation on bad REQ)";

    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    auto &rejected = std::get<ag::RegRejected>(outcome);
    EXPECT_EQ(rejected.detail.code, ag::RejectCode::replay_or_skew);
    EXPECT_EQ(rejected.detail.field, "client_nonce");
}

// ── Pended flow: commit returns Pended, pipeline forwards ─────────────

TEST(RegAdmissionPipeline, PendedOutcomeForwards)
{
    auto pair = make_valid_pair();
    std::unordered_set<std::string> nonces;
    auto gate_cb = make_permissive_gate_callbacks(nonces);
    auto ctx     = make_ctx(gate_cb);

    ag::RegCommitFn commit = [](const ag::RegRequest &) -> ag::RegOutcome {
        ag::RegPended p;
        p.reason     = "awaiting_channel_created";
        p.my_version = 0U;
        return p;
    };

    auto outcome = ag::run_reg_admission(pair.env, pair.body, ctx,
                                                    commit);
    ASSERT_TRUE(std::holds_alternative<ag::RegPended>(outcome));
    auto &pended = std::get<ag::RegPended>(outcome);
    EXPECT_EQ(pended.reason, "awaiting_channel_created");
    EXPECT_EQ(pended.my_version, 0U);
}

// ── Rejected by commit (protocol-level topology gate) ─────────────────

TEST(RegAdmissionPipeline, CommitLevelRejectionForwards)
{
    auto pair = make_valid_pair();
    std::unordered_set<std::string> nonces;
    auto gate_cb = make_permissive_gate_callbacks(nonces);
    auto ctx     = make_ctx(gate_cb);

    ag::RegCommitFn commit = [](const ag::RegRequest &) -> ag::RegOutcome {
        // Simulate topology cardinality gate firing:
        // FAN_OUT_IS_SINGLE_PRODUCER on second admission.
        ag::RegRejected r;
        r.detail.code    = ag::RejectCode::invalid_request;
        r.detail.field   = "channel_topology";
        r.detail.message = "FAN_OUT_IS_SINGLE_PRODUCER";
        return r;
    };

    auto outcome = ag::run_reg_admission(pair.env, pair.body, ctx,
                                                    commit);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.message,
              "FAN_OUT_IS_SINGLE_PRODUCER");
}

// ── Missing commit callback surfaces as broker-internal reject ────────

TEST(RegAdmissionPipeline, MissingCommitCallbackSurfacesBrokerInternalError)
{
    auto pair = make_valid_pair();
    std::unordered_set<std::string> nonces;
    auto gate_cb = make_permissive_gate_callbacks(nonces);
    auto ctx     = make_ctx(gate_cb);

    // Empty std::function is a BROKER misconfiguration (an unbound
    // callback), not a client wire violation.  Under the CURVE
    // integrity frame the distinction matters — INVALID_REQUEST maps
    // to "your wire is malformed" which would mislead a legitimate
    // client into hunting for wire bugs it doesn't have.  Design
    // mandate: BROKER_INTERNAL_ERROR.  A prior iteration returned
    // INVALID_REQUEST here and the test happily asserted the buggy
    // behavior; corrected by the test audit.
    ag::RegCommitFn commit;  // empty
    auto outcome = ag::run_reg_admission(pair.env, pair.body, ctx, commit);
    ASSERT_TRUE(std::holds_alternative<ag::RegRejected>(outcome));
    EXPECT_EQ(std::get<ag::RegRejected>(outcome).detail.code,
              ag::RejectCode::broker_internal_error);
}
