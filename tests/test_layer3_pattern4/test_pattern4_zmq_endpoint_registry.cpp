/**
 * @file test_pattern4_zmq_endpoint_registry.cpp
 * @brief Pattern 4 broker ZMQ endpoint-registry wire tests (HEP-CORE-0021).
 *
 * Successors of the wire-only workers formerly hosted under
 * `tests/test_layer3_datahub/workers/zmq_endpoint_registry_workers.cpp`
 * against the retired in-process HubHostBrokerHandle harness (task #52
 * sweep).  Broker runs in its own subprocess (the generic
 * `pattern4_broker_protocol.broker`); the parent drives wire traffic via
 * BrokerWireClient using the shared Pattern4WireTest base.
 *
 * Two reframes off in-process reads:
 *   - shm_and_zmq_coexist: `query_channel_snapshot()` → CHANNEL_LIST_REQ
 *     (which enumerates registered channels on the wire).
 *   - req_shape...fire_and_forget: the BRC `unmatched_replies()` counter
 *     → a direct wire drain asserting no `*_ACK`/`ERROR` reply arrives for
 *     the fire-and-forget REQs (NOTIFY fan-out is allowed).
 */
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"

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

class Pattern4ZmqEndpointRegistryTest
    : public pylabhub::tests::pattern4::Pattern4WireTest
{
protected:
    using ms = std::chrono::milliseconds;

    void register_producer_shm(BrokerWireClient    &c,
                               const pylabhub::tests::pattern4::Pattern4Setup &setup,
                               const std::string   &channel,
                               const std::string   &uid)
    {
        auto reply = c.request("REG_REQ",
                               producer_reg_body(setup, channel, uid, /*shm=*/true),
                               "REG_ACK", ms{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "SHM producer REG_REQ timed out";
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "body=" << reply->dump();
    }

    void register_producer_zmq_ep(BrokerWireClient    &c,
                                  const pylabhub::tests::pattern4::Pattern4Setup &setup,
                                  const std::string   &channel,
                                  const std::string   &uid,
                                  const std::string   &endpoint)
    {
        auto body = producer_reg_body(setup, channel, uid, /*shm=*/false);
        body["zmq_node_endpoint"] = endpoint;
        auto reply = c.request("REG_REQ", body, "REG_ACK",
                               ms{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "ZMQ producer REG_REQ timed out";
        ASSERT_EQ(reply->value("status", std::string{}), "success")
            << "body=" << reply->dump();
    }

    std::optional<nlohmann::json> discover(
        BrokerWireClient  &c,
        const std::string &channel,
        int                timeout_ms = pylabhub::kLongTimeoutMs)
    {
        nlohmann::json body;
        body["channel_name"] = channel;
        return c.request("DISC_REQ", body, "DISC_ACK", ms{timeout_ms});
    }

    std::optional<nlohmann::json> endpoint_update(BrokerWireClient  &c,
                                                  const std::string &channel,
                                                  const std::string &type,
                                                  const std::string &endpoint)
    {
        nlohmann::json body;
        body["channel_name"]  = channel;
        body["endpoint_type"] = type;
        body["endpoint"]      = endpoint;
        return c.request("ENDPOINT_UPDATE_REQ", body, "ENDPOINT_UPDATE_ACK",
                         ms{pylabhub::kMidTimeoutMs});
    }
};

}  // namespace

#define SPAWN_BROKER(temp_dir)                                                  \
    SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",               \
                              {(temp_dir).string(), "default"})

TEST_F(Pattern4ZmqEndpointRegistryTest, DefaultTransportIsShm)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "zmqvc.default.shm" + suffix;
    const std::string uid      = "prod.ep." + channel;
    const std::string cons_uid = "cons.ep." + channel;

    const fs::path temp_dir = make_test_temp_dir("zep_default_shm");
    const auto     setup    = make_pattern4_setup({uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_shm(prod, setup, channel, uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    auto disc = discover(cons, channel);
    ASSERT_TRUE(disc.has_value()) << "DISC_REQ timed out";
    EXPECT_EQ(disc->value("data_transport", std::string{}), "shm");

    broker.signal_quit();
}

TEST_F(Pattern4ZmqEndpointRegistryTest, ZmqTransportRoundTrip)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "zmqvc.zmq.roundtrip" + suffix;
    const std::string uid      = "prod.ep." + channel;
    const std::string cons_uid = "cons.ep." + channel;
    const std::string zmq_ep   = "tcp://127.0.0.1:55555";

    const fs::path temp_dir = make_test_temp_dir("zep_zmq_rt");
    const auto     setup    = make_pattern4_setup({uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer_zmq_ep(prod, setup, channel, uid, zmq_ep));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    auto disc = discover(cons, channel);
    ASSERT_TRUE(disc.has_value()) << "DISC_REQ timed out";
    EXPECT_EQ(disc->value("data_transport", std::string{}), "zmq");
    EXPECT_EQ(disc->value("zmq_node_endpoint", std::string{}), zmq_ep);

    broker.signal_quit();
}

TEST_F(Pattern4ZmqEndpointRegistryTest, MultipleConsumersDiscoverSameEndpoint)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "zmqvc.multi.disc" + suffix;
    const std::string uid     = "prod.ep." + channel;
    const std::string c1_uid  = "CONS1-" + channel;
    const std::string c2_uid  = "CONS2-" + channel;
    const std::string zmq_ep  = "tcp://127.0.0.1:55556";

    const fs::path temp_dir = make_test_temp_dir("zep_multi_disc");
    const auto     setup    = make_pattern4_setup({uid, c1_uid, c2_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer_zmq_ep(prod, setup, channel, uid, zmq_ep));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    auto c1 = make_wire_client(ctx, setup, c1_uid);
    auto c2 = make_wire_client(ctx, setup, c2_uid);
    auto d1 = discover(c1, channel);
    auto d2 = discover(c2, channel);
    ASSERT_TRUE(d1.has_value());
    ASSERT_TRUE(d2.has_value());
    EXPECT_EQ(d1->value("zmq_node_endpoint", std::string{}), zmq_ep);
    EXPECT_EQ(d2->value("zmq_node_endpoint", std::string{}), zmq_ep);

    broker.signal_quit();
}

// Reframe: query_channel_snapshot() → CHANNEL_LIST_REQ (enumerates
// registered channels on the wire).
TEST_F(Pattern4ZmqEndpointRegistryTest, ShmAndZmqCoexist)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string shm_ch  = "zmqvc.coexist.shm" + suffix;
    const std::string zmq_ch  = "zmqvc.coexist.zmq" + suffix;
    const std::string shm_uid = "prod.shm." + shm_ch;
    const std::string zmq_uid = "prod." + zmq_ch;

    const fs::path temp_dir = make_test_temp_dir("zep_coexist");
    const auto     setup    = make_pattern4_setup({shm_uid, zmq_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           shm_prod = make_wire_client(ctx, setup, shm_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_shm(shm_prod, setup, shm_ch, shm_uid));

    auto zmq_prod = make_wire_client(ctx, setup, zmq_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_zmq_ep(
        zmq_prod, setup, zmq_ch, zmq_uid, "tcp://127.0.0.1:55557"));

    auto list = shm_prod.request("CHANNEL_LIST_REQ", nlohmann::json::object(),
                                  "CHANNEL_LIST_ACK",
                                  milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(list.has_value()) << "CHANNEL_LIST_REQ timed out";
    ASSERT_TRUE(list->contains("channels") && list->at("channels").is_array());
    bool found_shm = false, found_zmq = false;
    for (const auto &ch : list->at("channels"))
    {
        const auto name = ch.value("name", std::string{});
        if (name == shm_ch) found_shm = true;
        if (name == zmq_ch) found_zmq = true;
    }
    EXPECT_TRUE(found_shm) << "SHM channel absent from CHANNEL_LIST_ACK";
    EXPECT_TRUE(found_zmq) << "ZMQ channel absent from CHANNEL_LIST_ACK";

    broker.signal_quit();
}

TEST_F(Pattern4ZmqEndpointRegistryTest, EndpointUpdateReflectedInDiscovery)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "zmqvc.ep.update" + suffix;
    const std::string uid      = "prod.ep." + channel;
    const std::string cons_uid = "cons.ep." + channel;
    const std::string updated_ep = "tcp://127.0.0.1:44444";

    const fs::path temp_dir = make_test_temp_dir("zep_update");
    const auto     setup    = make_pattern4_setup({uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer_zmq_ep(prod, setup, channel, uid, "tcp://127.0.0.1:0"));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    // ENDPOINT_UPDATE_REQ is sync; the ACK is a durability barrier, so a
    // subsequent DISC is guaranteed to observe the updated endpoint.
    auto upd = endpoint_update(prod, channel, "zmq_node", updated_ep);
    ASSERT_TRUE(upd.has_value()) << "ENDPOINT_UPDATE_REQ timed out";
    ASSERT_EQ(upd->value("status", std::string{}), "success")
        << "body=" << upd->dump();

    auto cons = make_wire_client(ctx, setup, cons_uid);
    auto disc = discover(cons, channel);
    ASSERT_TRUE(disc.has_value()) << "DISC_REQ timed out";
    EXPECT_EQ(disc->value("zmq_node_endpoint", std::string{}), updated_ep);

    broker.signal_quit();
}

TEST_F(Pattern4ZmqEndpointRegistryTest, EndpointUpdateNonProducerReturnsError)
{
    using namespace std::chrono;
    const std::string suffix    = ".pid" + std::to_string(::getpid());
    const std::string channel   = "zmqvc.ep.nonprod" + suffix;
    const std::string prod_uid  = "prod.ep." + channel;
    const std::string other_uid = "cons.notprod." + channel;
    const std::string updated_ep = "tcp://127.0.0.1:44455";

    const fs::path temp_dir = make_test_temp_dir("zep_nonprod");
    const auto     setup    = make_pattern4_setup({prod_uid, other_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer_zmq_ep(
        prod, setup, channel, prod_uid, "tcp://127.0.0.1:0"));

    // A different identity tries to update the producer's endpoint.
    auto other = make_wire_client(ctx, setup, other_uid);
    auto upd = endpoint_update(other, channel, "zmq_node", updated_ep);
    ASSERT_TRUE(upd.has_value()) << "ENDPOINT_UPDATE_REQ timed out";
    EXPECT_EQ(upd->value("status", std::string{}), "error")
        << "non-producer must be rejected; body=" << upd->dump();
    EXPECT_EQ(upd->value("error_code", std::string{}), "NOT_CHANNEL_OWNER")
        << "body=" << upd->dump();

    // The broker must not have silently applied the rejected update.  The
    // channel is still on port-0 (never heartbeat), so DISC may be
    // not-ready — only assert when a reply comes back.
    auto disc = discover(prod, channel, pylabhub::kMidTimeoutMs);
    if (disc.has_value())
    {
        EXPECT_NE(disc->value("zmq_node_endpoint", std::string{}), updated_ep)
            << "broker silently applied a rejected update";
    }

    broker.signal_quit();
}

TEST_F(Pattern4ZmqEndpointRegistryTest, EndpointUpdatePortZeroReturnsError)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "zmqvc.ep.zeroport" + suffix;
    const std::string uid     = "prod.ep." + channel;

    const fs::path temp_dir = make_test_temp_dir("zep_zeroport");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer_zmq_ep(prod, setup, channel, uid, "tcp://127.0.0.1:0"));

    // Update with a port-0 endpoint → INVALID_ENDPOINT.
    auto upd = endpoint_update(prod, channel, "zmq_node", "tcp://127.0.0.1:0");
    ASSERT_TRUE(upd.has_value()) << "ENDPOINT_UPDATE_REQ timed out";
    EXPECT_EQ(upd->value("status", std::string{}), "error")
        << "port-0 endpoint must be rejected; body=" << upd->dump();
    EXPECT_EQ(upd->value("error_code", std::string{}), "INVALID_ENDPOINT")
        << "body=" << upd->dump();

    broker.signal_quit();
}

// Reframe: BRC `unmatched_replies()` counter → direct wire drain.  None
// of the fire-and-forget REQs may produce a `*_ACK`/`ERROR` reply
// (HEP-CORE-0007 §12.2.1); unsolicited NOTIFY fan-out is allowed.
TEST_F(Pattern4ZmqEndpointRegistryTest, ReqShapeNoUnmatchedRepliesForFireAndForget)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "zmqvc.shape" + suffix;
    const std::string uid     = "prod.ep." + channel;
    const std::string band    = "!shape.test" + suffix;

    const fs::path temp_dir = make_test_temp_dir("zep_shape");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SPAWN_BROKER(temp_dir);
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(
        register_producer_zmq_ep(prod, setup, channel, uid, "tcp://127.0.0.1:44777"));

    // Join a band (sync REQ) so the band broadcast has a valid target; the
    // BAND_JOIN_ACK is consumed here so it isn't mistaken for an unmatched
    // reply below.
    nlohmann::json join_req;
    join_req["band"]      = band;
    join_req["role_uid"]  = uid;
    join_req["role_name"] = "shape";
    auto bj = prod.request("BAND_JOIN_REQ", join_req, "BAND_JOIN_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(bj.has_value()) << "BAND_JOIN_REQ timed out";

    // Fire every fire-and-forget REQ (HEP-CORE-0007 §12.2.1).
    nlohmann::json hb;
    hb["channel_name"] = channel; hb["role_uid"] = uid;
    hb["role_type"] = "producer"; hb["producer_pid"] = pylabhub::platform::get_pid();
    prod.send("HEARTBEAT_NOTIFY", hb);

    nlohmann::json csum;
    csum["channel_name"] = channel; csum["role_uid"] = uid;
    csum["slot_index"] = 0; csum["error"] = "shape-probe";
    csum["reporter_pid"] = pylabhub::platform::get_pid();
    prod.send("CHECKSUM_ERROR_REPORT", csum);

    nlohmann::json bcast;
    bcast["target_channel"] = channel; bcast["sender_uid"] = uid;
    bcast["message"] = "test-broadcast"; bcast["data"] = "test-data";
    prod.send("CHANNEL_BROADCAST_SEND_NOTIFY", bcast);

    nlohmann::json bband;
    bband["band"] = band; bband["role_uid"] = uid;
    bband["body"] = nlohmann::json{{"msg", "test-band-broadcast"}};
    prod.send("BAND_BROADCAST_SEND_NOTIFY", bband);

    // Drain for a short window; a `*_ACK` or `ERROR` frame would be a
    // §12.2.1 violation (the fire-and-forget REQ produced a reply).
    // CHANNEL_BROADCAST_DELIVER_NOTIFY back to this member is expected and
    // must NOT count.
    const auto deadline = steady_clock::now() + milliseconds{500};
    while (steady_clock::now() < deadline)
    {
        auto now = steady_clock::now();
        auto frame = prod.receive(
            duration_cast<milliseconds>(deadline - now));
        if (!frame) break;
        const std::string &mt = frame->first;
        const bool is_reply_shape =
            mt == "ERROR" ||
            (mt.size() >= 4 && mt.compare(mt.size() - 4, 4, "_ACK") == 0);
        EXPECT_FALSE(is_reply_shape)
            << "fire-and-forget REQ produced a reply-shape message '" << mt
            << "' — HEP-CORE-0007 §12.2.1 violation; body="
            << frame->second.dump();
    }

    // Leave the band (fire-and-forget cleanup via LEAVE sync REQ).
    nlohmann::json leave_req;
    leave_req["band"] = band; leave_req["role_uid"] = uid;
    (void)prod.request("BAND_LEAVE_REQ", leave_req, "BAND_LEAVE_ACK",
                        milliseconds{pylabhub::kMidTimeoutMs});

    broker.signal_quit();
}
