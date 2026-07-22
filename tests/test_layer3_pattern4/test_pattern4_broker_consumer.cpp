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

TEST_F(Pattern4BrokerConsumerTest, ConsumerReg_ChannelNotFound)
{
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
    EXPECT_EQ(resp->value("error_code", std::string{}), "CHANNEL_NOT_FOUND")
        << "body=" << resp->dump();

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

    // Post-registration: allowlist = [consumer pubkey] (bare Z85 strings).
    auto post = get_channel_auth(prod, channel, prod_uid);
    ASSERT_TRUE(post.has_value());
    EXPECT_EQ(post->value("status", std::string{}), "success");
    const auto &al = post->at("allowlist");
    ASSERT_TRUE(al.is_array());
    ASSERT_EQ(al.size(), 1u);
    ASSERT_TRUE(al[0].is_string());
    EXPECT_EQ(al[0].get<std::string>(), setup.curve.role(cons_uid).public_z85);

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
    bool admitted_in_ledger = false;
    for (const auto &e : auth->at("allowlist"))
        if (e.is_string() && e.get<std::string>() == setup.curve.role(cons_uid).public_z85)
            admitted_in_ledger = true;
    EXPECT_TRUE(admitted_in_ledger)
        << "success reply must reflect ledger admission; body=" << auth->dump();

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
// `_on_consumer_revoked` → `ledger.revoke`, and the attach gate reads the same
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
