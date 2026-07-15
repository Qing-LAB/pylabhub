/**
 * @file test_pattern4_broker_wire_smoke.cpp
 * @brief BrokerWireClient smoke test (HEP-CORE-0042 Phase 2.4a).
 *
 * Verifies the raw-DEALER wire client can:
 *   - complete the CURVE handshake against a real broker subprocess;
 *   - send a well-formed producer REG_REQ frame ([C, "REG_REQ", body])
 *     whose `role_uid` matches the HEP-CORE-0036 §5b naming grammar
 *     (`(prod|cons|proc).<name>.<unique>`); the broker's
 *     validate_identity_fields check accepts it and dispatches to the
 *     REG_ACK success path.
 *   - receive the REG_ACK reply — this pins the success codepath, not
 *     the schema-reject path — so a future regression in REG_ACK
 *     dispatch (e.g. body construction, envelope shape) breaks the
 *     smoke instead of silently degrading to an ERROR round-trip.
 *
 * Scope: proves the L3 broker-only test harness is viable.  HEP-0042
 * attach-coordination scenarios (fast-path admit, wait-path enqueue,
 * APPLIED_REQ drain, disconnect/close/timeout drains) land in Phase
 * 2.4b once this smoke is green.
 */
#include "broker_wire_client.h"
#include "pattern4_helpers.h"

#include "shared_test_helpers.h"
#include "test_patterns.h"

#include "utils/role_reg_payload.hpp"
#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>       // ::setenv / ::unsetenv (HEP-0032 §8 strict-mode test)
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::make_temp_dir;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4BrokerWireSmokeTest : public IsolatedProcessTest
{
protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    fs::path make_test_temp_dir(std::string_view label)
    {
        auto dir = make_temp_dir(label);
        paths_to_clean_.push_back(dir);
        return dir;
    }

    std::vector<fs::path> paths_to_clean_;
};

// ─── Smoke: parent client speaks the wire against a broker subprocess ─────

TEST_F(Pattern4BrokerWireSmokeTest, ClientCurveHandshakeAndReply)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    // ── 1. Setup: temp dir + Pattern4Setup with one client uid ──
    //
    // HEP-CORE-0036 §5b + naming.hpp grammar: role uid MUST be
    // `(prod|cons|proc).<name>.<unique>`.  Using a non-conforming uid
    // makes the broker short-circuit REG_REQ with INVALID_REQUEST
    // before ever exercising REG_ACK, which would silently pin this
    // smoke test to the error branch and hide REG_ACK regressions.
    const std::string client_uid = "prod.smoke.uid00000001";
    const fs::path temp_dir = make_test_temp_dir("broker_wire_smoke");
    const auto setup = make_pattern4_setup({client_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    // ── 2. Broker subprocess — reuse pattern4_smoke.broker ──
    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});

    // Wait for the broker to bind + log its endpoint.  Absence within
    // the mid budget means the broker never came up; the client
    // construct below would then fail with a CURVE handshake timeout
    // — the log assertion surfaces the real cause first.
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    // ── 3. Construct the BrokerWireClient against the running broker ──
    // Local ZMQ context — the parent test doesn't run the LifecycleGuard
    // module for the shared context (that's a subprocess concern).  A
    // dedicated context here keeps the client isolated from any other
    // parent-side ZMQ usage and avoids needing lifecycle setup.
    zmq::context_t ctx;
    const auto &role_kp = setup.curve.role(client_uid);

    BrokerWireClient::Config cfg;
    cfg.broker_endpoint  = setup.broker_endpoint;
    cfg.broker_pubkey    = setup.curve.hub.public_z85;
    cfg.client_pubkey    = role_kp.public_z85;
    cfg.client_seckey    = role_kp.secret_z85;
    cfg.client_role_uid  = client_uid;

    BrokerWireClient client(ctx, cfg);

    // ── 4. Send REG_REQ + expect REG_ACK ──
    //
    // Body built via the canonical `build_producer_reg_payload` helper
    // so the smoke pins the CURRENT HEP-CORE-0036 §5b wire shape
    // rather than a hand-rolled snapshot that would drift as the
    // schema evolves.  We pin the REG_ACK success path specifically:
    // an ERROR reply here means either the schema regressed or the
    // wire client regressed, both of which the smoke must catch.
    // `request()` returns early on ERROR (with the error body) so a
    // broker-side rejection surfaces the reason instead of burning
    // the whole budget.
    pylabhub::hub::ProducerRegInputs in;
    in.channel           = "ch.wire_smoke";
    in.role_uid          = client_uid;
    in.role_name         = "smoke";
    in.role_type         = "producer";
    in.is_zmq_transport  = true;
    // ZMQ node endpoint — for the smoke this is just the identity the
    // broker records on ChannelEntry.zmq_node_endpoint; no data-plane
    // socket actually binds here (that's a Phase 2.4b concern).  Use a
    // syntactically-valid tcp:// URI on an unused port.
    in.zmq_node_endpoint = "tcp://127.0.0.1:0";
    // Producer's CURVE identity — required per HEP-CORE-0036 §4.1;
    // the broker validates 40-char z85.
    in.zmq_pubkey        = role_kp.public_z85;
    nlohmann::json req   = pylabhub::hub::build_producer_reg_payload(in);

    auto reply = client.request("REG_REQ", req, "REG_ACK",
                                 std::chrono::milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value())
        << "BrokerWireClient did not receive REG_ACK or ERROR within budget "
           "(CURVE handshake failed silently, broker never sent, or client "
           "poll starved)";

    // `request()` returns on REG_ACK OR ERROR.  The success codepath
    // sets `status="success"`; a schema/wire regression would set
    // "error" or carry an `error_code` field.  Assert success to pin
    // that the broker exercised the REG_ACK dispatch.
    const std::string status = reply->value("status", "");
    ASSERT_EQ(status, "success")
        << "REG_REQ round-trip did not exercise REG_ACK success path — "
           "reply body: " << reply->dump();

    // ── 5. Teardown ──
    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── HEP-CORE-0032 §8 strict-mode ABI reject (task #326 L3) ─────────
//
// End-to-end proof that a broker configured with
// `strict_abi_mismatch=true` rejects a REG_REQ carrying an
// abi_fingerprint envelope that differs on a MAJOR axis.  The client
// bumps `shm_major` in the envelope after `build_producer_reg_payload`
// generates it, which is a minimal edit that simulates what a
// mismatched-build role would send.  Broker's `verify_peer_versions`
// returns incompatible + `major_mismatch.shm=true`; the helper flags
// the outcome and `handle_reg_req` returns
// `error_code='abi_major_mismatch'` before any state mutation.
//
// Env var `PLH_TEST_STRICT_ABI_MISMATCH=1` enables strict mode on the
// broker worker (see pattern4_smoke_workers.cpp) — avoids extending
// the Pattern4Setup schema for a single-test flag.
TEST_F(Pattern4BrokerWireSmokeTest, StrictModeRejects_MajorAbiMismatchOnRegReq)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string client_uid = "prod.strict.uid00000001";
    const fs::path temp_dir = make_test_temp_dir("broker_strict_abi");
    const auto setup = make_pattern4_setup({client_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    // Enable strict mode BEFORE the child inherits our env.  Unset in
    // the RAII-style cleanup so subsequent tests in the binary don't
    // inherit strict mode.
    ::setenv("PLH_TEST_STRICT_ABI_MISMATCH", "1", /*overwrite=*/1);
    struct EnvGuard {
        ~EnvGuard() { ::unsetenv("PLH_TEST_STRICT_ABI_MISMATCH"); }
    } env_guard;

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});
    expect_log(broker,
                "Pattern4Broker: strict_abi_mismatch=true",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &role_kp = setup.curve.role(client_uid);

    BrokerWireClient::Config cfg;
    cfg.broker_endpoint  = setup.broker_endpoint;
    cfg.broker_pubkey    = setup.curve.hub.public_z85;
    cfg.client_pubkey    = role_kp.public_z85;
    cfg.client_seckey    = role_kp.secret_z85;
    cfg.client_role_uid  = client_uid;

    BrokerWireClient client(ctx, cfg);

    pylabhub::hub::ProducerRegInputs in;
    in.channel           = "ch.strict_abi";
    in.role_uid          = client_uid;
    in.role_name         = "strict";
    in.role_type         = "producer";
    in.is_zmq_transport  = true;
    in.zmq_node_endpoint = "tcp://127.0.0.1:0";
    in.zmq_pubkey        = role_kp.public_z85;
    nlohmann::json req = pylabhub::hub::build_producer_reg_payload(in);

    // Bump shm_major by +1 to simulate a mismatched build.  The rest of
    // the envelope stays consistent with `current()`; only this axis
    // drifts, so mismatched_axes must contain exactly "shm".
    ASSERT_TRUE(req.contains("abi_fingerprint"));
    const unsigned original_shm_major =
        req["abi_fingerprint"].value("shm_major", 0u);
    req["abi_fingerprint"]["shm_major"] = original_shm_major + 1;

    auto reply = client.request("REG_REQ", req, "REG_ACK",
                                 std::chrono::milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value())
        << "BrokerWireClient did not receive REG_ACK or ERROR within budget";

    // Expected shape: broker rejects with error_code=abi_major_mismatch.
    // A "success" here means strict mode was NOT active — a regression
    // in the reject path or in the env-var gate.
    const std::string status     = reply->value("status", "");
    const std::string error_code = reply->value("error_code", "");
    EXPECT_EQ(status, "error")
        << "Strict-mode broker must reject major mismatch — reply body: "
        << reply->dump();
    EXPECT_EQ(error_code, "abi_major_mismatch")
        << "Reject must carry HEP-0032 §8.7 error_code — reply body: "
        << reply->dump();

    // Broker log must carry the §8.6 MAJOR_MISMATCH_REJECTED verdict
    // for the shm axis specifically.  This double-pins the log-content
    // stability contract per §8.4 (log-message content = MINOR-bump
    // concern) alongside the wire-level status.
    expect_log(broker,
                "event=AbiFingerprintReceived",
                std::chrono::milliseconds{kMidTimeoutMs});
    expect_log(broker,
                "verdict='MAJOR_MISMATCH_REJECTED'",
                std::chrono::milliseconds{kMidTimeoutMs});
    expect_log(broker,
                "mismatched_axes='shm'",
                std::chrono::milliseconds{kMidTimeoutMs});

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

} // namespace
