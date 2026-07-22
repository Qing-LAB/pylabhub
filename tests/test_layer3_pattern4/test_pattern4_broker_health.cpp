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
 * The CONSUMER_DIED family: ConsumerHeartbeatTimeout uses a silent
 * parent-side consumer + consumer_timeout profile (short ready/pending) —
 * heartbeat timeout (HEP-CORE-0023 §2.1) is the sole consumer-liveness
 * mechanism, and its reclaim now also revokes channel admission
 * (HEP-CORE-0036 §6.5).  (DeadConsumerDetected + the PID-liveness sweep it
 * exercised were removed 2026-07-17 — see the retirement doc-block below.)
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

class Pattern4BrokerHealthTest : public pylabhub::tests::pattern4::Pattern4WireTest
{
};

} // namespace

// producer registers, sends one heartbeat → Ready, then goes silent; the
// broker's fast_reclaim sweep demotes → terminates → CHANNEL_CLOSING_NOTIFY.
TEST_F(Pattern4BrokerHealthTest, ProducerGetsClosingNotify)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "health.closing_notify" + suffix;
    const std::string uid = "prod." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_closing_notify");
    const auto setup = make_pattern4_setup({uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "fast_reclaim"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, uid));
    // One heartbeat → Ready, then go silent.  producer_heartbeat sleeps
    // 100 ms; after that the parent stops sending, so the 500/500 ms
    // sweep reclaims the presence.
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, uid));

    auto notify = drain_for(prod, "CHANNEL_CLOSING_NOTIFY", milliseconds{5000});
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
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "health.producer_dereg" + suffix;
    const std::string prod_a = "prod.a." + channel;
    const std::string prod_b = "prod.b." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_producer_dereg");
    const auto setup = make_pattern4_setup({prod_a, prod_b});
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
        dbody["role_uid"] = prod_a;
        dbody["producer_pid"] = pylabhub::platform::get_pid();
        auto dereg =
            a.request("DEREG_REQ", dbody, "DEREG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
        ASSERT_TRUE(dereg.has_value()) << "DEREG_REQ timed out";
        EXPECT_EQ(dereg->value("status", std::string{}), "success");
    }

    // Producer B: same channel registers immediately because A explicitly
    // DEREG'd (one-to-one cardinality slot freed without a Pending wait).
    auto b = make_wire_client(ctx, setup, prod_b);
    auto reg_b = b.request("REG_REQ", producer_reg_body(setup, channel, prod_b, /*shm=*/false),
                           "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg_b.has_value()) << "producer B REG_REQ timed out";
    EXPECT_EQ(reg_b->value("status", std::string{}), "success")
        << "producer B failed to register — DEREG_REQ was not processed; body=" << reg_b->dump();

    broker.signal_quit();
}

// Producer A registers with schema_hash=aaa; producer B registers the same
// channel with schema_hash=bbb → SCHEMA_MISMATCH error, and the broker fires
// CHANNEL_ERROR_NOTIFY to producer A.
TEST_F(Pattern4BrokerHealthTest, SchemaMismatchNotify)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "health.schema_mismatch" + suffix;
    const std::string uid_a = "prod.a." + channel;
    const std::string uid_b = "prod.b." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_schema_mismatch");
    const auto setup = make_pattern4_setup({uid_a, uid_b});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "default"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto a = make_wire_client(ctx, setup, uid_a);
    // schema_hash without schema_blds → stored as an opaque citation (no
    // canonical recomputation).
    auto body_a = producer_reg_body(setup, channel, uid_a, /*shm=*/true);
    body_a["schema_hash"] = std::string(64, 'a');
    auto reg_a = a.request("REG_REQ", body_a, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg_a.has_value());
    ASSERT_EQ(reg_a->value("status", std::string{}), "success") << "body=" << reg_a->dump();

    auto b = make_wire_client(ctx, setup, uid_b);
    auto body_b = producer_reg_body(setup, channel, uid_b, /*shm=*/true);
    body_b["schema_hash"] = std::string(64, 'b');
    auto reg_b = b.request("REG_REQ", body_b, "REG_ACK", milliseconds{pylabhub::kLongTimeoutMs});
    ASSERT_TRUE(reg_b.has_value()) << "broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(reg_b->value("status", std::string{}), "error");
    EXPECT_EQ(reg_b->value("error_code", std::string{}), "SCHEMA_MISMATCH")
        << "body=" << reg_b->dump();

    // Producer A must receive CHANNEL_ERROR_NOTIFY for the conflict.
    auto notify = drain_for(a, "CHANNEL_ERROR_NOTIFY", milliseconds{5000});
    EXPECT_TRUE(notify.has_value())
        << "CHANNEL_ERROR_NOTIFY (schema mismatch) not received within 5s";

    broker.signal_quit();
}

// ── RETIRED 2026-07-17 — DeadConsumerDetected + the broker PID-liveness
//    sweep it exercised ──────────────────────────────────────────────────
// The test drove `check_dead_consumers`, which called `is_process_alive(pid)`
// on every registered consumer's PID.  That is a LOCAL-ONLY OS check with no
// hostname gate — meaningless (and actively harmful: false-positive reaps) for
// any consumer not co-located with the broker, i.e. every distributed
// deployment.  It only appeared to work because this + the L3 tests run
// broker + consumer on one machine.  The mechanism, the `process_dead` wire
// reason, and the `consumer_liveness_check_interval` config are deleted; the
// universal heartbeat FSM (HEP-CORE-0023 §2.1, Connected→Pending→Disconnected)
// is now the sole consumer-liveness path.
//
// Where the coverage lives now: `ConsumerHeartbeatTimeout_NotifyBodyShape`
// (below) pins the reclaim's CONSUMER_DIED_NOTIFY(reason="heartbeat_timeout"),
// and the reclaim now ALSO revokes channel admission — the revoke path that
// used to sit only on this deleted PID branch moved onto the heartbeat reclaim
// (broker_service.cpp check_heartbeat_timeouts; HEP-CORE-0036 §6.5), covered by
// the L3 `ConsumerAttach_DeniedAfterDereg` + the ledger unit tests.

// Heartbeat-timeout path: a registered consumer goes silent while the
// producer keeps heartbeating; with the PID sweep OFF (consumer_timeout
// profile, liveness=0, ready/pending=500 ms) the broker fires
// CONSUMER_DIED_NOTIFY(reason="heartbeat_timeout").
TEST_F(Pattern4BrokerHealthTest, ConsumerHeartbeatTimeout_NotifyBodyShape)
{
    using namespace std::chrono;
    const std::string suffix = ".pid" + std::to_string(::getpid());
    const std::string channel = "health.cons_hb_timeout" + suffix;
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    const fs::path temp_dir = make_test_temp_dir("bh_cons_hb_timeout");
    const auto setup = make_pattern4_setup({prod_uid, cons_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    auto broker = SpawnWorkerWithQuitSignal("pattern4_broker_protocol.broker",
                                            {temp_dir.string(), "consumer_timeout"});
    expect_log(broker, "Pattern4BrokerProtocol: bound endpoint",
               milliseconds{pylabhub::kMidTimeoutMs});

    zmq::context_t ctx;
    auto prod = make_wire_client(ctx, setup, prod_uid);
    ASSERT_NO_FATAL_FAILURE(register_producer(prod, setup, channel, prod_uid));
    ASSERT_NO_FATAL_FAILURE(producer_heartbeat(prod, channel, prod_uid));

    auto cons = make_wire_client(ctx, setup, cons_uid);
    ASSERT_NO_FATAL_FAILURE(register_consumer(cons, setup, channel, cons_uid));
    // One consumer heartbeat to establish liveness, then the consumer goes
    // silent (the parent never touches `cons` again).
    {
        nlohmann::json hb;
        hb["channel_name"] = channel;
        hb["role_uid"] = cons_uid;
        hb["role_type"] = "consumer";
        hb["producer_pid"] = pylabhub::platform::get_pid();
        cons.send("HEARTBEAT_NOTIFY", hb);
    }

    // Interleave producer heartbeats (keep it alive past the 500 ms ready
    // timeout) with draining for CONSUMER_DIED_NOTIFY — single socket, one
    // thread, so no concurrent send/recv on the raw client.
    auto send_prod_hb = [&]
    {
        nlohmann::json hb;
        hb["channel_name"] = channel;
        hb["role_uid"] = prod_uid;
        hb["role_type"] = "producer";
        hb["producer_pid"] = pylabhub::platform::get_pid();
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
           "(heartbeat timeout is the sole liveness path); body="
        << died.dump();
    EXPECT_TRUE(died.contains("consumer_pid"));
    EXPECT_TRUE(died.contains("consumer_hostname"));
    // Path pin: corroborate against the broker's own emission trace that
    // the heartbeat-timeout branch fired.
    expect_log(broker,
               "reason=heartbeat_timeout target_role=", milliseconds{pylabhub::kMidTimeoutMs});

    // REVIEW-D (#277): the reclaim MUST also revoke the consumer's channel
    // admission — heartbeat timeout is the sole liveness path, so this is the
    // only automatic revocation trigger for a dead consumer (the deleted PID
    // sweep used to carry it).  The producer receives CHANNEL_AUTH_CHANGED_
    // NOTIFY(phase="left") for the revoked consumer right after the DIED
    // notify (broker_service.cpp check_heartbeat_timeouts; HEP-CORE-0036 §6.5).
    auto revoked = drain_for(prod, "CHANNEL_AUTH_CHANGED_NOTIFY", milliseconds{5000});
    ASSERT_TRUE(revoked.has_value())
        << "reclaim must revoke admission + fire CHANNEL_AUTH_CHANGED_NOTIFY";
    EXPECT_EQ(revoked->value("channel_name", std::string{}), channel);
    EXPECT_EQ(revoked->value("role_uid", std::string{}), cons_uid);
    EXPECT_EQ(revoked->value("role_type", std::string{}), "consumer");
    EXPECT_EQ(revoked->value("phase", std::string{}), "left")
        << "revocation-on-reclaim must fire phase='left'; body=" << revoked->dump();

    broker.signal_quit();
}
