/**
 * @file test_pattern4_broker_health.cpp
 * @brief Pattern 4 broker health / notification wire tests.
 *
 * Successors of the tractable wire-only workers formerly hosted under
 * `tests/test_layer3_datahub/workers/datahub_broker_health_workers.cpp`
 * against the retired in-process HubHostBrokerHandle harness (task #52
 * sweep).  Broker runs in its own subprocess (the generic
 * `pattern4_broker_protocol.broker`, with named timeout profiles); the
 * parent drives wire traffic + drains broker-pushed NOTIFYs via
 * BrokerWireClient using the shared Pattern4WireTest base.
 *
 * NOT migrated here (stay in L3 datahub_broker_health_workers.cpp):
 *   - consumer_auto_deregisters / multi_producer_partial_pending_timeout
 *     / ctrl_zap_deny_path — inspect in-process state (channel snapshot,
 *     ZapRouter denied-count singleton).  Round-3 disposition.
 *   - two_snapshot_invariant — CHANNEL_CLOSING_NOTIFY timing pin (deferred).
 *
 * The CONSUMER_DIED family IS migrated here: DeadConsumerDetected uses a
 * dedicated dying-consumer subprocess worker
 * (`pattern4_broker_protocol.dying_consumer`) that registers then
 * `std::_Exit(0)`, and the broker (dead_consumer profile,
 * consumer_liveness_check_interval=1s) detects the dead PID;
 * ConsumerHeartbeatTimeout uses a silent parent-side consumer +
 * consumer_timeout profile (liveness=0, short ready/pending).
 */
#include "pattern4_wire_test_base.h"

#include "broker_wire_client.h"

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

class Pattern4BrokerHealthTest
    : public pylabhub::tests::pattern4::Pattern4WireTest
{
};

}  // namespace

// producer registers, sends one heartbeat → Ready, then goes silent; the
// broker's fast_reclaim sweep demotes → terminates → CHANNEL_CLOSING_NOTIFY.
TEST_F(Pattern4BrokerHealthTest, ProducerGetsClosingNotify)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "health.closing_notify" + suffix;
    const std::string uid     = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_closing_notify");
    const auto     setup    = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "fast_reclaim"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    // One heartbeat → Ready, then go silent.  producer_heartbeat sleeps
    // 100 ms; after that the parent stops sending, so the 500/500 ms
    // sweep reclaims the presence.
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    auto notify = drain_for(prod, "CHANNEL_CLOSING_NOTIFY",
                             milliseconds{5000});
    EXPECT_TRUE(notify.has_value())
        << "CHANNEL_CLOSING_NOTIFY was not received within 5s after the "
           "producer went silent";

    broker.signal_quit();
}

// Producer A registers + DEREGs; producer B then registers the same channel
// and succeeds immediately (no Pending window) — proving DEREG fired.  Long
// timeouts ensure a sweep can't be the reason B succeeds.
TEST_F(Pattern4BrokerHealthTest, ProducerAutoDeregisters)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "health.producer_dereg" + suffix;
    const std::string prod_a   = "prod.a." + channel;
    const std::string prod_b   = "prod.b." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_producer_dereg");
    const auto     setup    = make_pattern4_setup({prod_a, prod_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "long_reclaim"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    {
        auto a = make_wire_client(ctx, setup, prod_a);
        ASSERT_NO_FATAL_FAILURE(register_producer(a, setup, channel, prod_a));
        nlohmann::json dbody;
        dbody["channel_name"] = channel;
        dbody["role_uid"]     = prod_a;
        dbody["producer_pid"] = pylabhub::platform::get_pid();
        auto dereg = a.request("DEREG_REQ", dbody, "DEREG_ACK",
                                milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(dereg.has_value()) << "DEREG_REQ timed out";
        EXPECT_EQ(dereg->value("status", std::string{}), "success");
    }

    // Producer B: same channel registers immediately because A explicitly
    // DEREG'd (one-to-one cardinality slot freed without a Pending wait).
    auto b = make_wire_client(ctx, setup, prod_b);
    auto reg_b = b.request("REG_REQ",
                            producer_reg_body(setup, channel, prod_b, /*shm=*/false),
                            "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg_b.has_value()) << "producer B REG_REQ timed out";
    EXPECT_EQ(reg_b->value("status", std::string{}), "success")
        << "producer B failed to register — DEREG_REQ was not processed; body="
        << reg_b->dump();

    broker.signal_quit();
}

// Producer A registers with schema_hash=aaa; producer B registers the same
// channel with schema_hash=bbb → SCHEMA_MISMATCH error, and the broker fires
// CHANNEL_ERROR_NOTIFY to producer A.
TEST_F(Pattern4BrokerHealthTest, SchemaMismatchNotify)
{
    using namespace std::chrono;
    const std::string suffix  = ".pid" + std::to_string(::getpid());
    const std::string channel = "health.schema_mismatch" + suffix;
    const std::string uid_a   = "prod.a." + channel;
    const std::string uid_b   = "prod.b." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_schema_mismatch");
    const auto     setup    = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           a = make_wire_client(ctx, setup, uid_a);
    // schema_hash without schema_blds → stored as an opaque citation (no
    // canonical recomputation).
    auto body_a = producer_reg_body(setup, channel, uid_a, /*shm=*/true);
    body_a["schema_hash"] = std::string(64, 'a');
    auto reg_a = a.request("REG_REQ", body_a, "REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg_a.has_value());
    ASSERT_EQ(reg_a->value("status", std::string{}), "success")
        << "body=" << reg_a->dump();

    auto b = make_wire_client(ctx, setup, uid_b);
    auto body_b = producer_reg_body(setup, channel, uid_b, /*shm=*/true);
    body_b["schema_hash"] = std::string(64, 'b');
    auto reg_b = b.request("REG_REQ", body_b, "REG_ACK",
                            milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg_b.has_value())
        << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(reg_b->value("status", std::string{}), "error");
    EXPECT_EQ(reg_b->value("error_code", std::string{}), "SCHEMA_MISMATCH")
        << "body=" << reg_b->dump();

    // Producer A must receive CHANNEL_ERROR_NOTIFY for the conflict.
    auto notify = drain_for(a, "CHANNEL_ERROR_NOTIFY", milliseconds{5000});
    EXPECT_TRUE(notify.has_value())
        << "CHANNEL_ERROR_NOTIFY (schema mismatch) not received within 5s";

    broker.signal_quit();
}

// PID-death path: a dying-consumer subprocess registers then hard-exits
// (no DEREG); the broker's liveness sweep (dead_consumer profile,
// consumer_liveness_check_interval=1s) detects the dead PID and fires
// CONSUMER_DIED_NOTIFY(reason="process_dead") to the producer.
TEST_F(Pattern4BrokerHealthTest, DeadConsumerDetected)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "health.dead_consumer" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_dead_consumer");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "dead_consumer"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    // One heartbeat → kLive (ready timeout is 15 s, so the producer stays
    // alive through the drain without a heartbeat thread).
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    // Dying consumer: a subprocess that registers then std::_Exit(0).
    auto exiter = SpawnWorker("pattern4_broker_protocol.dying_consumer",
                              {temp_dir.string(), channel, cons_uid});
    exiter.wait_for_exit();

    // Broker's 1 s liveness sweep detects the dead PID → CONSUMER_DIED_NOTIFY.
    auto notify = drain_for(prod, "CONSUMER_DIED_NOTIFY", milliseconds{8000});
    ASSERT_TRUE(notify.has_value())
        << "CONSUMER_DIED_NOTIFY not received within 8s after the consumer "
           "subprocess exited";
    EXPECT_EQ(notify->value("channel_name", std::string{}), channel);
    EXPECT_EQ(notify->value("role_uid", std::string{}), cons_uid);
    EXPECT_EQ(notify->value("reason", std::string{}), "process_dead")
        << "PID-death path must report reason='process_dead'; body="
        << notify->dump();

    broker.signal_quit();
}

// Heartbeat-timeout path: a registered consumer goes silent while the
// producer keeps heartbeating; with the PID sweep OFF (consumer_timeout
// profile, liveness=0, ready/pending=500 ms) the broker fires
// CONSUMER_DIED_NOTIFY(reason="heartbeat_timeout").
TEST_F(Pattern4BrokerHealthTest, ConsumerHeartbeatTimeout_NotifyBodyShape)
{
    using namespace std::chrono;
    const std::string suffix   = ".pid" + std::to_string(::getpid());
    const std::string channel  = "health.cons_hb_timeout" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_cons_hb_timeout");
    const auto     setup    = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "consumer_timeout"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
                milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto           prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));
    // One consumer heartbeat to establish liveness, then the consumer goes
    // silent (the parent never touches `cons` again).
    {
        nlohmann::json hb;
        hb["channel_name"] = channel; hb["role_uid"] = cons_uid;
        hb["role_type"] = "consumer"; hb["producer_pid"] = pylabhub::platform::get_pid();
        cons.send("HEARTBEAT_NOTIFY", hb);
    }

    // Interleave producer heartbeats (keep it alive past the 500 ms ready
    // timeout) with draining for CONSUMER_DIED_NOTIFY — single socket, one
    // thread, so no concurrent send/recv on the raw client.
    auto send_prod_hb = [&] {
        nlohmann::json hb;
        hb["channel_name"] = channel; hb["role_uid"] = prod_uid;
        hb["role_type"] = "producer"; hb["producer_pid"] = pylabhub::platform::get_pid();
        prod.send("HEARTBEAT_NOTIFY", hb);
    };
    nlohmann::json died;
    const auto deadline = steady_clock::now() + milliseconds{8000};
    while (steady_clock::now() < deadline)
    {
        send_prod_hb();
        auto frame = prod.receive(milliseconds{200});
        if (frame && frame->first == "CONSUMER_DIED_NOTIFY")
        {
            died = frame->second;
            break;
        }
    }
    ASSERT_FALSE(died.is_null())
        << "CONSUMER_DIED_NOTIFY (heartbeat_timeout) not received within 8s";
    EXPECT_EQ(died.value("channel_name", std::string{}), channel);
    EXPECT_EQ(died.value("role_uid", std::string{}), cons_uid);
    EXPECT_EQ(died.value("reason", std::string{}), "heartbeat_timeout")
        << "silent-consumer path must report reason='heartbeat_timeout' "
           "(distinct from PID-death 'process_dead'); body=" << died.dump();
    EXPECT_TRUE(died.contains("consumer_pid"));
    EXPECT_TRUE(died.contains("consumer_hostname"));

    broker.signal_quit();
}
