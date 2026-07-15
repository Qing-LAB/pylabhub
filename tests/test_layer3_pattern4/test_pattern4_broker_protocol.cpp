/**
 * @file test_pattern4_broker_protocol.cpp
 * @brief Pattern 4 broker-protocol wire tests.
 *
 * These are the successors of the wire-only workers formerly hosted
 * under `tests/test_layer3_datahub/workers/datahub_broker_protocol_workers.cpp`
 * against the retired in-process HubHostBrokerHandle harness (see
 * `docs/README/README_testing.md` line 565 antipattern +
 * HEP-CORE-0036 §7.4 single-pumper invariant).  Broker runs in its
 * own subprocess; the parent test drives wire traffic via
 * BrokerWireClient.
 *
 * Migration reference: task #54 (Round 1 of the sweep).
 */
#include "broker_wire_client.h"
#include "pattern4_helpers.h"

#include "shared_test_helpers.h"
#include "test_patterns.h"

#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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

class Pattern4BrokerProtocolTest : public IsolatedProcessTest
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

}  // namespace

// ─── BAND_JOIN/LEAVE correlation_id echo (HEP-CORE-0046 §14 + broker_proto 5)
//
// Pins four cases:
//   1. BAND_JOIN_ACK success — corr_id in body matches request.
//   2. BAND_LEAVE_ACK success — corr_id in body matches request.
//   3. BAND_LEAVE error (NOT_A_MEMBER) — corr_id in body matches request.
//   4. Request omits body corr_id — ACK still carries the Frame 3
//      authoritative echo (HEP-CORE-0046 I-CORRELATION-STABLE); dispatch
//      layer injects it into the body.

TEST_F(Pattern4BrokerProtocolTest, WireConformance_Band_CorrIdEcho)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string channel = "tr1.bandcorr.pid" + std::to_string(::getpid());
    const std::string uid     = "prod." + channel;
    const std::string role_nm = channel;
    const std::string band    = "!tr1.bcorr.pid" + std::to_string(::getpid());

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_band_corrid");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                             {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto    &role_kp = setup.curve.role(uid);

    BrokerWireClient::Config cfg;
    cfg.broker_endpoint = setup.broker_endpoint;
    cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cfg.client_pubkey   = role_kp.public_z85;
    cfg.client_seckey   = role_kp.secret_z85;
    cfg.client_role_uid = uid;

    BrokerWireClient client(ctx, cfg);

    // ── Case 1: BAND_JOIN success echoes corr_id ─────────────
    const std::string join_corr = "test.band.join.corr.001";
    nlohmann::json join_req;
    join_req["band"]           = band;
    join_req["role_uid"]       = uid;
    join_req["role_name"]      = role_nm;
    join_req["correlation_id"] = join_corr;
    auto join_resp = client.request("BAND_JOIN_REQ", join_req, "BAND_JOIN_ACK",
                                     milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(join_resp.has_value())
        << "BAND_JOIN_REQ timed out";
    ASSERT_EQ(join_resp->value("status", std::string{}), "success")
        << "BAND_JOIN_REQ failed; body=" << join_resp->dump();
    ASSERT_TRUE(join_resp->contains("correlation_id"))
        << "BAND_JOIN_ACK missing correlation_id field "
           "(broker_proto 5 contract; broker_service.cpp B1 fix); "
           "body=" << join_resp->dump();
    EXPECT_EQ(join_resp->at("correlation_id").get<std::string>(), join_corr)
        << "BAND_JOIN_ACK echoed wrong correlation_id";

    // ── Case 2: BAND_LEAVE success echoes corr_id ────────────
    const std::string leave_corr = "test.band.leave.corr.002";
    nlohmann::json leave_req;
    leave_req["band"]           = band;
    leave_req["role_uid"]       = uid;
    leave_req["correlation_id"] = leave_corr;
    auto leave_resp = client.request("BAND_LEAVE_REQ", leave_req,
                                      "BAND_LEAVE_ACK",
                                      milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(leave_resp.has_value())
        << "BAND_LEAVE_REQ timed out";
    ASSERT_EQ(leave_resp->value("status", std::string{}), "success")
        << "BAND_LEAVE_REQ failed; body=" << leave_resp->dump();
    ASSERT_TRUE(leave_resp->contains("correlation_id"))
        << "BAND_LEAVE_ACK missing correlation_id; body=" << leave_resp->dump();
    EXPECT_EQ(leave_resp->at("correlation_id").get<std::string>(), leave_corr)
        << "BAND_LEAVE_ACK echoed wrong correlation_id";

    // ── Case 3: BAND_LEAVE NOT_A_MEMBER error echoes corr_id ─
    // The role left successfully in Case 2; another LEAVE must produce
    // typed `NOT_A_MEMBER` (HEP-CORE-0030 S4 amendment).  The error
    // path must also carry corr_id.
    const std::string err_corr = "test.band.leave.err.corr.003";
    nlohmann::json leave_req_err;
    leave_req_err["band"]           = band;
    leave_req_err["role_uid"]       = uid;
    leave_req_err["correlation_id"] = err_corr;
    auto err_resp = client.request("BAND_LEAVE_REQ", leave_req_err,
                                    "BAND_LEAVE_ACK",
                                    milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(err_resp.has_value())
        << "BAND_LEAVE_REQ (error path) timed out";
    ASSERT_EQ(err_resp->value("status", std::string{}), "error");
    EXPECT_EQ(err_resp->value("error_code", std::string{}), "NOT_A_MEMBER");
    ASSERT_TRUE(err_resp->contains("correlation_id"))
        << "BAND_LEAVE error reply missing correlation_id; body="
        << err_resp->dump();
    EXPECT_EQ(err_resp->at("correlation_id").get<std::string>(), err_corr)
        << "BAND_LEAVE error reply echoed wrong correlation_id";

    // ── Case 4: Frame 3 correlation_id is authoritative ────────
    // When the request omits body-level correlation_id, BrokerWireClient
    // still stamps Frame 3 per HEP-CORE-0046 §14 and the broker's
    // dispatch layer injects it into the ACK body so tests inspecting
    // body["correlation_id"] see the authoritative Frame 3 value.
    nlohmann::json rejoin_req;
    rejoin_req["band"]      = band;
    rejoin_req["role_uid"]  = uid;
    rejoin_req["role_name"] = role_nm;
    auto rejoin = client.request("BAND_JOIN_REQ", rejoin_req, "BAND_JOIN_ACK",
                                  milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(rejoin.has_value())
        << "BAND_JOIN_REQ (no body correlation_id) timed out";
    ASSERT_EQ(rejoin->value("status", std::string{}), "success");
    ASSERT_TRUE(rejoin->contains("correlation_id"))
        << "BAND_JOIN_ACK must echo Frame 3 correlation_id per "
           "I-CORRELATION-STABLE regardless of request body content; body="
        << rejoin->dump();
    EXPECT_FALSE(rejoin->at("correlation_id").get<std::string>().empty())
        << "echoed correlation_id must be non-empty per §14";

    broker.signal_quit();
}
