/**
 * @file test_pattern4_broker_consumer.cpp
 * @brief Pattern 4 consumer-registration protocol wire tests.
 *
 * Successors of the wire-only workers formerly hosted under
 * `tests/test_layer3_datahub/workers/broker_consumer_workers.cpp` against
 * the retired in-process HubHostBrokerHandle harness (task #52 sweep).
 * Broker runs in its own subprocess (the generic
 * `pattern4_broker_protocol.broker`); the parent drives wire traffic via
 * BrokerWireClient using the shared Pattern4WireTest base.
 *
 * All 15 workers were pure wire-only (verification entirely on
 * CONSUMER_REG_ACK / DISC_ACK / CONSUMER_DEREG_ACK / GET_CHANNEL_AUTH_ACK
 * / CONSUMER_ATTACH_ACK_SHM bodies) — no in-process broker-state reads.
 */
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4BrokerConsumerTest : public pylabhub::tests::pattern4::Pattern4WireTest
{
  protected:
    using ms = std::chrono::milliseconds;

    std::optional<nlohmann::json> discover(BrokerWireClient &c, const std::string &channel)
    {
        nlohmann::json body;
        body["channel_name"] = channel;
        return c.request("DISC_REQ", body, "DISC_ACK", ms{pylabhub::kLongTimeoutMs});
    }

    std::optional<nlohmann::json> dereg_consumer(BrokerWireClient &c, const std::string &channel,
                                                 const std::string &uid)
    {
        nlohmann::json body;
        body["channel_name"] = channel;
        body["role_uid"] = uid;
        body["consumer_pid"] = pylabhub::platform::get_pid();
        return c.request("CONSUMER_DEREG_REQ", body, "CONSUMER_DEREG_ACK",
                         ms{pylabhub::kLongTimeoutMs});
    }

    std::optional<nlohmann::json> get_channel_auth(BrokerWireClient &c, const std::string &channel,
                                                   const std::string &role_uid)
    {
        nlohmann::json body;
        body["channel_name"] = channel;
        body["role_uid"] = role_uid;
        return c.request("GET_CHANNEL_AUTH_REQ", body, "GET_CHANNEL_AUTH_ACK",
                         ms{pylabhub::kLongTimeoutMs});
    }

    std::optional<nlohmann::json> consumer_attach(BrokerWireClient &c, const std::string &channel,
                                                  const std::string &consumer_pubkey,
                                                  const std::string &consumer_uid,
                                                  const std::string &producer_uid)
    {
        nlohmann::json body;
        body["channel_name"] = channel;
        body["consumer_pubkey"] = consumer_pubkey;
        body["consumer_role_uid"] = consumer_uid;
        body["role_uid"] = producer_uid; // producer_role_uid on wire
        return c.request("CONSUMER_ATTACH_REQ_SHM", body, "CONSUMER_ATTACH_ACK_SHM",
                         ms{pylabhub::kLongTimeoutMs});
    }

    /// CONSUMER_REG_REQ with an explicitly chosen body role_uid + pubkey
    /// (for the identity/pubkey-mismatch spoofing tests); returns the
    /// raw reply (which may be an ERROR body).
    std::optional<nlohmann::json> register_consumer_raw(BrokerWireClient &c,
                                                        const std::string &channel,
                                                        const std::string &body_role_uid,
                                                        const std::string &pubkey)
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel = channel;
        in.role_uid = body_role_uid;
        in.role_name = "test_consumer";
        in.role_type = "consumer";
        in.data_transport = "zmq";
        in.zmq_pubkey = pubkey;
        return c.request("CONSUMER_REG_REQ", pylabhub::hub::build_consumer_reg_payload(in),
                         "CONSUMER_REG_ACK", ms{pylabhub::kLongTimeoutMs});
    }

    /// Register a ZMQ producer with an explicit (known) data endpoint so
    /// CONSUMER_REG_ACK.producers[] can be pinned against it.
    void register_producer_zmq(BrokerWireClient &c,
                               const pylabhub::tests::pattern4::Pattern4Setup &setup,
                               const std::string &channel, const std::string &uid,
                               const std::string &endpoint)
    {
        auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
        body["zmq_node_endpoint"] = endpoint;
        auto reply = c.request("REG_REQ", body, "REG_ACK", ms{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "producer REG_REQ timed out";
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "producer REG_REQ failed; body=" << reply->dump();
    }
};

} // namespace

// Spawn helper shared by every test — inlined (WorkerProcess is non-movable).
#define SPAWN_BROKER(temp_dir)                                                                     \
    SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker", {(temp_dir).string(), "default"})

// ─── CONSUMER_REG / DEREG / DISC ───────────────────────────────────────────

TEST_F(Pattern4BrokerConsumerTest, ConsumerReg_NoOwnerYet_AwaitingOwner)
{
    // HEP-CORE-0017 §4.7.0.1 C2/C3 (owner-first establishment,
    // 2026-07-26; re-pinned from the retired CHANNEL_NOT_FOUND
    // contract): a consumer REG on a channel whose owner has not
    // registered is a DIALING-side early arrival — the broker replies
    // the retryable AWAITING_OWNER immediately (pure responder, no
    // pend), and the role host's retry loop re-attempts within its
    // init budget.
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "cons.unknown" + suffix;
    const std::string channel = "consumer.no_such_channel" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_reg_not_found");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto cons = make_wire_client(ctx, setup, uid);
    auto resp = cons.request("CONSUMER_REG_REQ", consumer_reg_body(setup, channel, uid),
                             "CONSUMER_REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "CONSUMER_REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "AWAITING_OWNER")
        << "body=" << resp->dump();

    broker.signal_quit();
}

// ─── SI-1/SI-2 owner-open validation (schema/metrics integration design,
//     ratified 2026-07-26): the fan-in OWNER must declare the channel
//     schema at open, and the open row validates what it installs. ─────

TEST_F(Pattern4BrokerConsumerTest, FanInOwnerOpen_NoSchema_Rejected)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "cons.blankowner" + suffix;
    const std::string channel = "consumer.blank_open" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_blank_open");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto cons = make_wire_client(ctx, setup, uid);
    // fan-in topology + NO citation: would open a blank contract that the
    // exact-equality matcher then holds against every producer.
    auto resp = cons.request("CONSUMER_REG_REQ", consumer_reg_body(setup, channel, uid, "fan-in"),
                             "CONSUMER_REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "CONSUMER_REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "SCHEMA_REQUIRED")
        << "body=" << resp->dump();
    // Side-effect check: the reject must leave NO book (a later owner
    // open must still be a fresh open).
    auto retry = cons.request("CONSUMER_REG_REQ", consumer_reg_body(setup, channel, uid, "fan-in"),
                              "CONSUMER_REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(retry->value("error_code", std::string{}), "SCHEMA_REQUIRED");

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, FanInOwnerOpen_InconsistentFingerprint_Rejected)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "cons.badfp" + suffix;
    const std::string channel = "consumer.bad_fp_open" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_bad_fp_open");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto cons = make_wire_client(ctx, setup, uid);
    auto body = consumer_reg_body(setup, channel, uid, "fan-in");
    // Structure + a syntactically valid but WRONG fingerprint (the G8
    // config-typo case): must be refused BEFORE the book opens, at the
    // owner — not later at every innocent producer.
    body["expected_schema_blds"] = "ts:f64:1:0";
    body["expected_schema_packing"] = "aligned";
    body["expected_schema_hash"] = std::string(128, 'a');
    auto resp = cons.request("CONSUMER_REG_REQ", body, "CONSUMER_REG_ACK",
                             milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value()) << "CONSUMER_REG_REQ timed out";
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "FINGERPRINT_INCONSISTENT")
        << "body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, FanInOwnerOpen_ValidCitation_ProducerJoinsByContract)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.owner_contract" + suffix;
    const std::string cons_uid = "cons.owner" + suffix;
    const std::string prod_ok = "prod.match" + suffix;
    const std::string prod_bad = "prod.drift" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_owner_contract");
    const auto setup = make_pattern4_setup({cons_uid, prod_ok, prod_bad});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    // Owner opens with the standard self-consistent test citation.
    auto owner = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_fanin_owner(owner, setup, channel, cons_uid));

    // Matching producer (same anonymous structure ⇒ same fingerprint,
    // computed with the SAME canonical helper) is admitted — SI-3 join
    // row against the owner's installed contract.
    const std::string blds = "ts:f64:1:0";
    const std::string packing = "aligned";
    const auto fp =
        pylabhub::hub::verify_request_fingerprint(blds, packing, "", "", /*claimed=*/"");
    const std::string good_hash = pylabhub::format_tools::bytes_to_hex(
        {reinterpret_cast<const char *>(fp.hash.data()), fp.hash.size()});
    auto ok = make_wire_client(ctx, setup, prod_ok);
    auto ok_body = producer_reg_body(setup, channel, prod_ok, /*shm=*/false, "fan-in");
    ok_body["schema_blds"] = blds;
    ok_body["schema_packing"] = packing;
    ok_body["schema_hash"] = good_hash;
    auto ok_resp = ok.request("REG_REQ", ok_body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(ok_resp.has_value());
    EXPECT_EQ(ok_resp->value("status", std::string{}), "success") << "body=" << ok_resp->dump();

    // Drifted producer (different structure, self-consistent material)
    // is rejected against the owner's contract with SCHEMA_MISMATCH —
    // the reject lands on the drifted party, channel untouched.
    const std::string blds2 = "ts:f64:1:0|extra:u32:1:0";
    const auto fp2 =
        pylabhub::hub::verify_request_fingerprint(blds2, packing, "", "", /*claimed=*/"");
    const std::string bad_hash = pylabhub::format_tools::bytes_to_hex(
        {reinterpret_cast<const char *>(fp2.hash.data()), fp2.hash.size()});
    auto bad = make_wire_client(ctx, setup, prod_bad);
    auto bad_body = producer_reg_body(setup, channel, prod_bad, /*shm=*/false, "fan-in");
    bad_body["schema_blds"] = blds2;
    bad_body["schema_packing"] = packing;
    bad_body["schema_hash"] = bad_hash;
    auto bad_resp =
        bad.request("REG_REQ", bad_body, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(bad_resp.has_value());
    EXPECT_EQ(bad_resp->value("status", std::string{}), "error");
    EXPECT_EQ(bad_resp->value("error_code", std::string{}), "SCHEMA_MISMATCH")
        << "body=" << bad_resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, FanInOwnerOpen_OwnerAxis_Rejections)
{
    // SI-9/G9 (ruled 2026-07-26): the open row validates the OWNER AXIS
    // it installs.  Three rejects, each BEFORE the book opens.
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "cons.owneraxis" + suffix;
    const std::string channel = "consumer.owner_axis" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_owner_axis");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto cons = make_wire_client(ctx, setup, uid);

    // (a) Owner claim without a schema_id → INVALID_REQUEST (an owner
    //     claim without a named schema is meaningless — previously
    //     silently ignored).
    auto body_a = consumer_reg_body(setup, channel, uid, "fan-in");
    pylabhub::tests::pattern4::apply_owner_citation(body_a); // structure, no id
    body_a["expected_schema_owner"] = "hub";
    auto ra = cons.request("CONSUMER_REG_REQ", body_a, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(ra.has_value());
    EXPECT_EQ(ra->value("error_code", std::string{}), "INVALID_REQUEST") << ra->dump();

    // (b) Named citation under a third party's namespace →
    //     SCHEMA_FORBIDDEN_OWNER (consumers may claim "hub" only —
    //     symmetric with the producer rule).
    auto body_b = consumer_reg_body(setup, channel, uid, "fan-in");
    body_b["expected_schema_id"] = "$lab.p4.axis.v1";
    body_b["expected_schema_hash"] = std::string(128, 'a');
    body_b["expected_schema_owner"] = "prod.other.uid00000042";
    auto rb = cons.request("CONSUMER_REG_REQ", body_b, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(rb.has_value());
    EXPECT_EQ(rb->value("error_code", std::string{}), "SCHEMA_FORBIDDEN_OWNER") << rb->dump();

    // (c) Named citation with NO owner → SCHEMA_OWNER_REQUIRED (G9: an
    //     unowned named book is unjoinable by every producer — the
    //     front door defaults a named producer citation's owner to
    //     self, and anonymous joiners fail the name axis).
    auto body_c = consumer_reg_body(setup, channel, uid, "fan-in");
    body_c["expected_schema_id"] = "$lab.p4.axis.v1";
    body_c["expected_schema_hash"] = std::string(128, 'a');
    auto rc = cons.request("CONSUMER_REG_REQ", body_c, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->value("error_code", std::string{}), "SCHEMA_OWNER_REQUIRED") << rc->dump();

    // Side-effect pin: none of the rejects opened a book — a valid
    // anonymous owner open on the SAME channel is still a fresh open.
    auto body_ok = consumer_reg_body(setup, channel, uid, "fan-in");
    pylabhub::tests::pattern4::apply_owner_citation(body_ok);
    auto rok = cons.request("CONSUMER_REG_REQ", body_ok, "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(rok.has_value());
    EXPECT_EQ(rok->value("status", std::string{}), "success") << rok->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, FanInOwnerOpen_HubGlobal_ResolvedAndServed)
{
    // SI-9/G9+G10 (ruled 2026-07-26): a NAMED fan-in open is a registry
    // citation — owner="hub" resolves through the single validator, and
    // the record's structure is MATERIALIZED into the channel record so
    // the channel form serves it.  The broker loads the hub-global from
    // <temp_dir>/schemas via the production `load_hub_globals_` walker
    // (worker profile "hub_globals").
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "cons.hubglobal" + suffix;
    const std::string channel = "consumer.hub_global_open" + suffix;
    const std::string sid = "$lab.p4.frame.v1";
    const std::string good_hash =
        pylabhub::tests::pattern4::test_schema_fingerprint_hex("v:float32:1:0", "aligned");

    const fs::path temp_dir = make_test_temp_dir("bc_hub_global_open");
    // Stage the hub-global fixture: <temp_dir>/schemas/lab/p4/frame.v1.json
    // (HEP-CORE-0034 §12 layout; id "$lab.p4.frame.v1" once loaded).
    const fs::path schema_dir = temp_dir / "schemas" / "lab" / "p4";
    fs::create_directories(schema_dir);
    {
        std::ofstream f(schema_dir / "frame.v1.json");
        f << R"({"id":"lab.p4.frame","version":1,)"
          << R"("slot":{"packing":"aligned","fields":[{"name":"v","type":"float32"}]}})";
    }
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "hub_globals"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto cons = make_wire_client(ctx, setup, uid);

    // (a) Unknown hub-global → SCHEMA_UNKNOWN (registry resolution).
    auto body_u = consumer_reg_body(setup, channel, uid, "fan-in");
    body_u["expected_schema_id"] = "$lab.p4.nosuch.v1";
    body_u["expected_schema_hash"] = std::string(128, 'a');
    body_u["expected_schema_owner"] = "hub";
    auto ru = cons.request("CONSUMER_REG_REQ", body_u, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(ru.has_value());
    EXPECT_EQ(ru->value("error_code", std::string{}), "SCHEMA_UNKNOWN") << ru->dump();

    // (b) Known hub-global, drifted fingerprint → FINGERPRINT_INCONSISTENT.
    auto body_d = consumer_reg_body(setup, channel, uid, "fan-in");
    body_d["expected_schema_id"] = sid;
    body_d["expected_schema_hash"] = std::string(128, 'a');
    body_d["expected_schema_owner"] = "hub";
    auto rd = cons.request("CONSUMER_REG_REQ", body_d, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(rd.has_value());
    EXPECT_EQ(rd->value("error_code", std::string{}), "FINGERPRINT_INCONSISTENT") << rd->dump();

    // (c) Correct named-no-structure open → success (the citation is
    //     resolved against the registry; the record's structure is
    //     materialized into the channel invariants).
    auto body_ok = consumer_reg_body(setup, channel, uid, "fan-in");
    body_ok["expected_schema_id"] = sid;
    body_ok["expected_schema_hash"] = good_hash;
    body_ok["expected_schema_owner"] = "hub";
    auto rok = cons.request("CONSUMER_REG_REQ", body_ok, "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(rok.has_value());
    EXPECT_EQ(rok->value("status", std::string{}), "success") << rok->dump();

    // (d) G10 pin: the channel form serves the MATERIALIZED structure
    //     to a member — blds came from the registry record, not the
    //     (structure-free) citation.
    nlohmann::json q;
    q["channel_name"] = channel;
    q["role_uid"] = uid;
    auto sq = cons.request("SCHEMA_REQ", q, "SCHEMA_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(sq.has_value());
    EXPECT_EQ(sq->value("status", std::string{}), "success") << sq->dump();
    EXPECT_EQ(sq->value("schema_id", std::string{}), sid);
    EXPECT_EQ(sq->value("schema_owner", std::string{}), "hub");
    EXPECT_EQ(sq->value("blds", std::string{}), "v:float32:1:0") << sq->dump();
    EXPECT_EQ(sq->value("schema_hash", std::string{}), good_hash);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, FanInHubGlobalChannel_ProducerAdoptJoins)
{
    // THE keystone composition of the SI-9/G9 ruling: a named fan-in
    // channel is joinable — and ONLY joinable — through hub-global
    // adoption.  Consumer opens on (hub, id); a path-C producer
    // (schema_owner="hub") with the matching material joins; a path-B
    // producer (no owner claim → front door defaults owner to SELF)
    // is rejected on the owner axis.
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "cons.hubjoin" + suffix;
    const std::string prod_c = "prod.adopt" + suffix;
    const std::string prod_b = "prod.selfown" + suffix;
    const std::string channel = "consumer.hub_global_join" + suffix;
    const std::string sid = "$lab.p4.frame.v1";
    const std::string blds = "v:float32:1:0";
    const std::string packing = "aligned";
    const std::string good_hash =
        pylabhub::tests::pattern4::test_schema_fingerprint_hex(blds, packing);

    const fs::path temp_dir = make_test_temp_dir("bc_hub_global_join");
    const fs::path schema_dir = temp_dir / "schemas" / "lab" / "p4";
    fs::create_directories(schema_dir);
    {
        std::ofstream f(schema_dir / "frame.v1.json");
        f << R"({"id":"lab.p4.frame","version":1,)"
          << R"("slot":{"packing":"aligned","fields":[{"name":"v","type":"float32"}]}})";
    }
    const auto setup = make_pattern4_setup({uid, prod_c, prod_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "hub_globals"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    // Owner opens named-no-structure on the hub-global.
    auto cons = make_wire_client(ctx, setup, uid);
    auto open = consumer_reg_body(setup, channel, uid, "fan-in");
    open["expected_schema_id"] = sid;
    open["expected_schema_hash"] = good_hash;
    open["expected_schema_owner"] = "hub";
    auto ro = cons.request("CONSUMER_REG_REQ", open, "CONSUMER_REG_ACK",
                           milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(ro.has_value());
    ASSERT_EQ(ro->value("status", std::string{}), "success") << ro->dump();

    // Path-C producer adopts (hub, id) and joins the owner's contract.
    auto adopter = make_wire_client(ctx, setup, prod_c);
    auto jc = producer_reg_body(setup, channel, prod_c, /*shm=*/false, "fan-in");
    jc["schema_id"] = sid;
    jc["schema_owner"] = "hub";
    jc["schema_blds"] = blds;
    jc["schema_packing"] = packing;
    jc["schema_hash"] = good_hash;
    auto rc = adopter.request("REG_REQ", jc, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(rc.has_value());
    EXPECT_EQ(rc->value("status", std::string{}), "success")
        << "path-C adoption must join the hub-owned fan-in channel; body=" << rc->dump();

    // Path-B producer: identical material but NO owner claim — the
    // front door cites owner=self, which cannot equal "hub" →
    // SCHEMA_MISMATCH on the owner axis (the G9 analysis pin).
    auto selfown = make_wire_client(ctx, setup, prod_b);
    auto jb = producer_reg_body(setup, channel, prod_b, /*shm=*/false, "fan-in");
    jb["schema_id"] = sid;
    jb["schema_blds"] = blds;
    jb["schema_packing"] = packing;
    jb["schema_hash"] = good_hash;
    auto rb = selfown.request("REG_REQ", jb, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(rb.has_value());
    EXPECT_EQ(rb->value("status", std::string{}), "error");
    EXPECT_EQ(rb->value("error_code", std::string{}), "SCHEMA_MISMATCH") << rb->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, FanInOwnerOpen_StructureWithoutHash_Rejected)
{
    // SI-2 (G8): the fan-in OPENER's citation carrying structure but no
    // fingerprint is rejected MISSING_HASH before the book opens — the
    // installed contract must carry its fingerprint.  (Producer-side
    // twin: AnonymousReg_StructureWithoutHash_Rejected.)
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string uid = "cons.nohash" + suffix;
    const std::string channel = "consumer.open_no_hash" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_open_no_hash");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto cons = make_wire_client(ctx, setup, uid);
    auto body = consumer_reg_body(setup, channel, uid, "fan-in");
    body["expected_schema_blds"] = "ts:f64:1:0";
    body["expected_schema_packing"] = "aligned";
    // no expected_schema_hash
    auto resp = cons.request("CONSUMER_REG_REQ", body, "CONSUMER_REG_ACK",
                             milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "MISSING_HASH") << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerReg_HappyPath)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.reg_happy" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_reg_happy");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto disc = discover(cons, channel);
    ASSERT_TRUE(disc.has_value()) << "DISC_REQ timed out";
    EXPECT_EQ(disc->value("status", std::string{}), "success");
    EXPECT_GE(disc->value("consumer_count", std::uint32_t{0}), 1u);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerDereg_HappyPath)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.dereg_happy" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_dereg_happy");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto disc1 = discover(cons, channel);
    ASSERT_TRUE(disc1.has_value());
    EXPECT_EQ(disc1->value("consumer_count", std::uint32_t{99}), 1u);

    auto dereg = dereg_consumer(cons, channel, cons_uid);
    ASSERT_TRUE(dereg.has_value()) << "CONSUMER_DEREG_REQ timed out";
    EXPECT_EQ(dereg->value("status", std::string{}), "success");

    auto disc2 = discover(cons, channel);
    ASSERT_TRUE(disc2.has_value());
    EXPECT_EQ(disc2->value("consumer_count", std::uint32_t{99}), 0u);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerDereg_PidMismatch)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.dereg_pid_mismatch" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_correct = "cons." + channel + ".correct";
    const std::string cons_wrong = "cons." + channel + ".wrong";

    const fs::path temp_dir = make_test_temp_dir("bc_dereg_mismatch");
    const auto setup = make_pattern4_setup({prod_uid, cons_correct, cons_wrong});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto correct = make_wire_client(ctx, setup, cons_correct);
    ASSERT_NO_FATAL_FAILURE(register_consumer(correct, setup, channel, cons_correct));

    // A different consumer uid (never registered on this channel) tries to
    // deregister → NOT_REGISTERED.
    auto wrong = make_wire_client(ctx, setup, cons_wrong);
    auto dereg = dereg_consumer(wrong, channel, cons_wrong);
    ASSERT_TRUE(dereg.has_value()) << "broker should respond, not time out";
    EXPECT_EQ(dereg->value("status", std::string{}), "error");
    EXPECT_EQ(dereg->value("error_code", std::string{}), "NOT_REGISTERED")
        << "body=" << dereg->dump();

    auto disc = discover(correct, channel);
    ASSERT_TRUE(disc.has_value());
    EXPECT_EQ(disc->value("consumer_count", std::uint32_t{0}), 1u);

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, Disc_ShowsConsumerCount)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.disc_count" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string obs_uid = "observer." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_disc_count");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid, obs_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto observer = make_wire_client(ctx, setup, obs_uid);
    auto disc0 = discover(observer, channel);
    ASSERT_TRUE(disc0.has_value());
    EXPECT_EQ(disc0->value("consumer_count", std::uint32_t{99}), 0u);

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto disc1 = discover(observer, channel);
    ASSERT_TRUE(disc1.has_value());
    EXPECT_EQ(disc1->value("consumer_count", std::uint32_t{99}), 1u);

    auto dereg = dereg_consumer(cons, channel, cons_uid);
    ASSERT_TRUE(dereg.has_value());
    EXPECT_EQ(dereg->value("status", std::string{}), "success");

    auto disc2 = discover(observer, channel);
    ASSERT_TRUE(disc2.has_value());
    EXPECT_EQ(disc2->value("consumer_count", std::uint32_t{99}), 0u);

    broker.signal_quit();
}

// ─── CONSUMER_REG admission-gate rejections (I-DEALER-IDENTITY, pubkey) ─────

TEST_F(Pattern4BrokerConsumerTest, ConsumerReg_UnknownRole_IdentityMismatch)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.reg_unknown_role" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string real_uid = "cons.real." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_unknown_role");
    const auto setup = make_pattern4_setup({prod_uid, real_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    // Client connects (Frame 0 routing_id) as real_uid, but the body claims
    // a fabricated uid + real_uid's pubkey.  §14.5 gate order runs
    // I-DEALER-IDENTITY first → IDENTITY_MISMATCH (shadows UNKNOWN_ROLE).
    auto client = make_wire_client(ctx, setup, real_uid);
    const std::string fake_uid = "cons.fabricated.unregistered_" + channel;
    auto resp =
        register_consumer_raw(client, channel, fake_uid, setup.curve.role(real_uid).public_z85);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "IDENTITY_MISMATCH")
        << "body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerReg_PubkeyMismatch)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.reg_pubkey_mismatch" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string real_uid_a = "cons.real_a." + channel;
    const std::string real_uid_b = "cons.real_b." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_pubkey_mismatch");
    const auto setup = make_pattern4_setup({prod_uid, real_uid_a, real_uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    // Client connects as real_uid_a; body claims role_uid=real_uid_a (matches
    // Frame 0, passes identity gate) but attaches real_uid_b's pubkey →
    // PUBKEY_MISMATCH (HEP-CORE-0036 §6.3 Layer-2 step 2).
    auto client = make_wire_client(ctx, setup, real_uid_a);
    auto resp =
        register_consumer_raw(client, channel, real_uid_a, setup.curve.role(real_uid_b).public_z85);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "PUBKEY_MISMATCH")
        << "body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerRegAck_EmitsProducersZmq)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "consumer.reg_ack_producers_zmq" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string prod_endpoint = "tcp://127.0.0.1:55557";

    const fs::path temp_dir = make_test_temp_dir("bc_producers_zmq");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_zmq(prod, setup, channel, prod_uid, prod_endpoint));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    nlohmann::json creg;
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid, &creg));

    // HEP-CORE-0036 §6.4 — CONSUMER_REG_ACK carries producers[] with
    // {role_uid, pubkey_z85, endpoint}; single-producer → length 1.
    ASSERT_TRUE(creg.contains("producers"));
    const auto &producers = creg.at("producers");
    ASSERT_TRUE(producers.is_array());
    ASSERT_EQ(producers.size(), 1u);
    EXPECT_EQ(producers[0].value("role_uid", std::string{}), prod_uid);
    EXPECT_EQ(producers[0].value("pubkey_z85", std::string{}),
              setup.curve.role(prod_uid).public_z85);
    EXPECT_EQ(producers[0].value("endpoint", std::string{}), prod_endpoint);
    EXPECT_EQ(creg.value("data_transport", std::string{}), "zmq");
    // B-4 mutation pins: flat/legacy fields must be absent.
    EXPECT_FALSE(creg.contains("shm_capability_endpoint"));
    EXPECT_FALSE(creg.contains("producer_pubkey_z85"));
    EXPECT_FALSE(producers[0].contains("pubkey"));

    broker.signal_quit();
}

// ─── GET_CHANNEL_AUTH_REQ (allowlist pull) ─────────────────────────────────

TEST_F(Pattern4BrokerConsumerTest, GetChannelAuth_ReturnsAllowlist)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "auth.get_returns_allowlist" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_get_auth");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    // Pre-registration: allowlist empty.
    auto pre = get_channel_auth(prod, channel, prod_uid);
    ASSERT_TRUE(pre.has_value());
    EXPECT_EQ(pre->value("status", std::string{}), "success");
    ASSERT_TRUE(pre->contains("allowlist") && pre->at("allowlist").is_array());
    EXPECT_EQ(pre->at("allowlist").size(), 0u);

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    // Post-registration: allowlist = one `{role_uid, pubkey_z85}` row for
    // the consumer.  Rows, not bare strings, per HEP-CORE-0036 §6.5 — the
    // same shape `REG_ACK.initial_allowlist` carries, so a role's view
    // does not change meaning between the seed and a refresh.
    //
    // Both halves are pinned.  Checking only the key would still pass with
    // a nameless row, which is exactly the state this shape replaced: the
    // receiver could enforce on the key but could not tell its script who
    // the peer was.
    auto post = get_channel_auth(prod, channel, prod_uid);
    ASSERT_TRUE(post.has_value());
    EXPECT_EQ(post->value("status", std::string{}), "success");
    const auto &al = post->at("allowlist");
    ASSERT_TRUE(al.is_array());
    ASSERT_EQ(al.size(), 1u);
    ASSERT_TRUE(al[0].is_object()) << "allowlist rows are objects, not bare Z85 strings";
    EXPECT_EQ(al[0].value("pubkey_z85", std::string{}), setup.curve.role(cons_uid).public_z85);
    EXPECT_EQ(al[0].value("role_uid", std::string{}), cons_uid)
        << "the broker names the peer it admitted; a blank name here means the row was "
           "built from the ledger without asking the roster";

    // Consumer dereg → allowlist empty again.
    auto dereg = dereg_consumer(cons, channel, cons_uid);
    ASSERT_TRUE(dereg.has_value());
    EXPECT_EQ(dereg->value("status", std::string{}), "success");

    auto after = get_channel_auth(prod, channel, prod_uid);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->value("status", std::string{}), "success");
    EXPECT_EQ(after->at("allowlist").size(), 0u) << "allowlist must be empty after consumer dereg";

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, GetChannelAuth_RejectsNonProducer)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "auth.get_rejects_non_prod" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string other_uid = "cons.other." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_get_auth_reject");
    const auto setup = make_pattern4_setup({prod_uid, other_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    // A non-producer role must not be able to pull the allowlist.
    auto other = make_wire_client(ctx, setup, other_uid);
    auto resp = get_channel_auth(other, channel, other_uid);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "PRODUCER_NOT_AUTHORIZED")
        << "body=" << resp->dump();

    broker.signal_quit();
}

// ─── CONSUMER_ATTACH_REQ_SHM (pre-attach broker confirmation) ──────────────

TEST_F(Pattern4BrokerConsumerTest, ConsumerAttach_Authorized)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "attach.authorized" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_attach_ok");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));

    auto resp =
        consumer_attach(prod, channel, setup.curve.role(cons_uid).public_z85, cons_uid, prod_uid);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "success")
        << "registered consumer must be confirmed; body=" << resp->dump();
    EXPECT_EQ(resp->value("channel_name", std::string{}), channel);
    EXPECT_EQ(resp->value("consumer_pubkey", std::string{}), setup.curve.role(cons_uid).public_z85);
    EXPECT_FALSE(resp->contains("denial_reason"));

    // REVIEW-D (#277): pin broker-side STATE, not just the wire reply — the
    // admitted consumer must actually appear in the channel's allowlist ledger.
    auto auth = get_channel_auth(prod, channel, prod_uid);
    ASSERT_TRUE(auth.has_value());
    ASSERT_TRUE(auth->contains("allowlist") && auth->at("allowlist").is_array());
    // Rows carry `{role_uid, pubkey_z85}` (HEP-CORE-0036 §6.5).  Matching
    // on BOTH halves is the point: the key proves the ledger admitted it,
    // the name proves the broker resolved that key back to the role it
    // belongs to rather than emitting an anonymous entry.
    bool admitted_in_ledger = false;
    for (const auto &e : auth->at("allowlist"))
        if (e.is_object() &&
            e.value("pubkey_z85", std::string{}) == setup.curve.role(cons_uid).public_z85 &&
            e.value("role_uid", std::string{}) == cons_uid)
            admitted_in_ledger = true;
    EXPECT_TRUE(admitted_in_ledger)
        << "success reply must reflect ledger admission as a NAMED row; body=" << auth->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerAttach_Denied)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "attach.denied" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string fake_cons = "cons.unregistered." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_attach_denied");
    const auto setup = make_pattern4_setup({prod_uid, fake_cons});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    // fake_cons has a well-formed pubkey but never registered → not in the
    // channel's admission ledger → clean "denied" (not an ERROR frame).
    const std::string fake_pubkey = setup.curve.role(fake_cons).public_z85;
    auto resp = consumer_attach(prod, channel, fake_pubkey, fake_cons, prod_uid);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "denied") << "body=" << resp->dump();
    EXPECT_EQ(resp->value("channel_name", std::string{}), channel);
    EXPECT_EQ(resp->value("consumer_pubkey", std::string{}), fake_pubkey);
    EXPECT_TRUE(resp->contains("denial_reason"));

    // REVIEW-D (#277): denial reflects ledger state — the unregistered pubkey
    // is absent from the allowlist (empty, as fake_cons never registered).
    auto auth = get_channel_auth(prod, channel, prod_uid);
    ASSERT_TRUE(auth.has_value());
    ASSERT_TRUE(auth->contains("allowlist") && auth->at("allowlist").is_array());
    for (const auto &e : auth->at("allowlist"))
        EXPECT_FALSE(e.is_string() && e.get<std::string>() == fake_pubkey)
            << "denied pubkey must not appear in allowlist; body=" << auth->dump();

    broker.signal_quit();
}

// REVIEW-D (#277): the revoke → DENY entry-gate transition.  A consumer that
// was admitted (attach pre-confirm succeeds) and then deregistered must have
// its NEXT attach pre-confirm DENIED — `handle_consumer_dereg_req` calls
// `_on_channel_peer_revoked` → `ledger.revoke`, and the attach gate reads the same
// ledger via `admission_version_of`.  `GetChannelAuth_ReturnsAllowlist` proves
// the allowlist empties on dereg; THIS proves the gate then refuses a fresh
// attach.  Deterministic, no data plane: REG → attach(success) → dereg →
// attach(denied).
TEST_F(Pattern4BrokerConsumerTest, ConsumerAttach_DeniedAfterDereg)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "attach.denied_after_dereg" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_attach_revoke");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));
    const std::string cons_pubkey = setup.curve.role(cons_uid).public_z85;

    // Admitted: attach pre-confirm succeeds.
    auto admitted = consumer_attach(prod, channel, cons_pubkey, cons_uid, prod_uid);
    ASSERT_TRUE(admitted.has_value());
    EXPECT_EQ(admitted->value("status", std::string{}), "success")
        << "registered consumer must attach; body=" << admitted->dump();

    // Revoke: the consumer deregisters.
    auto dereg = dereg_consumer(cons, channel, cons_uid);
    ASSERT_TRUE(dereg.has_value());
    EXPECT_EQ(dereg->value("status", std::string{}), "success");

    // Deny: the SAME attach pre-confirm is now refused — ledger.revoke removed
    // the pubkey and the gate reads the same ledger.
    auto denied = consumer_attach(prod, channel, cons_pubkey, cons_uid, prod_uid);
    ASSERT_TRUE(denied.has_value());
    EXPECT_EQ(denied->value("status", std::string{}), "denied")
        << "revoked consumer's fresh attach MUST be denied; body=" << denied->dump();
    EXPECT_EQ(denied->value("consumer_pubkey", std::string{}), cons_pubkey);
    EXPECT_TRUE(denied->contains("denial_reason"));

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerAttach_ChannelNotFound)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string prod_uid = "prod.attach.no_channel" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_attach_no_chan");
    const auto setup = make_pattern4_setup({prod_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    auto resp = consumer_attach(prod, "nonexistent.channel" + suffix,
                                setup.curve.role(prod_uid).public_z85, prod_uid, prod_uid);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "CHANNEL_NOT_FOUND")
        << "body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerAttach_NonProducer)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "attach.non_prod" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string other_uid = "cons.other." + channel;

    const fs::path temp_dir = make_test_temp_dir("bc_attach_non_prod");
    const auto setup = make_pattern4_setup({prod_uid, other_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    // A role that is not a producer of the channel must be refused.
    auto other = make_wire_client(ctx, setup, other_uid);
    auto resp = consumer_attach(other, channel, setup.curve.role(other_uid).public_z85, other_uid,
                                other_uid);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "PRODUCER_NOT_AUTHORIZED")
        << "body=" << resp->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerConsumerTest, ConsumerAttach_InvalidRequest)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string prod_uid = "prod.attach.invalid" + suffix;

    const fs::path temp_dir = make_test_temp_dir("bc_attach_invalid");
    const auto setup = make_pattern4_setup({prod_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    // Empty consumer_pubkey → shape validation rejects before channel lookup.
    auto resp =
        consumer_attach(prod, "any.channel" + suffix, /*pubkey=*/"", "any.consumer.uid", prod_uid);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST")
        << "body=" << resp->dump();

    broker.signal_quit();
}
