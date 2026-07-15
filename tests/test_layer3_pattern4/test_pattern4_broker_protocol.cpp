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
#include "utils/security/shm_capability_channel.hpp"
#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

    /// Pure REG_REQ body builder (no wire I/O, no assertions).  Lets
    /// error-path tests inspect the reply and success-path tests add
    /// extra body fields (schema_hash, consumer_queue_type, …).  Pubkey
    /// comes from the shared curve setup; `shm=true` selects the SHM
    /// capability transport, else ZMQ on an unused port.
    nlohmann::json producer_reg_body(
        const pylabhub::tests::pattern4::Pattern4Setup &setup,
        const std::string                              &channel,
        const std::string                              &uid,
        bool                                            shm,
        const std::string                              &topology = {})
    {
        namespace sec = pylabhub::utils::security;
        pylabhub::hub::ProducerRegInputs in;
        in.channel          = channel;
        in.role_uid         = uid;
        in.role_name        = "test_producer";
        in.role_type        = "producer";
        in.channel_topology = topology;
        in.zmq_pubkey       = setup.curve.role(uid).public_z85;
        if (shm)
        {
            in.has_shm                  = true;
            in.shm_capability_endpoint  =
                sec::default_shm_capability_endpoint(channel);
        }
        else
        {
            in.is_zmq_transport  = true;
            in.zmq_node_endpoint =
                "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
        }
        return pylabhub::hub::build_producer_reg_payload(in);
    }

    /// Pure CONSUMER_REG_REQ body builder.  data_transport="zmq" mirrors
    /// the make_cons_opts default; transport arbitration keys off the
    /// separate `consumer_queue_type` field, which callers add when a
    /// transport-match test needs it.
    nlohmann::json consumer_reg_body(
        const pylabhub::tests::pattern4::Pattern4Setup &setup,
        const std::string                              &channel,
        const std::string                              &uid,
        const std::string                              &topology = {})
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel          = channel;
        in.role_uid         = uid;
        in.role_name        = "test_consumer";
        in.role_type        = "consumer";
        in.data_transport   = "zmq";
        in.channel_topology = topology;
        in.zmq_pubkey       = setup.curve.role(uid).public_z85;
        return pylabhub::hub::build_consumer_reg_payload(in);
    }

    /// Drain unsolicited frames from `client` until one of msg_type
    /// `want` arrives (returns its body) or the budget elapses (returns
    /// nullopt).  Non-matching frames (e.g. an interleaved
    /// CHANNEL_AUTH_CHANGED_NOTIFY) are discarded.  Used to observe
    /// broker-pushed NOTIFYs on a registered member's DEALER without a
    /// request/reply pairing.
    std::optional<nlohmann::json> drain_for(
        BrokerWireClient          &client,
        std::string_view           want,
        std::chrono::milliseconds  budget)
    {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + budget;
        while (true)
        {
            const auto now = clock::now();
            if (now >= deadline) return std::nullopt;
            auto frame = client.receive(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now));
            if (!frame) return std::nullopt;
            if (frame->first == want) return frame->second;
        }
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
        nlohmann::json                                 *out_ack = nullptr,
        const std::string                              &topology = {})
    {
        auto reply = client.request(
            "REG_REQ",
            producer_reg_body(setup, channel, uid, /*shm=*/false, topology),
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
        nlohmann::json                                 *out_ack = nullptr,
        const std::string                              &topology = {})
    {
        auto reply = client.request(
            "CONSUMER_REG_REQ", consumer_reg_body(setup, channel, uid, topology),
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

// ─── Duplicate REG_REQ — SHM cardinality + schema-hash conflict ────────────
//
// Two distinct producers race for one channel.  Migrated from the
// retired in-process harness (task #54 Round 1).

TEST_F(Pattern4BrokerProtocolTest,
       DuplicateReg_TwoDistinctProducers_OnShmChannel_RejectedOneToOneCardinality)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.dup.same" + suffix;
    const std::string uid1    = "prod.dup.same.uid1" + suffix;
    const std::string uid2    = "prod.dup.same.uid2" + suffix;
    const std::string hash(64, 'a');

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_dup_card");
    const auto     setup    = make_pattern4_setup({uid1, uid2});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           p1 = make_wire_client(ctx, setup, uid1);
    auto           b1 = producer_reg_body(setup, channel, uid1, /*shm=*/true);
    b1["schema_hash"] = hash;
    auto r1 = p1.request("REG_REQ", b1, "REG_ACK",
                          milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r1.has_value()) << "first REG_REQ timed out";
    ASSERT_EQ(r1->value("status", std::string{}), "success")
        << "first SHM producer should register; body=" << r1->dump();

    auto p2 = make_wire_client(ctx, setup, uid2);
    auto b2 = producer_reg_body(setup, channel, uid2, /*shm=*/true);
    b2["schema_hash"] = hash;
    auto r2 = p2.request("REG_REQ", b2, "REG_ACK",
                          milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r2.has_value())
        << "broker must return a structured error, not transport failure";
    EXPECT_EQ(r2->value("status", std::string{}), "error")
        << "second SHM producer must reject; body=" << r2->dump();
    // A default (undeclared) SHM channel stores topology `one-to-one`;
    // the second producer trips ONE_TO_ONE_CARDINALITY_VIOLATED (was
    // MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM pre-topology-migration).
    EXPECT_EQ(r2->value("error_code", std::string{}),
              "ONE_TO_ONE_CARDINALITY_VIOLATED")
        << "body=" << r2->dump();
    // Broker-side path pin: the WARN emitted for the rejected second
    // producer on a default one-to-one SHM channel.
    expect_log(broker,
                "event=RegReqRejected reason='ONE_TO_ONE_CARDINALITY_VIOLATED'",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, DuplicateReg_DifferentSchemaHash_Rejected)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.dup.diff" + suffix;
    const std::string uid1    = "prod.dup.diff.uid1" + suffix;
    const std::string uid2    = "prod.dup.diff.uid2" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_dup_schema");
    const auto     setup    = make_pattern4_setup({uid1, uid2});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           p1 = make_wire_client(ctx, setup, uid1);
    auto           b1 = producer_reg_body(setup, channel, uid1, /*shm=*/true);
    b1["schema_hash"] = std::string(64, 'a');
    auto r1 = p1.request("REG_REQ", b1, "REG_ACK",
                          milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r1.has_value()) << "first REG_REQ timed out";
    ASSERT_EQ(r1->value("status", std::string{}), "success");

    auto p2 = make_wire_client(ctx, setup, uid2);
    auto b2 = producer_reg_body(setup, channel, uid2, /*shm=*/true);
    b2["schema_hash"] = std::string(64, 'b');
    auto r2 = p2.request("REG_REQ", b2, "REG_ACK",
                          milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r2.has_value())
        << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(r2->value("status", std::string{}), "error");
    EXPECT_EQ(r2->value("error_code", std::string{}), "SCHEMA_MISMATCH")
        << "body=" << r2->dump();

    broker.signal_quit();
}

// ─── Transport arbitration (producer transport vs consumer_queue_type) ─────

TEST_F(Pattern4BrokerProtocolTest, TransportMismatch_ShmProducer_ZmqConsumer_Fails)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "proto.transport.shm_zmq" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_tx_mismatch");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    auto pr = prod.request("REG_REQ",
                            producer_reg_body(setup, channel, prod_uid, true),
                            "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(pr.has_value()) << "producer REG_REQ timed out";
    ASSERT_EQ(pr->value("status", std::string{}), "success");
    // R6 producer-kLive gate must clear before the broker reaches the
    // transport-arbitration check that is this test's subject.
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons  = make_wire_client(ctx, setup, cons_uid);
    auto cbody = consumer_reg_body(setup, channel, cons_uid);
    cbody["consumer_queue_type"] = "zmq";
    auto cr = cons.request("CONSUMER_REG_REQ", cbody, "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value())
        << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(cr->value("status", std::string{}), "error");
    EXPECT_EQ(cr->value("error_code", std::string{}), "TRANSPORT_MISMATCH")
        << "body=" << cr->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, TransportMatch_ShmConsumer_ShmProducer_Succeeds)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "proto.transport.shm_shm" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_tx_shm");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    auto pr = prod.request("REG_REQ",
                            producer_reg_body(setup, channel, prod_uid, true),
                            "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(pr.has_value()) << "producer REG_REQ timed out";
    ASSERT_EQ(pr->value("status", std::string{}), "success");
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons  = make_wire_client(ctx, setup, cons_uid);
    auto cbody = consumer_reg_body(setup, channel, cons_uid);
    cbody["consumer_queue_type"] = "shm";
    auto cr = cons.request("CONSUMER_REG_REQ", cbody, "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value()) << "CONSUMER_REG_REQ timed out";
    EXPECT_EQ(cr->value("status", std::string{}), "success")
        << "both sides use SHM — should succeed; body=" << cr->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, TransportMatch_NoDriverField_AlwaysSucceeds)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "proto.transport.nofield" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_tx_nofield");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    auto pr = prod.request("REG_REQ",
                            producer_reg_body(setup, channel, prod_uid, true),
                            "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(pr.has_value()) << "producer REG_REQ timed out";
    ASSERT_EQ(pr->value("status", std::string{}), "success");
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    // No consumer_queue_type field — broker skips transport arbitration.
    auto cr = cons.request("CONSUMER_REG_REQ",
                            consumer_reg_body(setup, channel, cons_uid),
                            "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value()) << "CONSUMER_REG_REQ timed out";
    EXPECT_EQ(cr->value("status", std::string{}), "success")
        << "omitted consumer_queue_type must succeed; body=" << cr->dump();

    broker.signal_quit();
}

// ─── REG_ACK / CONSUMER_REG_ACK heartbeat-negotiation block ────────────────

TEST_F(Pattern4BrokerProtocolTest, RegAck_ContainsHeartbeatBlock_Defaults)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.regack.hb_default" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_hb_default");
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

    ASSERT_TRUE(reg.contains("heartbeat")) << "REG_ACK missing heartbeat block";
    const auto &hb = reg["heartbeat"];
    ASSERT_TRUE(hb.is_object());
    EXPECT_EQ(hb.value("heartbeat_interval_ms", -1),
              pylabhub::kDefaultHeartbeatIntervalMs);
    EXPECT_EQ(hb.value("ready_miss_heartbeats", std::uint32_t{0}),
              pylabhub::kDefaultReadyMissHeartbeats);
    EXPECT_EQ(hb.value("pending_miss_heartbeats", std::uint32_t{0}),
              pylabhub::kDefaultPendingMissHeartbeats);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RegAck_HeartbeatBlock_HonorsCustomConfig)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.regack.hb_custom" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_hb_custom");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    // "hb_custom" profile: heartbeat_interval=250ms, ready_miss=12,
    // pending_miss=8 — the REG_ACK block must echo these.
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "hb_custom"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    nlohmann::json reg;
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid, &reg));

    ASSERT_TRUE(reg.contains("heartbeat"));
    const auto &hb = reg["heartbeat"];
    EXPECT_EQ(hb.value("heartbeat_interval_ms", -1), 250);
    EXPECT_EQ(hb.value("ready_miss_heartbeats", std::uint32_t{0}), 12u);
    EXPECT_EQ(hb.value("pending_miss_heartbeats", std::uint32_t{0}), 8u);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, ConsumerRegAck_ContainsHeartbeatBlock)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "proto.cons_regack.hb" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_cons_hb");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto           cons = make_wire_client(ctx, setup, cons_uid);
    nlohmann::json cons_reg;
    ASSERT_NO_FATAL_FAILURE(
        register_consumer(cons, setup, channel, cons_uid, &cons_reg));

    ASSERT_TRUE(cons_reg.contains("heartbeat"))
        << "CONSUMER_REG_ACK missing heartbeat block";
    const auto &hb = cons_reg["heartbeat"];
    EXPECT_TRUE(hb.contains("heartbeat_interval_ms"));
    EXPECT_TRUE(hb.contains("ready_miss_heartbeats"));
    EXPECT_TRUE(hb.contains("pending_miss_heartbeats"));

    broker.signal_quit();
}

// ─── CHECKSUM_ERROR_REPORT → CHANNEL_EVENT_NOTIFY forward ──────────────────
//
// Broker profile `checksum_notify` (ChecksumRepairPolicy::NotifyOnly):
// a reporter's CHECKSUM_ERROR_REPORT is forwarded to the channel's
// producer as an unsolicited CHANNEL_EVENT_NOTIFY (HEP-CORE-0019 Cat2).

TEST_F(Pattern4BrokerProtocolTest, ChecksumErrorReport_ForwardedToProducer)
{
    using namespace std::chrono;
    const std::string suffix       = ".pid" + std::to_string(::getpid());
    const std::string channel      = "proto.checksum.prod" + suffix;
    const std::string prod_uid     = "prod." + channel;
    const std::string reporter_uid = "reporter" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_checksum_fwd");
    const auto     setup    = make_pattern4_setup({prod_uid, reporter_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "checksum_notify"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));

    auto           reporter = make_wire_client(ctx, setup, reporter_uid);
    nlohmann::json report;
    report["channel_name"] = channel;
    report["slot_index"]   = 42;
    report["error"]        = "bad CRC in slot 42";
    report["reporter_pid"] = pylabhub::platform::get_pid();
    reporter.send("CHECKSUM_ERROR_REPORT", report);

    auto notify = drain_for(prod, "CHANNEL_EVENT_NOTIFY",
                             milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(notify.has_value())
        << "producer did not receive the forwarded checksum-error NOTIFY";
    EXPECT_EQ(notify->value("channel_name", std::string{}), channel)
        << "forwarded NOTIFY body=" << notify->dump();

    broker.signal_quit();
}

// ─── CHANNEL_BROADCAST_SEND_NOTIFY → fan-out CHANNEL_BROADCAST_DELIVER_NOTIFY ─
//
// A sender's broadcast fans out to the channel's producer + ALL
// consumers, and NOT to a non-member sender (HEP-CORE-0030 broadcast
// semantics).  Members are observed via a parent-side NOTIFY drain.

TEST_F(Pattern4BrokerProtocolTest, BroadcastFanOut_DeliveredToProducerAndAllConsumers)
{
    using namespace std::chrono;
    const std::string suffix    = ".pid" + std::to_string(::getpid());
    const std::string channel   = "proto.bcast.fanout" + suffix;
    const std::string prod_uid  = "prod." + channel;
    const std::string cons1_uid = "cons.first." + channel;
    const std::string cons2_uid = "cons.second." + channel;
    const std::string send_uid  = "prod.broadcast.sender" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_fanout");
    const auto     setup =
        make_pattern4_setup({prod_uid, cons1_uid, cons2_uid, send_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    // Fan-out — 1 producer, 2 consumers.  Explicit topology required
    // (default one-to-one would trip ONE_TO_ONE_CARDINALITY_VIOLATED on
    // the second consumer).
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer(prod, setup, channel, prod_uid, nullptr, "fan-out"));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons1 = make_wire_client(ctx, setup, cons1_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_consumer(cons1, setup, channel, cons1_uid, nullptr, "fan-out"));
    auto cons2 = make_wire_client(ctx, setup, cons2_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_consumer(cons2, setup, channel, cons2_uid, nullptr, "fan-out"));

    auto           sender = make_wire_client(ctx, setup, send_uid);
    nlohmann::json bcast;
    bcast["target_channel"] = channel;
    bcast["sender_uid"]     = send_uid;
    bcast["message"]        = "hello-fan-out";
    bcast["data"]           = "";
    sender.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    auto check = [&](BrokerWireClient &c, const char *who) {
        auto n = drain_for(c, "CHANNEL_BROADCAST_DELIVER_NOTIFY",
                            milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(n.has_value())
            << who << " did not receive CHANNEL_BROADCAST_DELIVER_NOTIFY";
        EXPECT_EQ(n->value("channel_name", std::string{}), channel) << who;
        EXPECT_EQ(n->value("event", std::string{}), "broadcast") << who;
        EXPECT_EQ(n->value("sender_uid", std::string{}), send_uid) << who;
        EXPECT_EQ(n->value("message", std::string{}), "hello-fan-out") << who;
    };
    ASSERT_NO_FATAL_FAILURE(check(prod, "producer"));
    ASSERT_NO_FATAL_FAILURE(check(cons1, "cons1"));
    ASSERT_NO_FATAL_FAILURE(check(cons2, "cons2"));

    // The external non-member sender must NOT receive the fan-out.
    auto leaked = drain_for(sender, "CHANNEL_BROADCAST_DELIVER_NOTIFY",
                             milliseconds{300});
    EXPECT_FALSE(leaked.has_value())
        << "non-member sender unexpectedly received the broadcast NOTIFY";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, BroadcastFanOut_DataPayloadRoundTrip)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "proto.bcast.payload" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string send_uid = "ext.bcast.payload" + suffix;
    const std::string msg      = "payload-test";
    const std::string data     = R"({"k":"v","n":42,"arr":[1,2,3]})";

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_data");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid, send_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto           sender = make_wire_client(ctx, setup, send_uid);
    nlohmann::json bcast;
    bcast["target_channel"] = channel;
    bcast["sender_uid"]     = send_uid;
    bcast["message"]        = msg;
    bcast["data"]           = data;
    sender.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    auto n = drain_for(cons, "CHANNEL_BROADCAST_DELIVER_NOTIFY",
                        milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(n.has_value())
        << "consumer did not receive broadcast NOTIFY with data payload";
    EXPECT_EQ(n->value("channel_name", std::string{}), channel);
    EXPECT_EQ(n->value("event", std::string{}), "broadcast");
    EXPECT_EQ(n->value("sender_uid", std::string{}), send_uid);
    EXPECT_EQ(n->value("message", std::string{}), msg);
    EXPECT_EQ(n->value("data", std::string{}), data)
        << "data payload was modified in transit";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, BroadcastUnknownChannel_NoNotifyDelivered)
{
    using namespace std::chrono;
    const std::string suffix    = ".pid" + std::to_string(::getpid());
    const std::string other_ch  = "proto.bcast.other" + suffix;
    const std::string other_prd = "prod." + other_ch;
    const std::string spec_uid  = "cons." + other_ch;
    const std::string unknown   = "proto.bcast.unknown" + suffix;
    const std::string send_uid  = "ext.bcast.unknown" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_unknown");
    const auto setup = make_pattern4_setup({other_prd, spec_uid, send_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           other_prod = make_wire_client(ctx, setup, other_prd);
    ASSERT_NO_FATAL_FAILURE(
        register_producer(other_prod, setup, other_ch, other_prd));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(other_prod, other_ch, other_prd));

    auto spec = make_wire_client(ctx, setup, spec_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(spec, setup, other_ch, spec_uid));

    auto           sender = make_wire_client(ctx, setup, send_uid);
    nlohmann::json bcast;
    bcast["target_channel"] = unknown;  // no such channel
    bcast["sender_uid"]     = send_uid;
    bcast["message"]        = "into-the-void";
    bcast["data"]           = "";
    sender.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    // The unrelated other-channel consumer must not receive the leak.
    auto leaked = drain_for(spec, "CHANNEL_BROADCAST_DELIVER_NOTIFY",
                             milliseconds{300});
    EXPECT_FALSE(leaked.has_value())
        << "broadcast for an unknown channel leaked to another channel's "
           "consumer";

    // Broker liveness after the unknown-channel broadcast — wire-observed
    // (a subsequent request still gets a reply).  This replaces the old
    // in-process query_channel_snapshot() liveness probe.
    nlohmann::json req;
    req["role_uid"] = other_prd;
    auto pres = sender.request("ROLE_PRESENCE_REQ", req, "ROLE_PRESENCE_ACK",
                                milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(pres.has_value())
        << "broker stopped servicing requests after unknown-channel broadcast";
    EXPECT_TRUE(pres->value("present", false))
        << "liveness probe: registered other-channel producer should be present";

    broker.signal_quit();
}
