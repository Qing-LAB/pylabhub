/**
 * @file test_pattern4_broker_wire_smoke.cpp
 * @brief BrokerWireClient smoke test (HEP-CORE-0042 Phase 2.4a).
 *
 * Verifies the raw-DEALER wire client can:
 *   - complete the CURVE handshake against a real broker subprocess;
 *   - send a well-formed REG_REQ frame ([C, "REG_REQ", body]);
 *   - receive a reply frame the broker sends back (REG_ACK on success;
 *     ERROR on schema-validation failure — either satisfies the smoke
 *     goal of "the round-trip works").
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

#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>

#include <gtest/gtest.h>

#include <chrono>
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
    const fs::path temp_dir = make_test_temp_dir("broker_wire_smoke");
    const std::string client_uid = "wire_client.smoke";
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
    cfg.broker_endpoint = setup.broker_endpoint;
    cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cfg.client_pubkey   = role_kp.public_z85;
    cfg.client_seckey   = role_kp.secret_z85;

    BrokerWireClient client(ctx, cfg);

    // ── 4. Send REG_REQ + expect a reply ──
    //
    // Body carries the canonical HEP-0036 §5b fields the broker needs
    // to validate a producer REG_REQ.  This test is not asserting on
    // the reply's success/failure semantics — the smoke goal is "the
    // round-trip works," so ERROR is as good as REG_ACK.  What we do
    // pin: SOME reply arrives within the budget.
    nlohmann::json req;
    req["role_uid"]      = client_uid;
    req["role"]           = "producer";
    req["role_type"]     = "test.producer";
    req["channel_name"]  = "ch.wire_smoke";
    req["data_transport"] = "zmq";
    req["short_tag"]     = "wire.smoke";
    req["schema_hash"]   = 0u;
    req["slot_bytes"]    = 4096u;
    req["slot_count"]    = 4u;

    auto reply = client.receive(std::chrono::milliseconds{kMidTimeoutMs});
    // Nothing yet — the CURVE handshake is async.  send() then
    // receive() with a fresh budget covers both.  (Above receive() is
    // a NOP-poll to drain any spurious pre-handshake frame; expected
    // to return nullopt.)
    ASSERT_FALSE(reply.has_value()) << "unexpected pre-request frame received";

    client.send("REG_REQ", req);
    auto reply2 = client.receive(std::chrono::milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(reply2.has_value())
        << "BrokerWireClient did not receive any reply within budget "
           "(CURVE handshake failed silently, broker never sent, or client "
           "poll starved)";

    // The reply MUST be one of the canonical shapes the broker sends
    // back to a REG_REQ.  We don't pin which — this smoke covers only
    // that the wire round-trip completes.
    const std::string &kind = reply2->first;
    EXPECT_TRUE(kind == "REG_ACK" || kind == "ERROR")
        << "unexpected reply msg_type='" << kind << "'";

    // ── 5. Teardown ──
    broker.signal_quit();
    ExpectWorkerOk(broker);
}

} // namespace
