/**
 * @file test_pattern4_channel_group.cpp
 * @brief Pattern 4 band pub/sub (channel-group) wire tests
 *        (HEP-CORE-0030).
 *
 * Successors of the wire-observable workers formerly in
 * `tests/test_layer3_datahub/workers/datahub_channel_group_workers.cpp`
 * against the in-process `DirectBrokerHandle` co-host (broker +
 * `ChannelClient`/`BrokerRequestComm` poll loop in one process — the
 * HubHostBrokerHandle-class antipattern, README_testing.md line 565 +
 * HEP-CORE-0036 §7.4).  Broker runs in its own subprocess; the parent
 * drives BAND_JOIN / BAND_LEAVE / BAND_BROADCAST via `BrokerWireClient`
 * and asserts the **actual** ACK bodies and pushed NOTIFY frames
 * (`drain_for`) rather than a subprocess exit code.
 *
 * Migration reference: task #52 Round 6 (DirectBrokerHandle sweep).
 */
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"

#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4ChannelGroupTest : public pylabhub::tests::pattern4::Pattern4WireTest
{
  protected:
    // Synchronous BAND_JOIN_REQ → returns the ACK body (carries the
    // member list per broker_service.cpp handle_band_join_req).  Pins
    // status=="success" here: `request()` also returns the ERROR body, so a
    // caller gating only on `has_value()` would silently accept a rejected
    // join — this turns that into a precise failure at the join site.
    std::optional<nlohmann::json> band_join(BrokerWireClient &c, const std::string &band,
                                            const std::string &uid)
    {
        nlohmann::json req;
        req["band"] = band;
        req["role_uid"] = uid;
        req["role_name"] = uid;
        auto reply = c.request("BAND_JOIN_REQ", req, "BAND_JOIN_ACK",
                               std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        if (reply)
        {
            EXPECT_EQ(reply->value("status", std::string{}), "success")
                << "BAND_JOIN_REQ for '" << band << "' (" << uid
                << ") was rejected; body=" << reply->dump();
        }
        return reply;
    }

    // Fire-and-forget BAND_BROADCAST_SEND_NOTIFY (matches BRC
    // band_broadcast: sender key is `role_uid`, payload under `body`).
    void band_broadcast(BrokerWireClient &c, const std::string &band, const std::string &uid,
                        const nlohmann::json &body)
    {
        nlohmann::json p;
        p["band"] = band;
        p["role_uid"] = uid;
        p["body"] = body;
        c.send("BAND_BROADCAST_SEND_NOTIFY", p);
    }
};

constexpr auto kNotifyBudget = std::chrono::milliseconds{2000};
// Short window used to prove a NOTIFY is ABSENT (self-exclusion / no
// cross-band leak).  A frame arriving inside this window fails the test;
// its absence after the window is the factual negative result.
constexpr auto kAbsenceBudget = std::chrono::milliseconds{400};

} // namespace

// ─── message fan-out: broadcast body delivered verbatim to a peer ──────────
TEST_F(Pattern4ChannelGroupTest, MessageFanout_BodyDeliveredToPeer)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid_a = "prod.sender" + suffix;
    const std::string uid_b = "cons.recvr" + suffix;
    const std::string band = "!msg_ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("channel_group_fanout");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    auto b = make_wire_client(ctx, setup, uid_b);

    ASSERT_TRUE(band_join(a, band, uid_a).has_value());
    ASSERT_TRUE(band_join(b, band, uid_b).has_value());

    band_broadcast(a, band, uid_a, {{"event", "hello"}, {"value", 42}});

    auto deliver = drain_for(b, "BAND_BROADCAST_DELIVER_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(deliver.has_value()) << "receiver never got BAND_BROADCAST_DELIVER_NOTIFY";
    const auto body = deliver->value("body", nlohmann::json::object());
    EXPECT_EQ(body.value("event", std::string{}), "hello");
    EXPECT_EQ(body.value("value", 0), 42);

    broker.signal_quit();
}

// ─── join notification: existing member learns of a newcomer ───────────────
TEST_F(Pattern4ChannelGroupTest, JoinNotification_ExistingMemberNotified)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid_a = "prod.first" + suffix;
    const std::string uid_b = "prod.second" + suffix;
    const std::string band = "!notify_ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("channel_group_joinnotify");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    auto b = make_wire_client(ctx, setup, uid_b);

    // A joins first, then B — A must receive BAND_JOIN_NOTIFY naming B.
    ASSERT_TRUE(band_join(a, band, uid_a).has_value());
    ASSERT_TRUE(band_join(b, band, uid_b).has_value());

    auto notify = drain_for(a, "BAND_JOIN_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(notify.has_value()) << "A never got BAND_JOIN_NOTIFY";
    // HEP-CORE-0030 §5.3: JOIN_NOTIFY carries {band, role_uid, role_name}.
    // Pin all three — role_uid alone would pass even if the broker tagged the
    // notify with the wrong band (a load-bearing routing field for members).
    EXPECT_EQ(notify->value("role_uid", std::string{}), uid_b)
        << "JOIN_NOTIFY must name the newcomer B";
    EXPECT_EQ(notify->value("band", std::string{}), band)
        << "JOIN_NOTIFY must carry the joined band";
    EXPECT_EQ(notify->value("role_name", std::string{}), uid_b)
        << "JOIN_NOTIFY must carry the newcomer's role_name";

    broker.signal_quit();
}

// ─── leave notification: remaining member learns of a departure ────────────
TEST_F(Pattern4ChannelGroupTest, LeaveNotification_RemainingMemberNotified)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid_a = "prod.stayer" + suffix;
    const std::string uid_b = "prod.leaver" + suffix;
    const std::string band = "!leave_ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("channel_group_leavenotify");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    auto b = make_wire_client(ctx, setup, uid_b);

    ASSERT_TRUE(band_join(a, band, uid_a).has_value());
    ASSERT_TRUE(band_join(b, band, uid_b).has_value());

    // Leaver B departs voluntarily.
    nlohmann::json lreq;
    lreq["band"] = band;
    lreq["role_uid"] = uid_b;
    auto leave =
        b.request("BAND_LEAVE_REQ", lreq, "BAND_LEAVE_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(leave.has_value() && leave->value("status", std::string{}) == "success");

    auto notify = drain_for(a, "BAND_LEAVE_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(notify.has_value()) << "stayer A never got BAND_LEAVE_NOTIFY";
    EXPECT_EQ(notify->value("role_uid", std::string{}), uid_b)
        << "LEAVE_NOTIFY must name the leaver B";
    EXPECT_EQ(notify->value("reason", std::string{}), "voluntary")
        << "voluntary BAND_LEAVE_REQ must report reason=voluntary";

    broker.signal_quit();
}

// ─── self-exclusion: broadcaster does not receive its own message ──────────
TEST_F(Pattern4ChannelGroupTest, Broadcast_SenderExcludedFromOwnMessage)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid_a = "prod.sender" + suffix;
    const std::string uid_b = "cons.recvr" + suffix;
    const std::string band = "!self_ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("channel_group_selfexcl");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    auto b = make_wire_client(ctx, setup, uid_b);

    ASSERT_TRUE(band_join(a, band, uid_a).has_value());
    ASSERT_TRUE(band_join(b, band, uid_b).has_value());

    band_broadcast(a, band, uid_a, {{"ping", true}});

    // Receiver B gets it (positive) ...
    auto at_b = drain_for(b, "BAND_BROADCAST_DELIVER_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(at_b.has_value()) << "receiver B did not get the broadcast";

    // ... and sender A does NOT (factual absence over a bounded window).
    auto at_a = drain_for(a, "BAND_BROADCAST_DELIVER_NOTIFY", kAbsenceBudget);
    EXPECT_FALSE(at_a.has_value())
        << "sender A received its own broadcast (self-exclusion violated)";

    broker.signal_quit();
}

// ─── multi-channel isolation: a broadcast reaches only its own band ────────
TEST_F(Pattern4ChannelGroupTest, MultiChannel_BroadcastStaysInItsBand)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid_a = "prod.alpha_only" + suffix;  // member of alpha ONLY
    const std::string uid_c = "prod.beta_only" + suffix;   // member of beta ONLY
    const std::string uid_b = "prod.broadcaster" + suffix; // member of BOTH
    const std::string alpha = "!ch_alpha" + suffix;
    const std::string beta = "!ch_beta" + suffix;

    const fs::path temp_dir = make_test_temp_dir("channel_group_multi");
    const auto setup = make_pattern4_setup({uid_a, uid_b, uid_c});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a); // alpha only
    auto c = make_wire_client(ctx, setup, uid_c); // beta only
    auto b = make_wire_client(ctx, setup, uid_b); // broadcaster, both bands

    ASSERT_TRUE(band_join(a, alpha, uid_a).has_value());
    ASSERT_TRUE(band_join(c, beta, uid_c).has_value());
    ASSERT_TRUE(band_join(b, alpha, uid_b).has_value());
    ASSERT_TRUE(band_join(b, beta, uid_b).has_value());

    // B broadcasts to alpha → the alpha member (A) receives it; the beta-only
    // member (C) must NOT.  The non-member's EXCLUSION is what proves band
    // isolation (HEP-CORE-0030 §5.1/§5.2 scoping) — the delivered `band` tag
    // alone is copied from the sender's request and cannot prove routing.
    band_broadcast(b, alpha, uid_b, {{"target", "alpha"}});
    auto at_a = drain_for(a, "BAND_BROADCAST_DELIVER_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(at_a.has_value()) << "alpha member A did not receive the alpha broadcast";
    EXPECT_EQ(at_a->value("band", std::string{}), alpha);
    auto leak_c = drain_for(c, "BAND_BROADCAST_DELIVER_NOTIFY", kAbsenceBudget);
    EXPECT_FALSE(leak_c.has_value())
        << "beta-only member C received an alpha broadcast — band isolation broken";

    // B broadcasts to beta → the beta member (C) receives it; the alpha-only
    // member (A) must NOT.
    band_broadcast(b, beta, uid_b, {{"target", "beta"}});
    auto at_c = drain_for(c, "BAND_BROADCAST_DELIVER_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(at_c.has_value()) << "beta member C did not receive the beta broadcast";
    EXPECT_EQ(at_c->value("band", std::string{}), beta);
    auto leak_a = drain_for(a, "BAND_BROADCAST_DELIVER_NOTIFY", kAbsenceBudget);
    EXPECT_FALSE(leak_a.has_value())
        << "alpha-only member A received a beta broadcast — band isolation broken";

    broker.signal_quit();
}

// ─── full band lifecycle over a single wire path ───────────────────────────
// (migrated from channel_group.roleapi_channel, which despite its name
// drove BrokerRequestComm directly — RoleHostCore was only a NOTIFY sink,
// not a role-side FSM under test.)
TEST_F(Pattern4ChannelGroupTest, FullLifecycle_JoinNotifyBroadcastMembersLeave)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid_a = "prod.role.a" + suffix;
    const std::string uid_b = "prod.role.b" + suffix;
    const std::string band = "!api_test_ch" + suffix;

    const fs::path temp_dir = make_test_temp_dir("channel_group_full");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    auto b = make_wire_client(ctx, setup, uid_b);

    ASSERT_TRUE(band_join(a, band, uid_a).has_value());
    ASSERT_TRUE(band_join(b, band, uid_b).has_value());

    // A learns of B's join.
    auto jn = drain_for(a, "BAND_JOIN_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(jn.has_value()) << "A never got BAND_JOIN_NOTIFY for B";

    // A broadcasts; B receives it.
    band_broadcast(a, band, uid_a, {{"action", "start"}, {"seq", 1}});
    auto deliver = drain_for(b, "BAND_BROADCAST_DELIVER_NOTIFY", kNotifyBudget);
    ASSERT_TRUE(deliver.has_value()) << "B never got the broadcast";
    EXPECT_EQ(deliver->value("body", nlohmann::json::object()).value("action", std::string{}),
              "start");

    // MEMBERS query shows both.
    nlohmann::json mreq;
    mreq["band"] = band;
    auto members = a.request("BAND_MEMBERS_REQ", mreq, "BAND_MEMBERS_ACK",
                             milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(members.has_value() && members->contains("members"));
    EXPECT_EQ((*members)["members"].size(), 2u);

    // A leaves.
    nlohmann::json lreq;
    lreq["band"] = band;
    lreq["role_uid"] = uid_a;
    auto leave =
        a.request("BAND_LEAVE_REQ", lreq, "BAND_LEAVE_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(leave.has_value() && leave->value("status", std::string{}) == "success");

    broker.signal_quit();
}
