/**
 * @file test_pattern4_broker_schema.cpp
 * @brief Pattern 4 broker named-schema protocol wire tests
 *        (HEP-CORE-0034 path B).
 *
 * Successors of the wire-only workers formerly hosted under
 * `tests/test_layer3_datahub/workers/broker_schema_workers.cpp` against
 * the retired in-process HubHostBrokerHandle harness (task #52 sweep).
 * Broker runs in its own subprocess (the generic
 * `pattern4_broker_protocol.broker`); the parent drives wire traffic via
 * BrokerWireClient using the shared Pattern4WireTest base.
 *
 * schema_hash_stored_on_reg stays in L3 for Round 3 — the broker's stored
 * schema_hash is not exposed on any wire ACK (CHANNEL_LIST_ACK carries
 * schema_id but not schema_hash), so verifying it requires the in-process
 * channel snapshot.
 */
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"

#include "utils/format_tools.hpp"
#include "utils/schema_utils.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

// Canonical schema for the named-citation tests.  When schema_blds is
// present the broker recomputes the BLAKE2b-256 canonical hash and
// rejects a mismatch (FINGERPRINT_INCONSISTENT, HEP-CORE-0034 §6.3), so
// schema_hash MUST be the real canonical value — the same production
// helper the role side uses.
constexpr const char *kSchemaBlds    = "ts:f64:1:0|value:f32:1:0";
constexpr const char *kSchemaPacking = "aligned";

std::string canonical_hash_hex(const std::string &blds,
                               const std::string &packing)
{
    const auto h = pylabhub::hub::compute_canonical_hash_from_wire(blds, packing);
    return pylabhub::format_tools::bytes_to_hex(
        {reinterpret_cast<const char *>(h.data()), h.size()});
}

class Pattern4BrokerSchemaTest : public pylabhub::tests::pattern4::Pattern4WireTest
{
protected:
    /// Register a producer whose REG_REQ carries a named-schema citation
    /// (schema_id + schema_hash + schema_blds + schema_packing layered on
    /// the base producer body).  Returns the REG_ACK reply.
    void register_producer_with_schema(
        BrokerWireClient                               &client,
        const pylabhub::tests::pattern4::Pattern4Setup &setup,
        const std::string                              &channel,
        const std::string                              &uid,
        const std::string                              &schema_id,
        const std::string                              &schema_hash,
        nlohmann::json                                 *out_ack = nullptr)
    {
        auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
        body["schema_id"]      = schema_id;
        body["schema_hash"]    = schema_hash;
        body["schema_blds"]    = kSchemaBlds;
        body["schema_packing"] = kSchemaPacking;
        auto reply = client.request(
            "REG_REQ", body, "REG_ACK",
            std::chrono::milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "REG_REQ timed out for " << uid;
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "REG_REQ (schema) failed for " << uid
            << "; body=" << reply->dump();
        if (out_ack != nullptr) *out_ack = *reply;
    }
};

}  // namespace

// ─── schema_id stored on REG — observable via CHANNEL_LIST_ACK ─────────────
//
// Reframe of the retired in-process schema_id_stored_on_reg: the broker
// records the producer's schema_id and echoes it back per-channel in
// CHANNEL_LIST_ACK (broker_service.cpp handle_channel_list_req).  This
// pins the stored value directly (stronger than the original, which only
// checked the channel's presence in the list).

TEST_F(Pattern4BrokerSchemaTest, SchemaIdStoredOnReg)
{
    using namespace std::chrono;
    const std::string suffix    = ".pid" + std::to_string(::getpid());
    const std::string channel   = "schema.id.stored" + suffix;
    const std::string uid       = "prod." + channel;
    const std::string schema_id = "$lab.test.sensor.v1";

    const fs::path temp_dir = make_test_temp_dir("broker_schema_id_stored");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal(
        "pattern4_broker_protocol.broker", {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_with_schema(
        prod, setup, channel, uid, schema_id, canonical_hash_hex(kSchemaBlds, kSchemaPacking)));

    auto list = prod.request("CHANNEL_LIST_REQ", nlohmann::json::object(),
                              "CHANNEL_LIST_ACK",
                              milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(list.has_value()) << "CHANNEL_LIST_REQ timed out";
    ASSERT_TRUE(list->contains("channels") && list->at("channels").is_array());
    bool found = false;
    for (const auto &ch : list->at("channels"))
    {
        if (ch.value("name", std::string{}) == channel)
        {
            found = true;
            EXPECT_EQ(ch.value("schema_id", std::string{}), schema_id)
                << "CHANNEL_LIST_ACK must echo the stored schema_id; entry="
                << ch.dump();
            break;
        }
    }
    EXPECT_TRUE(found) << "registered channel absent from CHANNEL_LIST_ACK; "
                          "body=" << list->dump();

    broker.signal_quit();
}

// ─── Consumer named-schema citation match / mismatch (HEP-CORE-0034 §10.3) ─

TEST_F(Pattern4BrokerSchemaTest, ConsumerSchemaIdMatch_Succeeds)
{
    using namespace std::chrono;
    const std::string suffix    = ".pid" + std::to_string(::getpid());
    const std::string channel   = "schema.consumer.match" + suffix;
    const std::string prod_uid  = "prod." + channel;
    const std::string cons_uid  = "cons." + channel;
    const std::string schema_id = "$lab.consumer.test.v2";
    const std::string hash = canonical_hash_hex(kSchemaBlds, kSchemaPacking);

    const fs::path temp_dir = make_test_temp_dir("broker_schema_match");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal(
        "pattern4_broker_protocol.broker", {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_with_schema(
        prod, setup, channel, prod_uid, schema_id, hash));
    // Channel-Ready precondition (R6 gate): the broker reaches the
    // CONSUMER_REG schema-match check only after the channel is Ready.
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons  = make_wire_client(ctx, setup, cons_uid);
    auto cbody = consumer_reg_body(setup, channel, cons_uid);
    cbody["expected_schema_id"]   = schema_id;
    cbody["expected_schema_hash"] = hash;
    auto cr = cons.request("CONSUMER_REG_REQ", cbody, "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value()) << "CONSUMER_REG_REQ timed out";
    EXPECT_EQ(cr->value("status", std::string{}), "success")
        << "consumer should succeed when schema_id matches; body=" << cr->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerSchemaTest, ConsumerSchemaIdMismatch_Fails)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "schema.consumer.mismatch" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string prod_sid = "$lab.producer.schema.v1";
    const std::string cons_sid = "$lab.other.schema.v1";
    const std::string hash = canonical_hash_hex(kSchemaBlds, kSchemaPacking);

    const fs::path temp_dir = make_test_temp_dir("broker_schema_mismatch");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal(
        "pattern4_broker_protocol.broker", {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_with_schema(
        prod, setup, channel, prod_uid, prod_sid, hash));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons  = make_wire_client(ctx, setup, cons_uid);
    auto cbody = consumer_reg_body(setup, channel, cons_uid);
    cbody["expected_schema_id"]   = cons_sid;
    cbody["expected_schema_hash"] = hash;
    auto cr = cons.request("CONSUMER_REG_REQ", cbody, "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value())
        << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(cr->value("status", std::string{}), "error");
    EXPECT_EQ(cr->value("error_code", std::string{}), "SCHEMA_ID_MISMATCH")
        << "body=" << cr->dump();

    broker.signal_quit();
}

TEST_F(Pattern4BrokerSchemaTest, ConsumerSchemaIdEmptyProducer_Fails)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "schema.consumer.empty.prod" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string cons_sid = "$lab.expected.schema.v3";

    const fs::path temp_dir = make_test_temp_dir("broker_schema_empty");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal(
        "pattern4_broker_protocol.broker", {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    // Producer registers WITHOUT any schema citation.
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons  = make_wire_client(ctx, setup, cons_uid);
    auto cbody = consumer_reg_body(setup, channel, cons_uid);
    cbody["expected_schema_id"] = cons_sid;  // named citation, no hash
    auto cr = cons.request("CONSUMER_REG_REQ", cbody, "CONSUMER_REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(cr.has_value())
        << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(cr->value("status", std::string{}), "error");
    EXPECT_EQ(cr->value("error_code", std::string{}),
              "MISSING_HASH_FOR_NAMED_CITATION")
        << "body=" << cr->dump();

    broker.signal_quit();
}

// schema_hash stored on REG — observed via the broker's own trace.  The
// broker echoes the stored schema_hash on its RegReqAccepted log line
// (broker_service.cpp), so the parent reads that instead of the retired
// in-process channel snapshot.  A bare schema_hash (no schema_blds) is
// stored opaquely — no canonical recomputation — so an arbitrary 64-hex
// value round-trips.
TEST_F(Pattern4BrokerSchemaTest, SchemaHashStoredOnReg)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "schema.hash.stored" + suffix;
    const std::string uid     = "prod." + channel;
    const std::string hash(64, 'a');

    const fs::path temp_dir = make_test_temp_dir("broker_schema_hash_stored");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal(
        "pattern4_broker_protocol.broker", {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    auto body = producer_reg_body(setup, channel, uid, /*shm=*/true);
    body["schema_hash"] = hash;  // no schema_blds → stored opaque
    auto reg = prod.request("REG_REQ", body, "REG_ACK",
                             milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg.has_value()) << "REG_REQ timed out";
    ASSERT_EQ(reg->value("status", std::string{}), "success")
        << "body=" << reg->dump();

    // The broker's RegReqAccepted trace echoes the stored schema_hash.
    expect_log(broker, "event=RegReqAccepted",
                milliseconds{pylabhub::kMidTimeoutMs});
    expect_log(broker, "schema_hash='" + hash + "'",
                milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}
