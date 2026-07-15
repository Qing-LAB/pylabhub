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
#include "wire_conformance.h"

#include "plh_platform.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::make_temp_dir;
using pylabhub::tests::pattern4::pick_unused_port;
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

    // ── Fixture wire helpers — shared across the broker-protocol tests ──
    //
    // GTest note: fatal-assertion macros (`ASSERT_*`) inside these methods
    // `return;` from the METHOD, not the calling test.  Callers MUST wrap
    // invocations in `ASSERT_NO_FATAL_FAILURE(...)` so a failed REG /
    // HEARTBEAT aborts the test at the correct call site rather than
    // cascading into a misattributed downstream failure.

    /// Construct a BrokerWireClient masquerading as `role_uid`.  role_uid
    /// becomes the DEALER routing_id per I-DEALER-IDENTITY (HEP-CORE-0046
    /// §8.1); the keypair comes from the shared curve setup so the parent
    /// never touches the process KeyStore (BrokerWireClient is
    /// KeyStore-independent by design).
    BrokerWireClient make_wire_client(
        zmq::context_t                                 &ctx,
        const pylabhub::tests::pattern4::Pattern4Setup &setup,
        const std::string                              &role_uid)
    {
        const auto &kp = setup.curve.role(role_uid);
        BrokerWireClient::Config c;
        c.broker_endpoint = setup.broker_endpoint;
        c.broker_pubkey   = setup.curve.hub.public_z85;
        c.client_pubkey   = kp.public_z85;
        c.client_seckey   = kp.secret_z85;
        c.client_role_uid = role_uid;
        return BrokerWireClient(ctx, c);
    }

    /// Producer REG_REQ → assert REG_ACK.status=="success".  ZMQ
    /// transport (Pattern 4 convention); the pubkey is the role's curve
    /// identity so ZAP authorizes and the broker records a valid
    /// producer_pubkey (HEP-CORE-0036 §5b.4).  Writes the REG_ACK body
    /// to `*out_ack` when non-null so shape tests can inspect it.
    void register_producer(
        BrokerWireClient                               &client,
        const pylabhub::tests::pattern4::Pattern4Setup &setup,
        const std::string                              &channel,
        const std::string                              &uid,
        nlohmann::json                                 *out_ack = nullptr)
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel          = channel;
        in.role_uid         = uid;
        in.role_name        = "test_producer";
        in.role_type        = "producer";
        in.is_zmq_transport = true;
        in.zmq_node_endpoint =
            "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
        in.zmq_pubkey = setup.curve.role(uid).public_z85;
        auto reply    = client.request(
            "REG_REQ", pylabhub::hub::build_producer_reg_payload(in),
            "REG_ACK", std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "REG_REQ timed out for " << uid;
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "REG_REQ failed for " << uid << "; body=" << reply->dump();
        if (out_ack != nullptr) *out_ack = *reply;
    }

    /// Producer HEARTBEAT_NOTIFY (fire-and-forget) → clears the R6
    /// producer-kLive gate that CONSUMER_REG_REQ trips (HEP-CORE-0036
    /// §5.2 / HEP-CORE-0023 §2.5.3).  The old BRC `register_consumer`
    /// masked this with an implicit CHANNEL_NOT_READY retry loop; the
    /// raw wire client gets one reply, so the heartbeat is explicit —
    /// which is what the design requires anyway.  Sleeps briefly so the
    /// broker processes the heartbeat before the consumer REG arrives.
    void producer_heartbeat(BrokerWireClient  &client,
                            const std::string &channel,
                            const std::string &uid)
    {
        nlohmann::json hb;
        hb["channel_name"] = channel;
        hb["role_uid"]     = uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = pylabhub::platform::get_pid();
        client.send("HEARTBEAT_NOTIFY", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    /// Consumer CONSUMER_REG_REQ → assert CONSUMER_REG_ACK.status==
    /// "success".  ZMQ transport, curve-identity pubkey for the
    /// channel-scope allowlist (HEP-CORE-0036 §6.5).  Writes the ACK to
    /// `*out_ack` when non-null.  Caller MUST have registered the
    /// producer AND sent a producer heartbeat first (R6 gate).
    void register_consumer(
        BrokerWireClient                               &client,
        const pylabhub::tests::pattern4::Pattern4Setup &setup,
        const std::string                              &channel,
        const std::string                              &uid,
        nlohmann::json                                 *out_ack = nullptr)
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel        = channel;
        in.role_uid       = uid;
        in.role_name      = "test_consumer";
        in.role_type      = "consumer";
        in.data_transport = "zmq";
        in.zmq_pubkey     = setup.curve.role(uid).public_z85;
        auto reply        = client.request(
            "CONSUMER_REG_REQ", pylabhub::hub::build_consumer_reg_payload(in),
            "CONSUMER_REG_ACK",
            std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "CONSUMER_REG_REQ timed out for " << uid;
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "CONSUMER_REG_REQ failed for " << uid
            << "; body=" << reply->dump();
        if (out_ack != nullptr) *out_ack = *reply;
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

// ─── ROLE_PRESENCE_REQ / ROLE_INFO_REQ (HEP-CORE-0007 §"ROLE_*_REQ") ────────
//
// These query the broker's presence/info registry.  The wire body's
// `role_uid` is the QUERIED SUBJECT, not the caller — so they run the
// `Control_EnvelopeWithQueryRoleUid` dispatch tier (grammar + tag policy
// on the subject uid; NO identity_match against the caller's Frame 0).
// Migrated from the retired in-process HubHostBrokerHandle harness
// (task #54 Round 1).

TEST_F(Pattern4BrokerProtocolTest, RolePresenceReq_UnknownUid_ReturnsFalse)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string querier = "QUERIER-unknown" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_pres_unknown");
    const auto     setup    = make_pattern4_setup({querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           client = make_wire_client(ctx, setup, querier);

    nlohmann::json req;
    req["role_uid"] = "prod.unknown.uiddeadbeef";
    auto resp = client.request("ROLE_PRESENCE_REQ", req, "ROLE_PRESENCE_ACK",
                                milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value())
        << "broker should respond to ROLE_PRESENCE_REQ, not time out";
    EXPECT_FALSE(resp->value("present", true))
        << "unknown uid → present=false; body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RoleInfoReq_UnknownUid_NotFound)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string querier = "QUERIER-unknown2" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_info_unknown");
    const auto     setup    = make_pattern4_setup({querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           client = make_wire_client(ctx, setup, querier);

    nlohmann::json req;
    req["role_uid"] = "prod.unknown.uiddeadbeef";
    auto info = client.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK",
                                milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(info.has_value())
        << "ROLE_INFO_REQ for an unknown uid must get an ACK (found=false), "
           "not time out";
    ASSERT_TRUE(info->contains("found") && info->at("found").is_boolean());
    EXPECT_FALSE(info->at("found").get<bool>())
        << "unknown uid → found=false; body=" << info->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RolePresenceReq_ProducerUid_ReturnsTrue)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "proto.presence.prod" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string querier  = "QUERIER-pres-prod" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_pres_prod");
    const auto     setup    = make_pattern4_setup({prod_uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer(prod, setup, channel, prod_uid));

    auto           q = make_wire_client(ctx, setup, querier);
    nlohmann::json req;
    req["role_uid"] = prod_uid;
    auto resp = q.request("ROLE_PRESENCE_REQ", req, "ROLE_PRESENCE_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "ROLE_PRESENCE_REQ timed out";
    EXPECT_TRUE(resp->value("present", false))
        << "registered producer → present=true; body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RolePresenceReq_ConsumerUid_ReturnsTrue)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "proto.presence.cons" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons.prestest" + suffix;
    const std::string querier  = "QUERIER-pres-cons" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_pres_cons");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer(prod, setup, channel, prod_uid));
    // R6 producer-kLive gate must clear before the consumer can register.
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_consumer(cons, setup, channel, cons_uid));

    auto           q = make_wire_client(ctx, setup, querier);
    nlohmann::json req;
    req["role_uid"] = cons_uid;
    auto resp = q.request("ROLE_PRESENCE_REQ", req, "ROLE_PRESENCE_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "ROLE_PRESENCE_REQ timed out";
    EXPECT_TRUE(resp->value("present", false))
        << "registered consumer → present=true; body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RoleInfoReq_WithInbox_ReturnsInfo)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.roleinfo.withinbox" + suffix;
    const std::string uid     = "prod." + channel;
    const std::string querier = "QUERIER-roleinfo" + suffix;
    const std::string inbox_ep     = "tcp://127.0.0.1:9987";
    const std::string schema_json  =
        R"([{"type":"float64","count":1,"length":0}])";
    const std::string packing = "aligned";

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_info_inbox");
    const auto     setup    = make_pattern4_setup({uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);

    // Register with the inbox advertised.  The base payload comes from
    // the production builder; the inbox_* fields are extra REG_REQ body
    // keys the broker records for ROLE_INFO_ACK (mirrors the old
    // `opts["inbox_*"]` registration).
    pylabhub::hub::ProducerRegInputs in;
    in.channel           = channel;
    in.role_uid          = uid;
    in.role_name         = "InboxProd";
    in.role_type         = "producer";
    in.is_zmq_transport  = true;
    in.zmq_node_endpoint =
        "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
    in.zmq_pubkey        = setup.curve.role(uid).public_z85;
    auto payload              = pylabhub::hub::build_producer_reg_payload(in);
    payload["inbox_endpoint"]    = inbox_ep;
    payload["inbox_schema_json"] = schema_json;
    payload["inbox_packing"]     = packing;
    auto reg = prod.request("REG_REQ", payload, "REG_ACK",
                             milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg.has_value()) << "REG_REQ (with inbox) timed out";
    ASSERT_EQ(reg->value("status", std::string{}), "success")
        << "REG_REQ (with inbox) failed; body=" << reg->dump();

    auto info = make_wire_client(ctx, setup, querier);
    nlohmann::json req;
    req["role_uid"] = uid;
    auto resp = info.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK",
                              milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "ROLE_INFO_REQ timed out";
    EXPECT_EQ(resp->value("inbox_endpoint", std::string{}), inbox_ep)
        << "body=" << resp->dump();
    EXPECT_EQ(resp->value("inbox_packing", std::string{}), packing)
        << "body=" << resp->dump();

    broker.signal_quit();
}

// ─── Wire-conformance ACK-shape regressions (Audit TR1) ────────────────────
//
// Pin the observable key set of major ACK families against their
// authoritative HEP §, asserting both REQUIRED keys AND absence of
// legacy keys.  Migrated from the retired in-process harness (task #54
// Round 1); the shared `wire_conformance.h` helpers emit precise
// diagnostics naming the missing/forbidden key + the HEP § the rule
// comes from.

TEST_F(Pattern4BrokerProtocolTest, WireConformance_RegAck_Shape)
{
    using namespace std::chrono;
    using namespace pylabhub::tests::wire;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "tr1.regack" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_regack_shape");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    nlohmann::json reg;
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid, &reg));

    // HEP-CORE-0023 §2.5.1 — REG_ACK carries `status` + the `heartbeat`
    // block.  Pin the spec-named keys; reject the band-family `band` key.
    expect_object_has_keys(reg, {"status", "heartbeat"},
                            "REG_ACK", "HEP-CORE-0023 §2.5.1");
    expect_string_field(reg, "status", "success",
                         "REG_ACK", "HEP-CORE-0023 §2.5.1");
    expect_object_lacks_keys(reg, {"band"}, "REG_ACK",
                              "HEP-CORE-0023 §2.5.1 (band family is separate "
                              "per HEP-CORE-0030 §5.1)");

    const auto &hb = reg["heartbeat"];
    expect_object_has_keys(hb,
        {"heartbeat_interval_ms", "ready_miss_heartbeats",
         "pending_miss_heartbeats"},
        "REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");
    expect_int_field(hb, "heartbeat_interval_ms", "REG_ACK.heartbeat",
                      "HEP-CORE-0023 §2.5.1");
    expect_int_field(hb, "ready_miss_heartbeats", "REG_ACK.heartbeat",
                      "HEP-CORE-0023 §2.5.1");
    expect_int_field(hb, "pending_miss_heartbeats", "REG_ACK.heartbeat",
                      "HEP-CORE-0023 §2.5.1");

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, WireConformance_ConsumerRegAck_Shape)
{
    using namespace std::chrono;
    using namespace pylabhub::tests::wire;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "tr1.creg" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_creg_shape");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto           cons = make_wire_client(ctx, setup, cons_uid);
    nlohmann::json reg;
    ASSERT_NO_FATAL_FAILURE(
        register_consumer(cons, setup, channel, cons_uid, &reg));

    expect_object_has_keys(reg, {"status", "heartbeat"},
                            "CONSUMER_REG_ACK", "HEP-CORE-0023 §2.5.1");
    expect_string_field(reg, "status", "success",
                         "CONSUMER_REG_ACK", "HEP-CORE-0023 §2.5.1");
    expect_object_lacks_keys(reg, {"band"}, "CONSUMER_REG_ACK",
                              "HEP-CORE-0023 §2.5.1");

    const auto &hb = reg["heartbeat"];
    expect_object_has_keys(hb,
        {"heartbeat_interval_ms", "ready_miss_heartbeats",
         "pending_miss_heartbeats"},
        "CONSUMER_REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, WireConformance_RoleInfoAck_Shape)
{
    using namespace std::chrono;
    using namespace pylabhub::tests::wire;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "tr1.roleinfo" + suffix;
    const std::string uid     = "prod." + channel;
    const std::string querier = "tr1.querier.uid0000001" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_roleinfo_shape");
    const auto     setup    = make_pattern4_setup({uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));

    auto q = make_wire_client(ctx, setup, querier);

    // ── Case 1: target uid found (no inbox configured) ──
    nlohmann::json req;
    req["role_uid"] = uid;
    auto info = q.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(info.has_value())
        << "ROLE_INFO_REQ timed out for a registered uid";

    // HEP-CORE-0027 §4.2 — ROLE_INFO_ACK for a found role carries
    // `found`, `channel`, and inbox metadata (empty when no inbox).
    expect_object_has_keys(*info,
        {"found", "channel", "inbox_endpoint", "inbox_packing",
         "inbox_checksum", "inbox_schema"},
        "ROLE_INFO_ACK", "HEP-CORE-0027 §4.2");
    // Code returns a parsed `inbox_schema` object, not the stringified
    // `inbox_schema_json` some HEPs still mention.  Pin the code contract.
    expect_object_lacks_keys(*info, {"inbox_schema_json"}, "ROLE_INFO_ACK",
                              "HEP-CORE-0027 §4.2 (code returns parsed object; "
                              "any `inbox_schema_json` reference is stale)");

    // ── Case 2: unknown uid → found=false ──
    nlohmann::json req2;
    req2["role_uid"] = "prod.no.such.role.uid00000000";
    auto not_found = q.request("ROLE_INFO_REQ", req2, "ROLE_INFO_ACK",
                                milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(not_found.has_value())
        << "ROLE_INFO_REQ for unknown uid should still get an ACK "
           "(found=false), not time out";
    expect_object_has_keys(*not_found, {"found"},
                            "ROLE_INFO_ACK (unknown uid)", "HEP-CORE-0027 §4.2");
    ASSERT_TRUE(not_found->at("found").is_boolean());
    EXPECT_FALSE(not_found->at("found").get<bool>())
        << "ROLE_INFO_ACK.found must be false for an unknown uid";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, WireConformance_BandAck_Shapes)
{
    using namespace std::chrono;
    using namespace pylabhub::tests::wire;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "tr1.bandshape" + suffix;
    const std::string uid     = "prod." + channel;
    const std::string band    = "!tr1.band" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bandshape");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    // Band join/leave/members need only a valid role_uid on the wire —
    // no prior REG (per the CorrIdEcho migration).  client_role_uid ==
    // body role_uid so the BAND_JOIN identity_match gate passes.
    auto client = make_wire_client(ctx, setup, uid);

    // ── BAND_JOIN_ACK ──
    nlohmann::json join_req;
    join_req["band"]      = band;
    join_req["role_uid"]  = uid;
    join_req["role_name"] = "test_band_member";
    auto join_ack = client.request("BAND_JOIN_REQ", join_req, "BAND_JOIN_ACK",
                                    milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(join_ack.has_value()) << "BAND_JOIN_REQ timed out";
    expect_object_has_keys(*join_ack, {"status", "band", "members"},
                            "BAND_JOIN_ACK", "HEP-CORE-0030 §5.1");
    expect_string_field(*join_ack, "status", "success",
                         "BAND_JOIN_ACK", "HEP-CORE-0030 §5.1");
    expect_object_lacks_keys(*join_ack, {"channel"}, "BAND_JOIN_ACK",
                              "HEP-CORE-0030 §5.1 (audit B1 — wire key is `band`)");
    ASSERT_TRUE(join_ack->at("band").is_string());
    EXPECT_EQ(join_ack->at("band").get<std::string>(), band);
    ASSERT_TRUE(join_ack->at("members").is_array());

    // ── BAND_MEMBERS_ACK ──
    nlohmann::json members_req;
    members_req["band"] = band;
    auto members_ack = client.request("BAND_MEMBERS_REQ", members_req,
                                       "BAND_MEMBERS_ACK",
                                       milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(members_ack.has_value()) << "BAND_MEMBERS_REQ timed out";
    expect_object_has_keys(*members_ack, {"band", "members"},
                            "BAND_MEMBERS_ACK", "HEP-CORE-0030 §5.1");
    expect_object_lacks_keys(*members_ack, {"channel"}, "BAND_MEMBERS_ACK",
                              "HEP-CORE-0030 §5.1 (audit B1 — wire key is `band`)");

    // ── BAND_LEAVE_ACK ──
    nlohmann::json leave_req;
    leave_req["band"]     = band;
    leave_req["role_uid"] = uid;
    auto leave_ack = client.request("BAND_LEAVE_REQ", leave_req,
                                     "BAND_LEAVE_ACK",
                                     milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(leave_ack.has_value()) << "BAND_LEAVE_REQ timed out";
    expect_object_has_keys(*leave_ack, {"status"},
                            "BAND_LEAVE_ACK", "HEP-CORE-0030 §5.1");
    expect_string_field(*leave_ack, "status", "success",
                         "BAND_LEAVE_ACK", "HEP-CORE-0030 §5.1");

    broker.signal_quit();
}
