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
 *   - dead_consumer_orchestrator + dead_consumer_exiter — a two-subprocess
 *     dying-role scenario (consumer registers then _exit(0); broker
 *     detects the dead PID).  Needs a dedicated dying-consumer Pattern 4
 *     worker + a consumer_liveness_check_interval profile — deferred.
 *   - consumer_heartbeat_timeout_notify / two_snapshot_invariant — tight
 *     timing windows + liveness-interval config — deferred.
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
