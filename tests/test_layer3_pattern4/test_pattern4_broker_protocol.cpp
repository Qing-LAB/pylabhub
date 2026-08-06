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
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"
#include "shared_test_helpers.h"
#include "wire_conformance.h"

#include "plh_platform.hpp"
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

class Pattern4BrokerProtocolTest : public pylabhub::tests::pattern4::Pattern4WireTest
{
};

} // namespace

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
    const std::string uid = "prod." + channel;
    const std::string role_nm = channel;
    const std::string band = "!tr1.bcorr.pid" + std::to_string(::getpid());

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_band_corrid");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint", milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &role_kp = setup.curve.role(uid);

    BrokerWireClient::Config cfg;
    cfg.broker_endpoint = setup.broker_endpoint;
    cfg.broker_pubkey = setup.curve.hub.public_z85;
    cfg.client_pubkey = role_kp.public_z85;
    cfg.client_seckey = role_kp.secret_z85;
    cfg.client_role_uid = uid;

    BrokerWireClient client(ctx, cfg);

    // ── Case 1: BAND_JOIN success echoes corr_id ─────────────
    const std::string join_corr = "test.band.join.corr.001";
    nlohmann::json join_req;
    join_req["band"] = band;
    join_req["role_uid"] = uid;
    join_req["role_name"] = role_nm;
    join_req["correlation_id"] = join_corr;
    auto join_resp =
        client.request("BAND_JOIN_REQ", join_req, "BAND_JOIN_ACK", milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(join_resp.has_value()) << "BAND_JOIN_REQ timed out";
    ASSERT_EQ(join_resp->value("status", std::string{}), "success")
        << "BAND_JOIN_REQ failed; body=" << join_resp->dump();
    ASSERT_TRUE(join_resp->contains("correlation_id"))
        << "BAND_JOIN_ACK missing correlation_id field "
           "(broker_proto 5 contract; broker_service.cpp B1 fix); "
           "body="
        << join_resp->dump();
    EXPECT_EQ(join_resp->at("correlation_id").get<std::string>(), join_corr)
        << "BAND_JOIN_ACK echoed wrong correlation_id";

    // ── Case 2: BAND_LEAVE success echoes corr_id ────────────
    const std::string leave_corr = "test.band.leave.corr.002";
    nlohmann::json leave_req;
    leave_req["band"] = band;
    leave_req["role_uid"] = uid;
    leave_req["correlation_id"] = leave_corr;
    auto leave_resp =
        client.request("BAND_LEAVE_REQ", leave_req, "BAND_LEAVE_ACK", milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(leave_resp.has_value()) << "BAND_LEAVE_REQ timed out";
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
    leave_req_err["band"] = band;
    leave_req_err["role_uid"] = uid;
    leave_req_err["correlation_id"] = err_corr;
    auto err_resp = client.request("BAND_LEAVE_REQ", leave_req_err, "BAND_LEAVE_ACK",
                                   milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(err_resp.has_value()) << "BAND_LEAVE_REQ (error path) timed out";
    ASSERT_EQ(err_resp->value("status", std::string{}), "error");
    EXPECT_EQ(err_resp->value("error_code", std::string{}), "NOT_A_MEMBER");
    ASSERT_TRUE(err_resp->contains("correlation_id"))
        << "BAND_LEAVE error reply missing correlation_id; body=" << err_resp->dump();
    EXPECT_EQ(err_resp->at("correlation_id").get<std::string>(), err_corr)
        << "BAND_LEAVE error reply echoed wrong correlation_id";

    // ── Case 4: Frame 3 correlation_id is authoritative ────────
    // When the request omits body-level correlation_id, BrokerWireClient
    // still stamps Frame 3 per HEP-CORE-0046 §14 and the broker's
    // dispatch layer injects it into the ACK body so tests inspecting
    // body["correlation_id"] see the authoritative Frame 3 value.
    nlohmann::json rejoin_req;
    rejoin_req["band"] = band;
    rejoin_req["role_uid"] = uid;
    rejoin_req["role_name"] = role_nm;
    auto rejoin =
        client.request("BAND_JOIN_REQ", rejoin_req, "BAND_JOIN_ACK", milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(rejoin.has_value()) << "BAND_JOIN_REQ (no body correlation_id) timed out";
    ASSERT_EQ(rejoin->value("status", std::string{}), "success");
    ASSERT_TRUE(rejoin->contains("correlation_id"))
        << "BAND_JOIN_ACK must echo Frame 3 correlation_id per "
           "I-CORRELATION-STABLE regardless of request body content; body="
        << rejoin->dump();
    EXPECT_FALSE(rejoin->at("correlation_id").get<std::string>().empty())
        << "echoed correlation_id must be non-empty per §14";

    broker.signal_quit();
}

// ─── BAND membership cleanup on producer dereg ─────────────────────────────
//
// Two producers on distinct channels join the same band; when producer A
// voluntarily deregisters its channel, the broker's on_channel_closed hook
// removes A from the band (`_dispatch_role_disconnected_if_dead` path).
// Verified entirely over the wire (BAND_MEMBERS count drops 2→1).  Migrated
// from datahub_role_state.band_membership_cleaned_on_role_close (task #52,
// DirectBrokerHandle sweep) — the original inspected the same effect but
// co-hosted the broker in-process.
TEST_F(Pattern4BrokerProtocolTest, Band_MembershipCleanedOnProducerDereg)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string ch_a = "tr1.band_a" + suffix;
    const std::string ch_b = "tr1.band_b" + suffix;
    const std::string uid_a = "prod.band.a" + suffix;
    const std::string uid_b = "prod.band.b" + suffix;
    const std::string band = "!tr1.band" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_band_cleanup");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint", milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    auto b = make_wire_client(ctx, setup, uid_b);

    ASSERT_NO_FATAL_FAILURE(register_producer(a, setup, ch_a, uid_a));
    ASSERT_NO_FATAL_FAILURE(register_producer(b, setup, ch_b, uid_b));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(a, ch_a, uid_a));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(b, ch_b, uid_b));

    auto band_join = [&](BrokerWireClient &c, const std::string &uid)
    {
        nlohmann::json req;
        req["band"] = band;
        req["role_uid"] = uid;
        req["role_name"] = uid;
        return c.request("BAND_JOIN_REQ", req, "BAND_JOIN_ACK", milliseconds{kLongTimeoutMs});
    };
    {
        auto ja = band_join(a, uid_a);
        ASSERT_TRUE(ja.has_value() && ja->value("status", std::string{}) == "success")
            << "producer A band_join failed";
        auto jb = band_join(b, uid_b);
        ASSERT_TRUE(jb.has_value() && jb->value("status", std::string{}) == "success")
            << "producer B band_join failed";
    }

    auto band_member_count = [&](BrokerWireClient &c) -> std::optional<std::size_t>
    {
        nlohmann::json req;
        req["band"] = band;
        auto r =
            c.request("BAND_MEMBERS_REQ", req, "BAND_MEMBERS_ACK", milliseconds{kLongTimeoutMs});
        if (!r || !r->contains("members"))
            return std::nullopt;
        return (*r)["members"].size();
    };

    ASSERT_EQ(band_member_count(b), std::optional<std::size_t>{2u})
        << "Expected 2 band members after both joins";

    // Producer A voluntarily deregisters its channel → on_channel_closed
    // fires → cleanup hook removes A from the band.
    {
        nlohmann::json dr;
        dr["channel_name"] = ch_a;
        dr["role_uid"] = uid_a;
        dr["producer_pid"] = static_cast<std::uint64_t>(pylabhub::platform::get_pid());
        auto resp = a.request("DEREG_REQ", dr, "DEREG_ACK", milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(resp.has_value()) << "DEREG_REQ timed out";
        EXPECT_EQ(resp->value("status", std::string{}), "success");
    }

    // Poll BAND_MEMBERS until the async cleanup lands (count drops to 1).
    bool cleaned = false;
    const auto deadline = steady_clock::now() + seconds{3};
    while (steady_clock::now() < deadline)
    {
        if (band_member_count(b) == std::optional<std::size_t>{1u})
        {
            cleaned = true;
            break;
        }
        std::this_thread::sleep_for(milliseconds{100});
    }
    ASSERT_TRUE(cleaned) << "Band membership was not cleaned up after producer A dereg";

    // Confirm the survivor is B.
    nlohmann::json mreq;
    mreq["band"] = band;
    auto members =
        b.request("BAND_MEMBERS_REQ", mreq, "BAND_MEMBERS_ACK", milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(members.has_value() && members->contains("members"));
    bool has_b = false;
    for (const auto &m : (*members)["members"])
        if (m.value("role_uid", std::string{}) == uid_b)
            has_b = true;
    EXPECT_TRUE(has_b) << "Remaining band member should be uid_b";

    broker.signal_quit();
}

// ─── BAND invalid-identifier rejection (HEP-CORE-0030 §3) ──────────────────
//
// A non-empty band name missing the `!` prefix is invalid.  JOIN / LEAVE /
// MEMBERS must all return status=error + error_code=INVALID_BAND_NAME (the
// R3.5 regression: the broker once silently bumped invalid_identifier_total
// and returned success).  A valid `!`-prefixed name still succeeds.  Migrated
// from datahub_role_state.broker_band_rejects_invalid_identifier (task #52).
TEST_F(Pattern4BrokerProtocolTest, Band_RejectsInvalidIdentifier)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "prod.r35" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_band_invalid");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint", milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    auto client = make_wire_client(ctx, setup, uid);

    const std::string invalid = "no_bang_prefix"; // non-empty, missing `!`

    {
        nlohmann::json req;
        req["band"] = invalid;
        req["role_uid"] = uid;
        req["role_name"] = uid;
        auto resp =
            client.request("BAND_JOIN_REQ", req, "BAND_JOIN_ACK", milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(resp.has_value()) << "broker must respond, not time out";
        EXPECT_EQ(resp->value("status", std::string{}), "error")
            << "invalid band JOIN must be rejected; body=" << resp->dump();
        EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_BAND_NAME");
    }
    {
        nlohmann::json req;
        req["band"] = invalid;
        req["role_uid"] = uid;
        auto resp =
            client.request("BAND_LEAVE_REQ", req, "BAND_LEAVE_ACK", milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->value("status", std::string{}), "error");
        EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_BAND_NAME");
    }
    {
        nlohmann::json req;
        req["band"] = invalid;
        auto resp = client.request("BAND_MEMBERS_REQ", req, "BAND_MEMBERS_ACK",
                                   milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->value("status", std::string{}), "error");
        EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_BAND_NAME");
    }
    // Sanity: valid `!`-prefixed name still succeeds (happy path intact).
    {
        nlohmann::json req;
        req["band"] = "!r35.valid" + suffix;
        req["role_uid"] = uid;
        req["role_name"] = uid;
        auto resp =
            client.request("BAND_JOIN_REQ", req, "BAND_JOIN_ACK", milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->value("status", std::string{}), "success")
            << "valid `!`-prefixed band name must still succeed";
    }

    broker.signal_quit();
}

// ─── BAND join / leave / member-query lifecycle ────────────────────────────
//
// Two roles join a band: the JOIN ACK carries the growing member list, a
// MEMBERS query reflects it, and LEAVE shrinks it — all synchronous wire
// round-trips.  Migrated from datahub_channel_group.channel_join_leave
// (task #52 DirectBrokerHandle sweep); the original co-hosted the broker
// with two in-process BrokerRequestComm poll loops (ChannelClient).
TEST_F(Pattern4BrokerProtocolTest, Band_JoinLeaveMemberQuery)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid_a = "prod.role.a" + suffix;
    const std::string uid_b = "prod.role.b" + suffix;
    const std::string band = "!test_ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_band_joinleave");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint", milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    auto b = make_wire_client(ctx, setup, uid_b);

    auto band_join = [&](BrokerWireClient &c, const std::string &uid)
    {
        nlohmann::json req;
        req["band"] = band;
        req["role_uid"] = uid;
        req["role_name"] = uid;
        return c.request("BAND_JOIN_REQ", req, "BAND_JOIN_ACK", milliseconds{kLongTimeoutMs});
    };
    auto band_members = [&](BrokerWireClient &c)
    {
        nlohmann::json req;
        req["band"] = band;
        return c.request("BAND_MEMBERS_REQ", req, "BAND_MEMBERS_ACK", milliseconds{kLongTimeoutMs});
    };

    // A joins → sole member (JOIN ACK carries the member list).
    auto j1 = band_join(a, uid_a);
    ASSERT_TRUE(j1.has_value()) << "A BAND_JOIN timed out";
    EXPECT_EQ(j1->value("status", std::string{}), "success");
    ASSERT_TRUE(j1->contains("members"));
    EXPECT_EQ((*j1)["members"].size(), 1u) << "JOIN ACK should list A only";

    // B joins → both members visible.
    auto j2 = band_join(b, uid_b);
    ASSERT_TRUE(j2.has_value());
    ASSERT_TRUE(j2->contains("members"));
    EXPECT_EQ((*j2)["members"].size(), 2u) << "JOIN ACK should list A+B";

    // MEMBERS query agrees.
    auto m1 = band_members(a);
    ASSERT_TRUE(m1.has_value() && m1->contains("members"));
    EXPECT_EQ((*m1)["members"].size(), 2u);

    // A leaves.
    nlohmann::json lreq;
    lreq["band"] = band;
    lreq["role_uid"] = uid_a;
    auto leave = a.request("BAND_LEAVE_REQ", lreq, "BAND_LEAVE_ACK", milliseconds{kLongTimeoutMs});
    ASSERT_TRUE(leave.has_value());
    EXPECT_EQ(leave->value("status", std::string{}), "success");

    // MEMBERS query now shows exactly 1 — and it must be B, not A (identity,
    // not just cardinality: a broker that dropped the wrong member would keep
    // size 1 too).
    auto m2 = band_members(b);
    ASSERT_TRUE(m2.has_value() && m2->contains("members"));
    ASSERT_EQ((*m2)["members"].size(), 1u) << "B should be the sole member after A leaves";
    EXPECT_EQ((*m2)["members"][0].value("role_uid", std::string{}), uid_b)
        << "the surviving member must be B (A left)";

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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string querier = "QUERIER-unknown" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_pres_unknown");
    const auto setup = make_pattern4_setup({querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto client = make_wire_client(ctx, setup, querier);

    nlohmann::json req;
    req["role_uid"] = "prod.unknown.uiddeadbeef";
    auto resp = client.request("ROLE_PRESENCE_REQ", req, "ROLE_PRESENCE_ACK",
                               milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "broker should respond to ROLE_PRESENCE_REQ, not time out";
    EXPECT_FALSE(resp->value("present", true))
        << "unknown uid → present=false; body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RoleInfoReq_UnknownUid_NotFound)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string querier = "QUERIER-unknown2" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_info_unknown");
    const auto setup = make_pattern4_setup({querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto client = make_wire_client(ctx, setup, querier);

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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.presence.prod" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string querier = "QUERIER-pres-prod" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_pres_prod");
    const auto setup = make_pattern4_setup({prod_uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));

    auto q = make_wire_client(ctx, setup, querier);
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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.presence.cons" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons.prestest" + suffix;
    const std::string querier = "QUERIER-pres-cons" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_pres_cons");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    // R6 producer-kLive gate must clear before the consumer can register.
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto q = make_wire_client(ctx, setup, querier);
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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.roleinfo.withinbox" + suffix;
    const std::string uid = "prod." + channel;
    // Tagged `prod.` because the querier now REGISTERS: a hub discloses an
    // inbox only to a sender the target can admit, and only a registered
    // role is on any roster.  An untagged uid passes role_uid grammar but
    // fails the producer role-tag policy at REG_REQ.
    const std::string querier = "prod.roleinfo.querier" + suffix;
    const std::string inbox_ep = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
    // HEP-0027 §6 canonical object form — packing rides IN-OBJECT
    // (HEP-0046 B.2; the separate `inbox_packing` REG field is retired,
    // and the typed-body boundary rejects non-canonical shapes).
    const std::string schema_json =
        R"({"packing":"aligned","fields":[{"name":"v","type":"float64","count":1,"length":0}]})";
    const std::string packing = "aligned";

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_info_inbox");
    const auto setup = make_pattern4_setup({uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;

    // The querier registers FIRST.  A hub discloses an inbox only to a
    // sender the target can already admit (HEP-CORE-0035 §4.9.7), and the
    // roster the target confirms below is the one its own REG_ACK carried —
    // so the querier has to be in the ledger before that REG_ACK is built.
    // This is the ordinary "receiver starts after sender" case, which is
    // also the one that used to lose the first message.
    auto q = make_wire_client(ctx, setup, querier);
    ASSERT_NO_FATAL_FAILURE(register_producer(q, setup, channel + ".q", querier));

    auto prod = make_wire_client(ctx, setup, uid);

    // Register with the inbox advertised.  The base payload comes from
    // the production builder; the inbox_* fields are extra REG_REQ body
    // keys the broker records for ROLE_INFO_ACK (mirrors the old
    // `opts["inbox_*"]` registration).
    pylabhub::hub::ProducerRegInputs in;
    in.channel = channel;
    in.role_uid = uid;
    in.role_name = "InboxProd";
    in.role_type = "producer";
    in.is_zmq_transport = true;
    in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
    in.zmq_pubkey = setup.curve.role(uid).public_z85;
    auto payload = pylabhub::hub::build_producer_reg_payload(in);
    payload["inbox_endpoint"] = inbox_ep;
    payload["inbox_schema_json"] = schema_json;
    auto reg = prod.request("REG_REQ", payload, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg.has_value()) << "REG_REQ (with inbox) timed out";
    ASSERT_EQ(reg->value("status", std::string{}), "success")
        << "REG_REQ (with inbox) failed; body=" << reg->dump();

    // Before confirming, the target is registered and has an inbox but has
    // told the hub nothing about which roster it applied — so the address is
    // withheld and the reason says why.  Asserted rather than skipped: this
    // is the state that used to hand out coordinates to a peer that would
    // then be refused at the door.
    nlohmann::json req;
    req["role_uid"] = uid;
    auto early =
        q.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(early.has_value()) << "ROLE_INFO_REQ timed out before confirmation";
    EXPECT_FALSE(early->value("found", true)) << "body=" << early->dump();
    EXPECT_EQ(early->value("reason", std::string{}), "not_reachable_yet")
        << "body=" << early->dump();
    EXPECT_EQ(early->value("inbox_endpoint", std::string{}), "")
        << "the address must not travel before the target can admit the asker; body="
        << early->dump();

    // The target confirms the roster its REG_ACK carried — which names the
    // querier, because the querier registered first.
    ASSERT_NO_FATAL_FAILURE(confirm_roster(prod, uid, *reg));

    auto resp =
        q.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "ROLE_INFO_REQ timed out";
    EXPECT_TRUE(resp->value("found", false)) << "body=" << resp->dump();
    EXPECT_EQ(resp->value("inbox_endpoint", std::string{}), inbox_ep) << "body=" << resp->dump();
    EXPECT_EQ(resp->value("inbox_packing", std::string{}), packing) << "body=" << resp->dump();

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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "tr1.regack" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_regack_shape");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    nlohmann::json reg;
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid, &reg));

    // HEP-CORE-0023 §2.5.1 — REG_ACK carries `status` + the `heartbeat`
    // block.  Pin the spec-named keys; reject the band-family `band` key.
    expect_object_has_keys(reg, {"status", "heartbeat"}, "REG_ACK", "HEP-CORE-0023 §2.5.1");
    expect_string_field(reg, "status", "success", "REG_ACK", "HEP-CORE-0023 §2.5.1");
    expect_object_lacks_keys(reg, {"band"}, "REG_ACK",
                             "HEP-CORE-0023 §2.5.1 (band family is separate "
                             "per HEP-CORE-0030 §5.1)");

    const auto &hb = reg["heartbeat"];
    expect_object_has_keys(
        hb, {"heartbeat_interval_ms", "ready_miss_heartbeats", "pending_miss_heartbeats"},
        "REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");
    expect_int_field(hb, "heartbeat_interval_ms", "REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");
    expect_int_field(hb, "ready_miss_heartbeats", "REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");
    expect_int_field(hb, "pending_miss_heartbeats", "REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, WireConformance_ConsumerRegAck_Shape)
{
    using namespace std::chrono;
    using namespace pylabhub::tests::wire;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "tr1.creg" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_creg_shape");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    nlohmann::json reg;
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid, &reg));

    expect_object_has_keys(reg, {"status", "heartbeat"}, "CONSUMER_REG_ACK",
                           "HEP-CORE-0023 §2.5.1");
    expect_string_field(reg, "status", "success", "CONSUMER_REG_ACK", "HEP-CORE-0023 §2.5.1");
    expect_object_lacks_keys(reg, {"band"}, "CONSUMER_REG_ACK", "HEP-CORE-0023 §2.5.1");

    const auto &hb = reg["heartbeat"];
    expect_object_has_keys(
        hb, {"heartbeat_interval_ms", "ready_miss_heartbeats", "pending_miss_heartbeats"},
        "CONSUMER_REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");

    broker.signal_quit();
}

// ─── Roster replication (HEP-CORE-0035 §4.9) ──────────────────────────────

// The DECREASE case — unreachable before I-ROSTER-PRESENT, and the reason
// the version machinery existed without ever being exercised.
//
// While the roster was every configured role, it could only ever grow with
// the vault and never moved at runtime: no test could observe an entry
// leaving, so `revoke` and the monotonic guard were dead weight that still
// passed review.  Membership now follows registration, so a role that
// deregisters LEAVES every other role's list.
//
// Observed through successive REG_ACKs rather than a pushed notify: each
// registration answers with the roster as of that moment, which is the same
// evidence a role acts on and needs no unsolicited receive.
TEST_F(Pattern4BrokerProtocolTest, RosterShrinksWhenARoleDeregisters)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string ch_a = "roster.decrease.a" + suffix;
    const std::string ch_b = "roster.decrease.b" + suffix;
    const std::string ch_c = "roster.decrease.c" + suffix;
    const std::string uid_a = "prod." + ch_a;
    const std::string uid_b = "prod." + ch_b;
    const std::string uid_c = "prod." + ch_c;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_roster_decrease");
    const auto setup = make_pattern4_setup({uid_a, uid_b, uid_c});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;

    // Names in a roster block, as a set.  Order is NOT what is under test
    // here — membership is — and `local_role_roster()` already documents and
    // guarantees uid order for the benefit of wire captures, so a sequence
    // assertion in this test would duplicate a guarantee pinned elsewhere
    // while making the failure message about the wrong thing.
    const auto roster_uids = [](const nlohmann::json &ack)
    {
        std::set<std::string> out;
        for (const auto &e : ack.value("known_roles", nlohmann::json::array()))
            out.insert(e.value("uid", std::string{}));
        return out;
    };

    auto a = make_wire_client(ctx, setup, uid_a);
    nlohmann::json ack_a;
    ASSERT_NO_FATAL_FAILURE(register_producer(a, setup, ch_a, uid_a, &ack_a));
    EXPECT_EQ(roster_uids(ack_a), (std::set<std::string>{uid_a}))
        << "a hub that has one role registered must replicate exactly that one; body="
        << ack_a.dump();

    auto b = make_wire_client(ctx, setup, uid_b);
    nlohmann::json ack_b;
    ASSERT_NO_FATAL_FAILURE(register_producer(b, setup, ch_b, uid_b, &ack_b));
    EXPECT_EQ(roster_uids(ack_b), (std::set<std::string>{uid_a, uid_b}));
    const auto version_with_b = ack_b.value("known_roles_version", std::uint64_t{0});
    EXPECT_GT(version_with_b, ack_a.value("known_roles_version", std::uint64_t{0}))
        << "admitting a role must move the version";

    // B leaves.
    {
        nlohmann::json dereg;
        dereg["channel_name"] = ch_b;
        dereg["role_uid"] = uid_b;
        dereg["producer_pid"] = pylabhub::platform::get_pid();
        auto reply =
            b.request("DEREG_REQ", dereg, "DEREG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "DEREG_REQ timed out";
        ASSERT_EQ(reply->value("status", std::string{}), "success") << reply->dump();
    }

    // C registers, and its REG_ACK is the observation: B is gone from the
    // list, and the version is higher than the one that still contained it.
    auto c = make_wire_client(ctx, setup, uid_c);
    nlohmann::json ack_c;
    ASSERT_NO_FATAL_FAILURE(register_producer(c, setup, ch_c, uid_c, &ack_c));
    EXPECT_EQ(roster_uids(ack_c), (std::set<std::string>{uid_a, uid_c}))
        << "a deregistered role must leave every other role's list; body=" << ack_c.dump();
    EXPECT_GT(ack_c.value("known_roles_version", std::uint64_t{0}), version_with_b)
        << "a revocation must move the version, or holders would never learn of it";

    broker.signal_quit();
}

// A restarted role must EARN reachability again.
//
// The confirmation and the admission are separate halves of the ledger, and
// only the admission is erased when a role leaves.  Left alone, a role that
// stops and starts again carries a confirmation from its previous life — so
// the hub would judge it reachable and hand out its address while it has
// adopted no roster at all since restarting, and its own gate would refuse
// the sender the hub just waved through.  Exactly the failure the gate
// exists to prevent, arrived at from the other direction.
//
// Nothing about this is observable from outside the ledger, and a passing
// delivery test cannot distinguish it: the window is narrow and closes as
// soon as the restarted role adopts anything.  So it is pinned here.
TEST_F(Pattern4BrokerProtocolTest, RestartedRoleIsNotReachableOnAStaleConfirmation)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string ch_t = "roster.restart.target" + suffix;
    const std::string ch_s = "roster.restart.asker" + suffix;
    const std::string uid_t = "prod." + ch_t;
    const std::string uid_s = "prod." + ch_s;
    const std::string inbox_ep = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
    const std::string schema_json =
        R"({"packing":"aligned","fields":[{"name":"v","type":"float64","count":1,"length":0}]})";

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_roster_restart");
    const auto setup = make_pattern4_setup({uid_t, uid_s});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto s = make_wire_client(ctx, setup, uid_s);
    ASSERT_NO_FATAL_FAILURE(register_producer(s, setup, ch_s, uid_s));

    // Registering the target with its inbox advertised — done twice below, so
    // it is a lambda rather than two copies that could drift apart.
    auto t = make_wire_client(ctx, setup, uid_t);
    const auto register_target = [&]() -> nlohmann::json
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel = ch_t;
        in.role_uid = uid_t;
        in.role_name = "RestartTarget";
        in.role_type = "producer";
        in.is_zmq_transport = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
        in.zmq_pubkey = setup.curve.role(uid_t).public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        payload["inbox_endpoint"] = inbox_ep;
        payload["inbox_schema_json"] = schema_json;
        auto reply =
            t.request("REG_REQ", payload, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
        EXPECT_TRUE(reply.has_value()) << "REG_REQ timed out";
        return reply.value_or(nlohmann::json::object());
    };

    nlohmann::json req;
    req["role_uid"] = uid_t;
    const auto ask = [&]
    {
        auto r = s.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
        EXPECT_TRUE(r.has_value()) << "ROLE_INFO_REQ timed out";
        return r.value_or(nlohmann::json::object());
    };

    // ── First life: register, confirm, become reachable ──
    const auto ack1 = register_target();
    ASSERT_EQ(ack1.value("status", std::string{}), "success") << ack1.dump();
    ASSERT_NO_FATAL_FAILURE(confirm_roster(t, uid_t, ack1));
    {
        const auto info = ask();
        ASSERT_TRUE(info.value("found", false))
            << "baseline: a confirmed target must be reachable; body=" << info.dump();
    }

    // ── Stop ──
    {
        nlohmann::json dereg;
        dereg["channel_name"] = ch_t;
        dereg["role_uid"] = uid_t;
        dereg["producer_pid"] = pylabhub::platform::get_pid();
        auto reply =
            t.request("DEREG_REQ", dereg, "DEREG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "DEREG_REQ timed out";
        ASSERT_EQ(reply->value("status", std::string{}), "success") << reply->dump();
    }

    // ── Restart, and DO NOT confirm ──
    const auto ack2 = register_target();
    ASSERT_EQ(ack2.value("status", std::string{}), "success") << ack2.dump();

    {
        const auto info = ask();
        EXPECT_FALSE(info.value("found", true))
            << "a restarted role must not inherit its previous confirmation — it has adopted no "
               "roster since restarting, so its gate would refuse whoever the hub sent; body="
            << info.dump();
        EXPECT_EQ(info.value("reason", std::string{}), "not_reachable_yet") << info.dump();
        EXPECT_EQ(info.value("inbox_endpoint", std::string{}), "") << info.dump();
    }

    // ── Confirm again: reachability is earned, not remembered ──
    ASSERT_NO_FATAL_FAILURE(confirm_roster(t, uid_t, ack2));
    {
        const auto info = ask();
        EXPECT_TRUE(info.value("found", false))
            << "after confirming its new roster the target is reachable again; body="
            << info.dump();
    }

    broker.signal_quit();
}

// ─── The hub re-sends a roster the target may already hold ────────────────
//
// The gate pushes to an unconfirmed target on EVERY ask, because the hub
// does not know what that target holds — a confirmation is the only thing
// that tells it, and by definition it has not arrived.  So two asks about
// one unconfirmed target produce two identical pushes.
//
// That is the precondition the role-side rule exists for: a role that is
// offered a version it already holds must decline the CONTENT and still
// answer with its version (HEP-CORE-0035 §4.9.7).  Silence there would
// leave the hub refusing senders on the role's behalf until a tick
// happened to say otherwise.
//
// Pinned HERE rather than at L4 because here it is not a race.  A live
// role confirms the first push within microseconds, so at L4 the second
// ask lands after confirmation and the duplicate never occurs; forcing it
// would mean pinning timing.  A wire client simply never confirms, and the
// duplicate is then the only possible outcome — every step below is a
// reply on the connection that carried its request.
TEST_F(Pattern4BrokerProtocolTest, UnconfirmedTargetIsRePushedTheSameRoster)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.roster.repush" + suffix;
    const std::string uid = "prod." + channel;
    const std::string querier = "prod.roster.repush.q" + suffix;
    const std::string inbox_ep = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_roster_repush");
    const auto setup = make_pattern4_setup({uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;

    // Querier first, so the roster the target is offered names it — the
    // push is then a roster the target could act on, not an empty one.
    auto q = make_wire_client(ctx, setup, querier);
    ASSERT_NO_FATAL_FAILURE(register_producer(q, setup, channel + ".q", querier));

    auto target = make_wire_client(ctx, setup, uid);
    pylabhub::hub::ProducerRegInputs in;
    in.channel = channel;
    in.role_uid = uid;
    in.role_name = "RepushProd";
    in.role_type = "producer";
    in.is_zmq_transport = true;
    in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
    in.zmq_pubkey = setup.curve.role(uid).public_z85;
    auto payload = pylabhub::hub::build_producer_reg_payload(in);
    payload["inbox_endpoint"] = inbox_ep;
    payload["inbox_schema_json"] =
        R"({"packing":"aligned","fields":[{"name":"v","type":"float64","count":1,"length":0}]})";
    auto reg =
        target.request("REG_REQ", payload, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg.has_value()) << "REG_REQ (with inbox) timed out";
    ASSERT_EQ(reg->value("status", std::string{}), "success") << "body=" << reg->dump();

    const auto reg_version = reg->value("known_roles_version", std::uint64_t{0});
    ASSERT_GT(reg_version, 0U) << "a REG_ACK is built after admitting the role it answers, so it "
                                  "carries a real version; 0 means 'no roster' (I-ROSTER-VERSION); "
                                  "body="
                               << reg->dump();

    // The target NEVER confirms.  Nothing below waits on it, so there is
    // no window to lose.
    nlohmann::json req;
    req["role_uid"] = uid;

    const auto ask_and_collect_push = [&](const char *which) -> std::uint64_t
    {
        auto ack = q.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK",
                             milliseconds{pylabhub::kLongTimeoutMs});
        EXPECT_TRUE(ack.has_value()) << which << " ROLE_INFO_REQ timed out";
        if (!ack)
            return 0;
        EXPECT_EQ(ack->value("reason", std::string{}), "not_reachable_yet")
            << which << " ask; body=" << ack->dump();

        // The push is enqueued before the ACK is built, so by the time the
        // reply lands the notify is at worst in flight.  Waiting for it to
        // arrive is not ordering by time — the budget only bounds failure.
        auto push =
            drain_for(target, "ROSTER_UPDATE_NOTIFY", milliseconds{pylabhub::kLongTimeoutMs});
        EXPECT_TRUE(push.has_value())
            << which
            << " ask: the target was not sent a roster, so it has no way to become "
               "reachable and the asker's retry would never succeed";
        return push ? push->value("known_roles_version", std::uint64_t{0}) : 0;
    };

    const std::uint64_t first = ask_and_collect_push("first");
    const std::uint64_t second = ask_and_collect_push("second");

    EXPECT_EQ(first, reg_version)
        << "the roster pushed to an unconfirmed target is the one its own REG_ACK carried";
    EXPECT_EQ(second, first) << "nothing joined or left between the two asks, so the second push "
                                "repeats the first — this is the duplicate a role must decline "
                                "without going silent (HEP-CORE-0035 §4.9.7)";

    // Still withheld after both pushes: pushing is not disclosing.  Without
    // this a hub that pushed and then answered `found` would pass every
    // assertion above while handing out coordinates to a closed door.
    auto after =
        q.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->value("found", true)) << "body=" << after->dump();
    EXPECT_EQ(after->value("inbox_endpoint", std::string{}), "") << "body=" << after->dump();

    broker.signal_quit();
}

// The PERMANENT refusal.  A caller with no registration on this hub can
// never appear on any roster it issues, so "not yet" would be a lie and a
// retry loop would spin forever.  Distinguishing it costs one string and is
// the difference between a caller waiting usefully and waiting always.
TEST_F(Pattern4BrokerProtocolTest, InboxWithheldPermanentlyFromUnregisteredAsker)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "roster.stranger" + suffix;
    const std::string uid = "prod." + channel;
    const std::string stranger = "prod.roster.stranger.asker" + suffix;
    const std::string inbox_ep = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
    const std::string schema_json =
        R"({"packing":"aligned","fields":[{"name":"v","type":"float64","count":1,"length":0}]})";

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_roster_stranger");
    const auto setup = make_pattern4_setup({uid, stranger});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto target = make_wire_client(ctx, setup, uid);

    pylabhub::hub::ProducerRegInputs in;
    in.channel = channel;
    in.role_uid = uid;
    in.role_name = "StrangerTarget";
    in.role_type = "producer";
    in.is_zmq_transport = true;
    in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(pick_unused_port());
    in.zmq_pubkey = setup.curve.role(uid).public_z85;
    auto payload = pylabhub::hub::build_producer_reg_payload(in);
    payload["inbox_endpoint"] = inbox_ep;
    payload["inbox_schema_json"] = schema_json;
    auto reg =
        target.request("REG_REQ", payload, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg.has_value()) << "REG_REQ timed out";
    ASSERT_EQ(reg->value("status", std::string{}), "success") << reg->dump();

    // The target is fully converged — registered, inbox advertised, roster
    // confirmed.  Everything on its side is ready; the asker is the problem.
    ASSERT_NO_FATAL_FAILURE(confirm_roster(target, uid, *reg));

    // The stranger holds a vault key — it authenticates to the broker fine —
    // but has never registered, so it is on nobody's roster.
    auto s = make_wire_client(ctx, setup, stranger);
    nlohmann::json req;
    req["role_uid"] = uid;
    auto resp =
        s.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "ROLE_INFO_REQ timed out";
    EXPECT_FALSE(resp->value("found", true)) << resp->dump();
    EXPECT_EQ(resp->value("reason", std::string{}), "sender_not_registered")
        << "a caller that can never be admitted must be told so, not told to retry; body="
        << resp->dump();
    EXPECT_EQ(resp->value("inbox_endpoint", std::string{}), "")
        << "the address must not travel to a role no roster can name; body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, WireConformance_RoleInfoAck_Shape)
{
    using namespace std::chrono;
    using namespace pylabhub::tests::wire;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "tr1.roleinfo" + suffix;
    const std::string uid = "prod." + channel;
    const std::string querier = "tr1.querier.uid0000001" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_roleinfo_shape");
    const auto setup = make_pattern4_setup({uid, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));

    auto q = make_wire_client(ctx, setup, querier);

    // ── Case 1: target uid found (no inbox configured) ──
    nlohmann::json req;
    req["role_uid"] = uid;
    auto info =
        q.request("ROLE_INFO_REQ", req, "ROLE_INFO_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(info.has_value()) << "ROLE_INFO_REQ timed out for a registered uid";

    // HEP-CORE-0027 §4.2 — ROLE_INFO_ACK for a found role carries
    // `found`, `channel`, and inbox metadata (empty when no inbox).
    expect_object_has_keys(
        *info,
        {"found", "channel", "inbox_endpoint", "inbox_packing", "inbox_checksum", "inbox_schema"},
        "ROLE_INFO_ACK", "HEP-CORE-0027 §4.2");
    // Code returns a parsed `inbox_schema` object, not the stringified
    // `inbox_schema_json` some HEPs still mention.  Pin the code contract.
    expect_object_lacks_keys(*info, {"inbox_schema_json"}, "ROLE_INFO_ACK",
                             "HEP-CORE-0027 §4.2 (code returns parsed object; "
                             "any `inbox_schema_json` reference is stale)");

    // ── Case 2: unknown uid → found=false ──
    nlohmann::json req2;
    req2["role_uid"] = "prod.no.such.role.uid00000000";
    auto not_found =
        q.request("ROLE_INFO_REQ", req2, "ROLE_INFO_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(not_found.has_value()) << "ROLE_INFO_REQ for unknown uid should still get an ACK "
                                          "(found=false), not time out";
    expect_object_has_keys(*not_found, {"found"}, "ROLE_INFO_ACK (unknown uid)",
                           "HEP-CORE-0027 §4.2");
    ASSERT_TRUE(not_found->at("found").is_boolean());
    EXPECT_FALSE(not_found->at("found").get<bool>())
        << "ROLE_INFO_ACK.found must be false for an unknown uid";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, WireConformance_BandAck_Shapes)
{
    using namespace std::chrono;
    using namespace pylabhub::tests::wire;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "tr1.bandshape" + suffix;
    const std::string uid = "prod." + channel;
    const std::string band = "!tr1.band" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bandshape");
    const auto setup = make_pattern4_setup({uid});
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
    join_req["band"] = band;
    join_req["role_uid"] = uid;
    join_req["role_name"] = "test_band_member";
    auto join_ack = client.request("BAND_JOIN_REQ", join_req, "BAND_JOIN_ACK",
                                   milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(join_ack.has_value()) << "BAND_JOIN_REQ timed out";
    expect_object_has_keys(*join_ack, {"status", "band", "members"}, "BAND_JOIN_ACK",
                           "HEP-CORE-0030 §5.1");
    expect_string_field(*join_ack, "status", "success", "BAND_JOIN_ACK", "HEP-CORE-0030 §5.1");
    expect_object_lacks_keys(*join_ack, {"channel"}, "BAND_JOIN_ACK",
                             "HEP-CORE-0030 §5.1 (audit B1 — wire key is `band`)");
    ASSERT_TRUE(join_ack->at("band").is_string());
    EXPECT_EQ(join_ack->at("band").get<std::string>(), band);
    ASSERT_TRUE(join_ack->at("members").is_array());

    // ── BAND_MEMBERS_ACK ──
    nlohmann::json members_req;
    members_req["band"] = band;
    auto members_ack = client.request("BAND_MEMBERS_REQ", members_req, "BAND_MEMBERS_ACK",
                                      milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(members_ack.has_value()) << "BAND_MEMBERS_REQ timed out";
    expect_object_has_keys(*members_ack, {"band", "members"}, "BAND_MEMBERS_ACK",
                           "HEP-CORE-0030 §5.1");
    expect_object_lacks_keys(*members_ack, {"channel"}, "BAND_MEMBERS_ACK",
                             "HEP-CORE-0030 §5.1 (audit B1 — wire key is `band`)");

    // ── BAND_LEAVE_ACK ──
    nlohmann::json leave_req;
    leave_req["band"] = band;
    leave_req["role_uid"] = uid;
    auto leave_ack = client.request("BAND_LEAVE_REQ", leave_req, "BAND_LEAVE_ACK",
                                    milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(leave_ack.has_value()) << "BAND_LEAVE_REQ timed out";
    expect_object_has_keys(*leave_ack, {"status"}, "BAND_LEAVE_ACK", "HEP-CORE-0030 §5.1");
    expect_string_field(*leave_ack, "status", "success", "BAND_LEAVE_ACK", "HEP-CORE-0030 §5.1");

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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.dup.same" + suffix;
    const std::string uid1 = "prod.dup.same.uid1" + suffix;
    const std::string uid2 = "prod.dup.same.uid2" + suffix;
    const std::string hash(64, 'a');

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_dup_card");
    const auto setup = make_pattern4_setup({uid1, uid2});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto p1 = make_wire_client(ctx, setup, uid1);
    auto b1 = producer_reg_body(setup, channel, uid1, /*shm=*/true);
    b1["schema_hash"] = hash;
    auto r1 = p1.request("REG_REQ", b1, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r1.has_value()) << "first REG_REQ timed out";
    ASSERT_EQ(r1->value("status", std::string{}), "success")
        << "first SHM producer should register; body=" << r1->dump();

    auto p2 = make_wire_client(ctx, setup, uid2);
    auto b2 = producer_reg_body(setup, channel, uid2, /*shm=*/true);
    b2["schema_hash"] = hash;
    auto r2 = p2.request("REG_REQ", b2, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r2.has_value()) << "broker must return a structured error, not transport failure";
    EXPECT_EQ(r2->value("status", std::string{}), "error")
        << "second SHM producer must reject; body=" << r2->dump();
    // A default (undeclared) SHM channel stores topology `one-to-one`;
    // the second producer trips ONE_TO_ONE_CARDINALITY_VIOLATED (was
    // MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM pre-topology-migration).
    EXPECT_EQ(r2->value("error_code", std::string{}), "ONE_TO_ONE_CARDINALITY_VIOLATED")
        << "body=" << r2->dump();
    // Broker-side path pin: the WARN emitted for the rejected second
    // producer on a default one-to-one SHM channel.
    expect_log(broker, "event=RegReqRejected reason='ONE_TO_ONE_CARDINALITY_VIOLATED'",
               milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, DuplicateReg_DifferentSchemaHash_Rejected)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.dup.diff" + suffix;
    const std::string uid1 = "prod.dup.diff.uid1" + suffix;
    const std::string uid2 = "prod.dup.diff.uid2" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_dup_schema");
    const auto setup = make_pattern4_setup({uid1, uid2});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto p1 = make_wire_client(ctx, setup, uid1);
    auto b1 = producer_reg_body(setup, channel, uid1, /*shm=*/true);
    b1["schema_hash"] = std::string(128, 'a'); // 64-byte two-zone fingerprint width
    auto r1 = p1.request("REG_REQ", b1, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r1.has_value()) << "first REG_REQ timed out";
    ASSERT_EQ(r1->value("status", std::string{}), "success");

    auto p2 = make_wire_client(ctx, setup, uid2);
    auto b2 = producer_reg_body(setup, channel, uid2, /*shm=*/true);
    b2["schema_hash"] = std::string(128, 'b'); // 64-byte two-zone fingerprint width
    auto r2 = p2.request("REG_REQ", b2, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r2.has_value()) << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(r2->value("status", std::string{}), "error");
    EXPECT_EQ(r2->value("error_code", std::string{}), "SCHEMA_MISMATCH") << "body=" << r2->dump();

    broker.signal_quit();
}

// ─── SI-2 open-row validation, producer side (G8b — schema/metrics
//     integration design, 2026-07-26): an ANONYMOUS producer carrying
//     schema STRUCTURE must carry a matching fingerprint.  Hash-only
//     registration (above) stays legal. ───────────────────────────────

TEST_F(Pattern4BrokerProtocolTest, Reg_OwnerWithoutId_Rejected)
{
    // SI-9 hygiene (ruled 2026-07-26): a producer schema_owner claim
    // without a schema_id was silently ignored — now INVALID_REQUEST
    // (consumer twin pinned in FanInOwnerOpen_OwnerAxis_Rejections).
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "prod.ownernoid" + suffix;
    const std::string channel = "proto.owner_no_id" + suffix;

    const fs::path temp_dir = make_test_temp_dir("p4_owner_no_id");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
    body["schema_owner"] = "hub"; // owner claim, NO schema_id
    auto resp = prod.request("REG_REQ", body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST") << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, NamedReg_MissingBlds_Rejected)
{
    // HEP-0034 §10.1: a NAMED registration must carry the full
    // structure — schema_id + packing + hash without schema_blds is
    // MISSING_BLDS (nothing to hash, nothing to install).
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "prod.noblds" + suffix;
    const std::string channel = "proto.named_no_blds" + suffix;

    const fs::path temp_dir = make_test_temp_dir("p4_named_no_blds");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
    body["schema_id"] = "$lab.noblds.v1";
    body["schema_packing"] = "aligned";
    body["schema_hash"] = std::string(128, 'a');
    // no schema_blds
    auto resp = prod.request("REG_REQ", body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "MISSING_BLDS") << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, AnonymousReg_StructureWithoutHash_Rejected)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.si2.nohash" + suffix;
    const std::string uid = "prod.si2.nohash" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_si2_nohash");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto p = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
    body["schema_blds"] = "ts:f64:1:0"; // structure, deliberately no hash
    body["schema_packing"] = "aligned";
    auto r = p.request("REG_REQ", body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->value("status", std::string{}), "error");
    EXPECT_EQ(r->value("error_code", std::string{}), "MISSING_HASH") << "body=" << r->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, AnonymousReg_InconsistentFingerprint_Rejected)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.si2.badfp" + suffix;
    const std::string uid = "prod.si2.badfp" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_si2_badfp");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto p = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
    body["schema_blds"] = "ts:f64:1:0";
    body["schema_packing"] = "aligned";
    body["schema_hash"] = std::string(128, 'a'); // valid hex, wrong value
    auto r = p.request("REG_REQ", body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->value("status", std::string{}), "error");
    EXPECT_EQ(r->value("error_code", std::string{}), "FINGERPRINT_INCONSISTENT")
        << "body=" << r->dump();

    broker.signal_quit();
}

// ─── Transport arbitration (producer transport vs consumer data_transport) ─
//
// HEP-CORE-0036 §5b.6: `data_transport` is REQUIRED on CONSUMER_REG_REQ and
// must equal the channel's stored transport or the broker rejects with
// TRANSPORT_MISMATCH.  (The pre-§5b.6 `consumer_queue_type` field is
// retired — "Forbidden / removed" — and the broker no longer reads it.)

TEST_F(Pattern4BrokerProtocolTest, TransportMismatch_ShmProducer_ZmqConsumer_Fails)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.transport.shm_zmq" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_tx_mismatch");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    auto pr = prod.request("REG_REQ", producer_reg_body(setup, channel, prod_uid, true), "REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(pr.has_value()) << "producer REG_REQ timed out";
    ASSERT_EQ(pr->value("status", std::string{}), "success");
    // R6 producer-kLive gate must clear before the broker reaches the
    // transport-arbitration check that is this test's subject.
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    // SHM channel, consumer DECLARES data_transport="zmq" — §5b.6 mismatch.
    auto cbody = consumer_reg_body(setup, channel, cons_uid, /*topology=*/{},
                                   /*data_transport=*/"zmq");
    auto cr = cons.request("CONSUMER_REG_REQ", cbody, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value()) << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(cr->value("status", std::string{}), "error");
    EXPECT_EQ(cr->value("error_code", std::string{}), "TRANSPORT_MISMATCH")
        << "body=" << cr->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, TransportMatch_ShmConsumer_ShmProducer_Succeeds)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.transport.shm_shm" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_tx_shm");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    auto pr = prod.request("REG_REQ", producer_reg_body(setup, channel, prod_uid, true), "REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(pr.has_value()) << "producer REG_REQ timed out";
    ASSERT_EQ(pr->value("status", std::string{}), "success");
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    // SHM channel, consumer DECLARES data_transport="shm" — §5b.6 match.
    auto cbody = consumer_reg_body(setup, channel, cons_uid, /*topology=*/{},
                                   /*data_transport=*/"shm");
    auto cr = cons.request("CONSUMER_REG_REQ", cbody, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value()) << "CONSUMER_REG_REQ timed out";
    EXPECT_EQ(cr->value("status", std::string{}), "success")
        << "both sides use SHM — should succeed; body=" << cr->dump();

    broker.signal_quit();
}

// §5b.6 has no "no declaration" case — `data_transport` is REQUIRED and the
// handler validates the VALUE ∈ {"shm","zmq"} before the mismatch check
// (mirrors the producer-side #281 handling).  This replaces the retired
// `TransportMatch_NoDriverField_AlwaysSucceeds`, which pinned the abolished
// "omitted consumer_queue_type → arbitration skipped" behavior.
TEST_F(Pattern4BrokerProtocolTest, TransportValue_Bogus_RejectedInvalidRequest)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.transport.badvalue" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_tx_badvalue");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    auto pr = prod.request("REG_REQ", producer_reg_body(setup, channel, prod_uid, true), "REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(pr.has_value()) << "producer REG_REQ timed out";
    ASSERT_EQ(pr->value("status", std::string{}), "success");
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    auto cr = cons.request("CONSUMER_REG_REQ",
                           consumer_reg_body(setup, channel, cons_uid, /*topology=*/{},
                                             /*data_transport=*/"bogus"),
                           "CONSUMER_REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value()) << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(cr->value("status", std::string{}), "error");
    EXPECT_EQ(cr->value("error_code", std::string{}), "INVALID_REQUEST") << "body=" << cr->dump();

    broker.signal_quit();
}

// ─── Optional-field-absent wire conformance ────────────────────────────────

// End-to-end pin for the §14.3 optional-accessor contract: a REG_REQ that
// omits `role_name` entirely (it is OPTIONAL — a redundant display label,
// HEP-0046 §14.3) must register successfully.  Regression guard for the
// 2026-07-24 incident where the typed handler's throwing `role_name()`
// accessor crashed the broker on exactly this wire.
TEST_F(Pattern4BrokerProtocolTest, RegReq_WithoutRoleName_Succeeds)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.optional.norolename" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_norolename");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
    body.erase("role_name");
    auto reply = prod.request("REG_REQ", body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(reply->value("status", std::string{}), "success")
        << "role_name is OPTIONAL — omitting it must not fail; body=" << reply->dump();

    broker.signal_quit();
}

// ─── REG_ACK / CONSUMER_REG_ACK heartbeat-negotiation block ────────────────

TEST_F(Pattern4BrokerProtocolTest, RegAck_ContainsHeartbeatBlock_Defaults)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.regack.hb_default" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_hb_default");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    nlohmann::json reg;
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid, &reg));

    ASSERT_TRUE(reg.contains("heartbeat")) << "REG_ACK missing heartbeat block";
    const auto &hb = reg["heartbeat"];
    ASSERT_TRUE(hb.is_object());
    EXPECT_EQ(hb.value("heartbeat_interval_ms", -1), pylabhub::kDefaultHeartbeatIntervalMs);
    EXPECT_EQ(hb.value("ready_miss_heartbeats", std::uint32_t{0}),
              pylabhub::kDefaultReadyMissHeartbeats);
    EXPECT_EQ(hb.value("pending_miss_heartbeats", std::uint32_t{0}),
              pylabhub::kDefaultPendingMissHeartbeats);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RegAck_HeartbeatBlock_HonorsCustomConfig)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.regack.hb_custom" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_hb_custom");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    // "hb_custom" profile: heartbeat_interval=250ms, ready_miss=12,
    // pending_miss=8 — the REG_ACK block must echo these.
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "hb_custom"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.cons_regack.hb" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_cons_hb");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    nlohmann::json cons_reg;
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid, &cons_reg));

    ASSERT_TRUE(cons_reg.contains("heartbeat")) << "CONSUMER_REG_ACK missing heartbeat block";
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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.checksum.prod" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string reporter_uid = "reporter" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_checksum_fwd");
    const auto setup = make_pattern4_setup({prod_uid, reporter_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "checksum_notify"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));

    auto reporter = make_wire_client(ctx, setup, reporter_uid);
    nlohmann::json report;
    report["channel_name"] = channel;
    report["slot_index"] = 42;
    report["error"] = "bad CRC in slot 42";
    report["reporter_pid"] = pylabhub::platform::get_pid();
    reporter.send("CHECKSUM_ERROR_REPORT", report);

    auto notify = drain_for(prod, "CHANNEL_EVENT_NOTIFY", milliseconds{pylabhub::kLongTimeoutMs});
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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.bcast.fanout" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons1_uid = "cons.first." + channel;
    const std::string cons2_uid = "cons.second." + channel;
    const std::string send_uid = "prod.broadcast.sender" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_fanout");
    const auto setup = make_pattern4_setup({prod_uid, cons1_uid, cons2_uid, send_uid});
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
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid, nullptr, "fan-out"));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons1 = make_wire_client(ctx, setup, cons1_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_consumer(cons1, setup, channel, cons1_uid, nullptr, "fan-out"));
    auto cons2 = make_wire_client(ctx, setup, cons2_uid);
    ASSERT_NO_FATAL_FAILURE(
        register_consumer(cons2, setup, channel, cons2_uid, nullptr, "fan-out"));

    auto sender = make_wire_client(ctx, setup, send_uid);
    nlohmann::json bcast;
    bcast["target_channel"] = channel;
    bcast["message"] = "hello-fan-out";
    bcast["data"] = "";
    sender.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    auto check = [&](BrokerWireClient &c, const char *who)
    {
        auto n = drain_for(c, "CHANNEL_BROADCAST_DELIVER_NOTIFY",
                           milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(n.has_value()) << who << " did not receive CHANNEL_BROADCAST_DELIVER_NOTIFY";
        EXPECT_EQ(n->value("channel_name", std::string{}), channel) << who;
        EXPECT_EQ(n->value("event", std::string{}), "broadcast") << who;
        // The request never said who was sending.  This name came from the
        // key the sender's connection proved at handshake.
        EXPECT_EQ(n->value("sender_uid", std::string{}), send_uid) << who;
        EXPECT_EQ(n->value("message", std::string{}), "hello-fan-out") << who;
    };
    ASSERT_NO_FATAL_FAILURE(check(prod, "producer"));
    ASSERT_NO_FATAL_FAILURE(check(cons1, "cons1"));
    ASSERT_NO_FATAL_FAILURE(check(cons2, "cons2"));

    // The external non-member sender must NOT receive the fan-out.
    auto leaked = drain_for(sender, "CHANNEL_BROADCAST_DELIVER_NOTIFY", milliseconds{300});
    EXPECT_FALSE(leaked.has_value())
        << "non-member sender unexpectedly received the broadcast NOTIFY";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, BroadcastFanOut_DataPayloadRoundTrip)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.bcast.payload" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string send_uid = "ext.bcast.payload" + suffix;
    const std::string msg = "payload-test";
    const std::string data = R"({"k":"v","n":42,"arr":[1,2,3]})";

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_data");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid, send_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto sender = make_wire_client(ctx, setup, send_uid);
    nlohmann::json bcast;
    bcast["target_channel"] = channel;
    bcast["message"] = msg;
    bcast["data"] = data;
    sender.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    auto n =
        drain_for(cons, "CHANNEL_BROADCAST_DELIVER_NOTIFY", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(n.has_value()) << "consumer did not receive broadcast NOTIFY with data payload";
    EXPECT_EQ(n->value("channel_name", std::string{}), channel);
    EXPECT_EQ(n->value("event", std::string{}), "broadcast");
    EXPECT_EQ(n->value("sender_uid", std::string{}), send_uid);
    EXPECT_EQ(n->value("message", std::string{}), msg);
    EXPECT_EQ(n->value("data", std::string{}), data) << "data payload was modified in transit";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, BroadcastUnknownChannel_NoNotifyDelivered)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string other_ch = "proto.bcast.other" + suffix;
    const std::string other_prd = "prod." + other_ch;
    const std::string spec_uid = "cons." + other_ch;
    const std::string unknown = "proto.bcast.unknown" + suffix;
    const std::string send_uid = "ext.bcast.unknown" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_unknown");
    const auto setup = make_pattern4_setup({other_prd, spec_uid, send_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto other_prod = make_wire_client(ctx, setup, other_prd);
    ASSERT_NO_FATAL_FAILURE(register_producer(other_prod, setup, other_ch, other_prd));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(other_prod, other_ch, other_prd));

    auto spec = make_wire_client(ctx, setup, spec_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(spec, setup, other_ch, spec_uid));

    auto sender = make_wire_client(ctx, setup, send_uid);
    nlohmann::json bcast;
    bcast["target_channel"] = unknown; // no such channel
    bcast["message"] = "into-the-void";
    bcast["data"] = "";
    sender.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    // The unrelated other-channel consumer must not receive the leak.
    auto leaked = drain_for(spec, "CHANNEL_BROADCAST_DELIVER_NOTIFY", milliseconds{300});
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

// ─── Broadcast sender attribution (HEP-CORE-0035 §4.2.2) ──────────────────
//
// Recipients read a broadcast's `sender_uid` as fact, so it is decided by
// the broker from the key the connection proved — not announced by the
// sender.  These two pin the halves of that: the name follows the key and
// not the routing id, and a body that still declares a sender is refused
// rather than quietly corrected.

TEST_F(Pattern4BrokerProtocolTest, Broadcast_AttributedToProvenKey_NotRoutingId)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "bcastatk.ch" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string alice = "prod.bcast.alice" + suffix;
    const std::string bob = "prod.bcast.bob" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_attrib");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid, alice, bob});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));
    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    // Alice's key on the socket, Bob's uid as the routing id — the one
    // value a client picks for itself.  Both are well-formed and both name
    // roles this hub knows, so nothing here is malformed; the only thing
    // separating them is which one was PROVEN.
    const auto &alice_kp = setup.curve.role(alice);
    BrokerWireClient::Config c;
    c.broker_endpoint = setup.broker_endpoint;
    c.broker_pubkey = setup.curve.hub.public_z85;
    c.client_pubkey = alice_kp.public_z85;
    c.client_seckey = alice_kp.secret_z85;
    c.client_role_uid = bob;
    BrokerWireClient masquerader(ctx, c);

    nlohmann::json bcast;
    bcast["target_channel"] = channel;
    bcast["message"] = "who-sent-this";
    masquerader.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    auto n =
        drain_for(cons, "CHANNEL_BROADCAST_DELIVER_NOTIFY", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(n.has_value()) << "consumer did not receive the broadcast";
    EXPECT_EQ(n->value("sender_uid", std::string{}), alice)
        << "the broadcast must be attributed to the key the connection PROVED";
    EXPECT_NE(n->value("sender_uid", std::string{}), bob)
        << "attribution followed the client-chosen routing id — anyone could "
           "then broadcast under any uid; body="
        << n->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, Broadcast_BodyDeclaringSender_Refused)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "bcastdecl.ch" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string alice = "prod.decl.alice" + suffix;
    const std::string bob = "prod.decl.bob" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_bcast_declared");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid, alice, bob});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));
    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    // Alice, under her own identity, declaring Bob as the sender.
    auto sender = make_wire_client(ctx, setup, alice);
    nlohmann::json bcast;
    bcast["target_channel"] = channel;
    bcast["sender_uid"] = bob; // retired field
    bcast["message"] = "labelled-as-bob";
    sender.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    // The harm first, and unconditionally.  Refused means NOT delivered;
    // re-attributing the message to Alice and sending it anyway would also
    // be "safe", and would still be wrong, because Alice addressed it as Bob
    // and has no way to learn it went out otherwise.  Asserted before the
    // reply so that a build which accepts the body — and therefore answers
    // nothing, this being fire-and-forget — still fails HERE, on the
    // delivery, rather than on a missing error reply.
    auto leaked = drain_for(cons, "CHANNEL_BROADCAST_DELIVER_NOTIFY",
                            milliseconds{pylabhub::kShortTimeoutMs});
    EXPECT_FALSE(leaked.has_value()) << "a broadcast declaring its own sender was delivered; body="
                                     << (leaked ? leaked->dump() : "");

    auto reply = sender.receive(milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value()) << "broker did not answer the declared-sender broadcast";
    EXPECT_EQ(reply->first, "ERROR") << "body=" << reply->second.dump();
    EXPECT_EQ(reply->second.value("error_code", std::string{}), "BODY_SCHEMA_VIOLATION")
        << "body=" << reply->second.dump();
    EXPECT_NE(reply->second.value("message", std::string{}).find("sender_uid"), std::string::npos)
        << "the rejection must name the offending field, or an operator cannot "
           "tell which of the body's fields the broker refused; body="
        << reply->second.dump();

    broker.signal_quit();
}

// ─── Control-tier ownership (HEP-CORE-0035 §4.2) ──────────────────────────
//
// A heartbeat holds a role's presence alive, so forging one keeps a DEAD
// role looking alive.  Note what makes this the sharpest case of the
// family: the transport refuses a second connection using a routing id
// already in use, which blocks impersonation of a role that is currently
// attached — and a heartbeat is only worth forging once the victim is gone
// and its routing id is free.  The transport contributes nothing here.

TEST_F(Pattern4BrokerProtocolTest, HeartbeatNotify_ProvenKeyClaimingAnotherRole_Rejected)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "hbatk.ch" + suffix;
    const std::string bob = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string alice = "prod.hb.alice" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_hb_attack");
    const auto setup = make_pattern4_setup({bob, cons_uid, alice});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;

    // Bob registers and heartbeats once — enough for the consumer to attach —
    // then releases the connection, freeing his routing id.
    {
        auto victim = make_wire_client(ctx, setup, bob);
        ASSERT_NO_FATAL_FAILURE(register_producer(victim, setup, channel, bob));
        ASSERT_NO_FATAL_FAILURE(producer_heartbeat(victim, channel, bob));
    }

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    const auto &alice_kp = setup.curve.role(alice);
    BrokerWireClient::Config c;
    c.broker_endpoint = setup.broker_endpoint;
    c.broker_pubkey = setup.curve.hub.public_z85;
    c.client_pubkey = alice_kp.public_z85;
    c.client_seckey = alice_kp.secret_z85;
    c.client_role_uid = bob;

    // Metrics ride the heartbeat, which gives the forgery an observable
    // footprint in broker state: if the heartbeat lands, this marker is
    // readable back through METRICS_REQ under Bob's presence.
    nlohmann::json hb;
    hb["channel_name"] = channel;
    hb["role_uid"] = bob;
    hb["role_type"] = "producer";
    hb["metrics"] = nlohmann::json{{"forged_marker", 1}};

    // Bob's routing id is released asynchronously after his disconnect, and
    // until it is, the broker's ROUTER drops this client's frames.  Wait for
    // a connection that is actually being serviced, using a query whose
    // reply is unconditional — NOT the heartbeat, whose whole point is that
    // it draws no reply when accepted.  Polling on the heartbeat's reply
    // would make "the gate is missing" indistinguishable from "the id is
    // still held", and the test would spend its budget proving neither.  The
    // winning client is kept: dropping it would release the id and put the
    // next one back at the start of the same wait.
    std::unique_ptr<BrokerWireClient> attacker;
    ASSERT_TRUE(pylabhub::tests::helper::poll_until(
        [&]
        {
            auto probe = std::make_unique<BrokerWireClient>(ctx, c);
            nlohmann::json q;
            q["role_uid"] = bob;
            if (!probe
                     ->request("ROLE_PRESENCE_REQ", q, "ROLE_PRESENCE_ACK",
                               milliseconds{pylabhub::kShortTimeoutMs})
                     .has_value())
                return false;
            attacker = std::move(probe);
            return true;
        },
        milliseconds{pylabhub::kLongTimeoutMs}))
        << "no connection under Bob's routing id was ever serviced";

    attacker->send("HEARTBEAT_NOTIFY", hb);
    auto reply = attacker->receive(milliseconds{pylabhub::kMidTimeoutMs});
    EXPECT_TRUE(reply.has_value()) << "Alice's key MUST NOT hold Bob's presence alive — the "
                                      "forged heartbeat drew no rejection at all";
    if (reply.has_value())
    {
        EXPECT_EQ(reply->first, "ERROR") << "body=" << reply->second.dump();
        EXPECT_EQ(reply->second.value("error_code", std::string{}), "IDENTITY_MISMATCH")
            << "body=" << reply->second.dump();
    }

    // Side effect, not just the reply.  A gate that answered ERROR while the
    // handler still refreshed the presence would satisfy the assertions above
    // and leave a dead producer looking alive.
    nlohmann::json mreq;
    mreq["channel_name"] = channel;
    mreq["role_uid"] = cons_uid;
    auto metrics =
        cons.request("METRICS_REQ", mreq, "METRICS_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(metrics.has_value()) << "METRICS_REQ timed out";
    EXPECT_EQ(metrics->value("metrics", nlohmann::json::object()).dump().find("forged_marker"),
              std::string::npos)
        << "the forged heartbeat was applied to Bob's presence despite the "
           "rejection; metrics="
        << metrics->value("metrics", nlohmann::json::object()).dump();

    broker.signal_quit();
}

// ─── Round-1 leftover hybrids, re-migrated via broker log traces ───────────
// These verified broker-internal state in-process; the broker subprocess
// already logs the state at the decision point, so the parent reads that.

// heartbeat wire payload carries role_uid + role_type — the broker's
// "first heartbeat received" INFO trace echoes both (heartbeats are
// fire-and-forget, so the log is the only observable).
TEST_F(Pattern4BrokerProtocolTest, HeartbeatWirePayloadIncludesUidAndRoleType)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.hb.wire.uid" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("proto_hb_wire");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    expect_log(broker,
               "first heartbeat received from role='" + uid + "' channel='" + channel +
                   "' role_type='producer'",
               milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

// heartbeat transitions the producer presence toward Live — the broker
// logs "pending first heartbeat" on REG (registering) and
// "producer-presence sub-Live" on the heartbeat (transition observability).
TEST_F(Pattern4BrokerProtocolTest, HeartbeatTransitionsToReady)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "proto.hb.ready" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("proto_hb_ready");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    // Registered but not yet heartbeat: the RegReqAccepted trace marks the
    // "(pending first heartbeat)" (registering) state for this channel.
    expect_log(broker, "event=RegReqAccepted role='" + uid + "' channel='" + channel + "'",
               milliseconds{pylabhub::kMidTimeoutMs});

    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));
    // The heartbeat drives the presence toward Live.
    expect_log(broker, "channel '" + channel + "' producer-presence sub-Live",
               milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}

// A CHECKSUM_ERROR_REPORT for an unknown channel is silently dropped and
// the broker stays operational — observed by a follow-up wire request
// still getting a reply (replaces the in-process query_channel_snapshot
// liveness probe).
TEST_F(Pattern4BrokerProtocolTest, ChecksumErrorReport_UnknownChannel_Silent)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string reporter_uid = "reporter.bogus" + suffix;

    const fs::path temp_dir = make_test_temp_dir("proto_checksum_unknown");
    const auto setup = make_pattern4_setup({reporter_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto reporter = make_wire_client(ctx, setup, reporter_uid);

    nlohmann::json report;
    report["channel_name"] = "proto.checksum.bogus" + suffix; // never registered
    report["slot_index"] = 0;
    report["error"] = "test";
    report["reporter_pid"] = pylabhub::platform::get_pid();
    reporter.send("CHECKSUM_ERROR_REPORT", report);

    // Liveness: a subsequent request still gets a reply.
    nlohmann::json req;
    req["role_uid"] = "prod.no.such.role.uid00000000";
    auto resp = reporter.request("ROLE_PRESENCE_REQ", req, "ROLE_PRESENCE_ACK",
                                 milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "broker stopped servicing requests after an unknown-channel "
                                     "checksum report";
    EXPECT_FALSE(resp->value("present", true));

    broker.signal_quit();
}

// ── I-REPLAY-BOUND: LIVE admission path, end-to-end ──────────────────────────
//
// These pin the ACTUAL live wiring that the unit tests (gate_replay_bound in
// test_admission_gates, HubState::nonce_seen in test_hub_state_nonce_dedup)
// do NOT cover: recv loop (broker_service.cpp:1377) → receive_and_validate →
// run_reg_family_gates → check_replay_bound → HubState::nonce_seen →
// dispatch_received ERROR reply.  Before these, nothing proved a replayed REG
// is rejected through the real broker.

TEST_F(Pattern4BrokerProtocolTest, RegReq_ReplayedNonce_RejectedReplayOrSkew)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "prod.replay.nonce" + suffix;
    const std::string channel = "replay.nonce.ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_replay_nonce");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto client = make_wire_client(ctx, setup, uid);

    // Pin client_nonce so both sends carry the SAME tag (the wire client
    // preserves a caller-set client_nonce; a fresh client_wall_ts is stamped
    // each send, so it is the NONCE — not skew — that trips the second one).
    nlohmann::json body = producer_reg_body(setup, channel, uid, /*shm=*/false);
    body["client_nonce"] = "fixed.replay.nonce.0123456789abcdef";

    auto ack = client.request("REG_REQ", body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(ack.has_value()) << "first REG_REQ timed out";
    ASSERT_EQ(ack->value("status", std::string{}), "success")
        << "first REG must be admitted; body=" << ack->dump();

    client.send("REG_REQ", body); // replay: same nonce
    auto reply = client.receive(milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value()) << "no reply to replayed REG_REQ";
    EXPECT_EQ(reply->first, "ERROR")
        << "replayed REG must be rejected, not ACKed; got msg_type=" << reply->first
        << " body=" << reply->second.dump();
    EXPECT_EQ(reply->second.value("error_code", std::string{}), "REPLAY_OR_SKEW")
        << "replayed REG must reject REPLAY_OR_SKEW; body=" << reply->second.dump();

    broker.signal_quit();
}

// ── Attested identity: a proven key cannot register as someone else ────
//
// THE defect this closes (HEP-CORE-0035 §4.2).  Alice and Bob are BOTH in
// the roster, so neither of the cases below is an unknown-key rejection:
// every value on the wire is one the hub recognises.  What separates them
// is provenance — which key the connection actually PROVED at handshake —
// and that is the fact the registration path used to discard.
//
// Both cases set the DEALER routing id to Bob's uid deliberately.  Without
// it the dealer-identity consistency gate rejects first, the attested
// binding gate never runs, and the test would pass while proving nothing.

TEST_F(Pattern4BrokerProtocolTest, RegReq_ProvenKeyClaimingAnotherRolesKey_Rejected)
{
    // Alice proves Alice's key, then sends {role_uid: bob, zmq_pubkey:
    // BOB'S KEY}.  A roster lookup on that pair MATCHES — which is exactly
    // why reading the body alone admitted this.  The announced key
    // disagrees with the proven one, so it dies as PUBKEY_MISMATCH.
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string alice = "prod.attested.alice" + suffix;
    const std::string bob = "prod.attested.bob" + suffix;
    const std::string alice_ch = "attested.alice.ch" + suffix;
    const std::string bob_ch = "attested.bob.ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_attested_key");
    const auto setup = make_pattern4_setup({alice, bob});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;

    // Premise: this broker and roster DO admit Alice as Alice.  Without
    // this, a broker that rejected everything would pass the assertion
    // below for the wrong reason.
    {
        auto honest = make_wire_client(ctx, setup, alice);
        auto ack = honest.request("REG_REQ",
                                  producer_reg_body(setup, alice_ch, alice,
                                                    /*shm=*/false),
                                  "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(ack.has_value()) << "honest REG_REQ timed out";
        ASSERT_EQ(ack->value("status", std::string{}), "success")
            << "Alice must be admitted as Alice, or the impersonation "
               "assertion below proves nothing; body="
            << ack->dump();
    }

    // Alice's keypair on the socket, Bob's uid as routing id.
    const auto &alice_kp = setup.curve.role(alice);
    BrokerWireClient::Config c;
    c.broker_endpoint = setup.broker_endpoint;
    c.broker_pubkey = setup.curve.hub.public_z85;
    c.client_pubkey = alice_kp.public_z85;
    c.client_seckey = alice_kp.secret_z85;
    c.client_role_uid = bob;
    BrokerWireClient impostor(ctx, c);

    // Body claims Bob completely — Bob's uid AND Bob's real pubkey.
    impostor.send("REG_REQ", producer_reg_body(setup, bob_ch, bob, /*shm=*/false));
    auto reply = impostor.receive(milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value()) << "no reply to impersonating REG_REQ";
    EXPECT_EQ(reply->first, "ERROR")
        << "a peer that proved Alice's key MUST NOT register as Bob; got msg_type=" << reply->first
        << " body=" << reply->second.dump();
    EXPECT_EQ(reply->second.value("error_code", std::string{}), "PUBKEY_MISMATCH")
        << "the announced key disagrees with the proven one; body=" << reply->second.dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RegReq_ProvenKeyClaimingAnotherRolesUid_Rejected)
{
    // The subtler half.  Alice proves Alice's key and announces ALICE'S
    // key — internally consistent, so the announced-vs-proven check passes
    // — but claims Bob's uid.  Only resolving the proven key to its owner
    // catches this: the key belongs to Alice, so Alice is who registers,
    // and the claim is refused as IDENTITY_MISMATCH.
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string alice = "prod.attested2.alice" + suffix;
    const std::string bob = "prod.attested2.bob" + suffix;
    const std::string channel = "attested2.ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_attested_uid");
    const auto setup = make_pattern4_setup({alice, bob});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;

    const auto &alice_kp = setup.curve.role(alice);
    BrokerWireClient::Config c;
    c.broker_endpoint = setup.broker_endpoint;
    c.broker_pubkey = setup.curve.hub.public_z85;
    c.client_pubkey = alice_kp.public_z85;
    c.client_seckey = alice_kp.secret_z85;
    c.client_role_uid = bob;
    BrokerWireClient impostor(ctx, c);

    // Claim Bob's uid, but announce the key actually proved (Alice's), so
    // the body is self-consistent with the handshake.
    nlohmann::json body = producer_reg_body(setup, channel, bob, /*shm=*/false);
    body["zmq_pubkey"] = alice_kp.public_z85;

    impostor.send("REG_REQ", body);
    auto reply = impostor.receive(milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value()) << "no reply to impersonating REG_REQ";
    EXPECT_EQ(reply->first, "ERROR")
        << "Alice's proven key MUST NOT carry Bob's identity; got msg_type=" << reply->first
        << " body=" << reply->second.dump();
    EXPECT_EQ(reply->second.value("error_code", std::string{}), "IDENTITY_MISMATCH")
        << "the proven key belongs to Alice, not the claimed Bob; body=" << reply->second.dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, DeregReq_ProvenKeyTargetingAnotherRole_Rejected)
{
    // The post-registration half of the same defect.  Bob registers
    // legitimately.  Alice — a perfectly valid role, admitted by ZAP with her
    // own key — then sends DEREG_REQ naming Bob, with her routing id set to
    // Bob's uid so the dealer-identity consistency check is satisfied.
    //
    // Before the ownership gate this succeeded: the handler resolved its
    // target by role_uid alone, and on a last-producer leave that tears the
    // channel down for every consumer attached to it.  Nothing about it
    // required a stolen key — only a valid one.
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string alice = "prod.deregatk.alice" + suffix;
    const std::string bob = "prod.deregatk.bob" + suffix;
    const std::string querier = "QUERIER-deregatk" + suffix;
    const std::string channel = "deregatk.ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_dereg_attack");
    const auto setup = make_pattern4_setup({alice, bob, querier});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;

    // Bob registers for real, then RELEASES the connection.  A ROUTER
    // refuses a second peer presenting a routing id already in use, so the
    // attacker below — which must present Bob's uid as its routing id to
    // reach the gate under test — cannot connect while Bob is attached.
    // Registration outlives the connection ("disconnect is terminal" is not
    // enforced today; see #93), which is precisely what makes the attack
    // worth defending against.
    {
        auto victim = make_wire_client(ctx, setup, bob);
        auto ack = victim.request("REG_REQ", producer_reg_body(setup, channel, bob, /*shm=*/false),
                                  "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(ack.has_value()) << "victim REG_REQ timed out";
        ASSERT_EQ(ack->value("status", std::string{}), "success") << "body=" << ack->dump();
    }

    // Independent observer, on its own identity — the tier that may
    // legitimately ask about another role.
    auto observer = make_wire_client(ctx, setup, querier);
    const auto bob_is_present = [&]() -> bool
    {
        nlohmann::json q;
        q["role_uid"] = bob;
        auto resp = observer.request("ROLE_PRESENCE_REQ", q, "ROLE_PRESENCE_ACK",
                                     milliseconds{pylabhub::kLongTimeoutMs});
        EXPECT_TRUE(resp.has_value()) << "ROLE_PRESENCE_REQ timed out";
        return resp.has_value() && resp->value("present", false);
    };

    ASSERT_TRUE(bob_is_present()) << "Bob must be registered, or the attack below proves nothing";

    // Alice's key on the socket; Bob's uid everywhere the client controls.
    const auto &alice_kp = setup.curve.role(alice);
    BrokerWireClient::Config c;
    c.broker_endpoint = setup.broker_endpoint;
    c.broker_pubkey = setup.curve.hub.public_z85;
    c.client_pubkey = alice_kp.public_z85;
    c.client_seckey = alice_kp.secret_z85;
    c.client_role_uid = bob;

    nlohmann::json dereg;
    dereg["channel_name"] = channel;
    dereg["role_uid"] = bob;

    // The broker's ROUTER releases Bob's routing id asynchronously after his
    // disconnect, and until it does it drops this client's frames.  Poll the
    // condition that actually has to hold — the hostile request reaches the
    // broker and is answered — instead of any proxy for it.  A fresh client
    // per attempt also draws a fresh nonce, so retries are not replays.
    std::optional<std::pair<std::string, nlohmann::json>> reply;
    ASSERT_TRUE(pylabhub::tests::helper::poll_until(
        [&]
        {
            BrokerWireClient attacker(ctx, c);
            attacker.send("DEREG_REQ", dereg);
            reply = attacker.receive(milliseconds{pylabhub::kShortTimeoutMs});
            return reply.has_value();
        },
        milliseconds{pylabhub::kLongTimeoutMs}))
        << "hostile DEREG_REQ never reached the broker";

    EXPECT_EQ(reply->first, "ERROR")
        << "Alice's key MUST NOT deregister Bob; got msg_type=" << reply->first
        << " body=" << reply->second.dump();
    EXPECT_EQ(reply->second.value("error_code", std::string{}), "IDENTITY_MISMATCH")
        << "body=" << reply->second.dump();

    // Side effect, not just the reply.  A gate that returned an error while
    // the handler still dropped the producer would satisfy the assertion
    // above and lose the channel anyway.
    EXPECT_TRUE(bob_is_present())
        << "Bob's registration must have SURVIVED the hostile DEREG — if he is "
           "gone, the rejection was cosmetic and the channel was torn down";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerProtocolTest, RegReq_StaleTimestamp_RejectedReplayOrSkew)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "prod.stale.ts" + suffix;
    const std::string channel = "stale.ts.ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("broker_protocol_stale_ts");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto client = make_wire_client(ctx, setup, uid);

    // Stamp a wall_ts far in the past (well beyond the 30 s skew tolerance).
    // client_nonce is left unset → wire client auto-stamps a fresh one, so it
    // is the SKEW leg (not nonce reuse) that must trip.
    nlohmann::json body = producer_reg_body(setup, channel, uid, /*shm=*/false);
    body["client_wall_ts"] = static_cast<std::uint64_t>(1'000'000ULL); // ~1970

    client.send("REG_REQ", body);
    auto reply = client.receive(milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reply.has_value()) << "no reply to stale-timestamp REG_REQ";
    EXPECT_EQ(reply->first, "ERROR");
    EXPECT_EQ(reply->second.value("error_code", std::string{}), "REPLAY_OR_SKEW")
        << "stale wall_ts must reject REPLAY_OR_SKEW; body=" << reply->second.dump();

    broker.signal_quit();
}
