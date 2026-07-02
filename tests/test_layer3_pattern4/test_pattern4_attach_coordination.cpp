/**
 * @file test_pattern4_attach_coordination.cpp
 * @brief L3 broker-wire tests for HEP-CORE-0042 Channel Attach
 *        Coordination Protocol (Phase 2.4b, task #246).
 *
 * Each test drives a `BrokerWireClient`-per-role against a real broker
 * subprocess (spawned via `pattern4_smoke.broker`), issues the
 * canonical HEP-0036 §5b REG_REQ / CONSUMER_REG_REQ + HEP-0042 §5.5
 * CHANNEL_AUTH_APPLIED_REQ / CONSUMER_ATTACH_REQ_ZMQ wire frames, and
 * pins the broker's handler-flow outcomes.
 *
 * **This rung (2.4b-1) covers the fast-path admit scenario** — the
 * happy synchronous flow through HEP-0042 §5.4 steps 1-4 (validate,
 * allowlist, producer-live, confirmed-version check).  Wait-path,
 * denial paths, and timeout land in subsequent rungs (2.4b-2, 2.4b-3).
 */
#include "broker_wire_client.h"
#include "pattern4_helpers.h"

#include "shared_test_helpers.h"
#include "test_patterns.h"

#include "plh_platform.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
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

class Pattern4AttachCoordinationTest : public IsolatedProcessTest
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

    std::vector<fs::path> paths_to_clean_;
};

// ─── Fast-path admit (HEP-CORE-0042 §5.4 steps 1-4) ──────────────────────
//
// Setup:
//   - producer P registers on channel K → channel_version[K] = 0
//     (empty allowlist).
//   - consumer C registers on K, gets added to allowlist →
//     channel_version[K] = 1.  Broker fires CHANNEL_AUTH_CHANGED_NOTIFY
//     to P (fan-out) — the test's producer client ignores it.
//   - producer P sends CHANNEL_AUTH_APPLIED_REQ(applied_version=1,
//     instance_id=1) → broker advances confirmed_version[K][P] = 1.
// Attach:
//   - consumer C sends CONSUMER_ATTACH_REQ_ZMQ(channel=K, producer=P).
//   - Broker: consumer.pubkey ∈ allowlist ✓, producer live ✓,
//     confirmed_version(1) >= channel_version(1) ✓ → immediate
//     {status="success"} via §5.4 fast-path.

TEST_F(Pattern4AttachCoordinationTest, FastPathAdmit)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    // ── 1. Setup: temp dir + Pattern4Setup with producer + consumer uids ──
    const std::string producer_uid = "prod.p.uid00000001";
    const std::string consumer_uid = "cons.c.uid00000002";
    const std::string channel_name = "ch.attach.fastpath";

    const fs::path temp_dir = make_test_temp_dir("attach_fastpath");
    const auto setup = make_pattern4_setup({producer_uid, consumer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    // Producer's declared ZMQ data-plane endpoint — the broker stores
    // this on ChannelEntry.zmq_node_endpoint and validates it on
    // consumer REG (which fails with CHANNEL_NOT_READY if the port
    // hasn't been resolved yet).  Use a distinct unused port from the
    // broker's control endpoint so the two don't collide.
    const int producer_data_port = pick_unused_port();

    // ── 2. Broker subprocess ──
    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    // ── 3. Construct one BrokerWireClient per role ──
    zmq::context_t ctx;
    const auto &prod_kp = setup.curve.role(producer_uid);
    const auto &cons_kp = setup.curve.role(consumer_uid);

    BrokerWireClient::Config prod_cfg;
    prod_cfg.broker_endpoint = setup.broker_endpoint;
    prod_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    prod_cfg.client_pubkey   = prod_kp.public_z85;
    prod_cfg.client_seckey   = prod_kp.secret_z85;
    BrokerWireClient prod_client(ctx, prod_cfg);

    BrokerWireClient::Config cons_cfg;
    cons_cfg.broker_endpoint = setup.broker_endpoint;
    cons_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cons_cfg.client_pubkey   = cons_kp.public_z85;
    cons_cfg.client_seckey   = cons_kp.secret_z85;
    BrokerWireClient cons_client(ctx, cons_cfg);

    // ── 4. Producer REG_REQ → REG_ACK ──
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = producer_uid;
        in.role_name         = "p";
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(producer_data_port);
        in.zmq_pubkey        = prod_kp.public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);

        auto reply = prod_client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "producer REG_REQ: no reply";
        ASSERT_EQ(reply->value("status", ""), "success")
            << "producer REG_REQ failed: " << reply->dump();
    }

    // ── 4b. Producer HEARTBEAT_REQ (fire-and-forget) ──
    // Broker rejects CONSUMER_REG_REQ with CHANNEL_NOT_READY until the
    // producer has sent at least one heartbeat (HEP-CORE-0023 §2.5.3
    // "awaiting_first_heartbeat" gate).
    {
        nlohmann::json hb;
        hb["channel_name"] = channel_name;
        hb["role_uid"]     = producer_uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = 1;
        prod_client.send("HEARTBEAT_REQ", hb);
        // No reply expected — HEARTBEAT_REQ is fire-and-forget.
        // Give the broker a beat to process before consumer REG.
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // ── 5. Consumer CONSUMER_REG_REQ → CONSUMER_REG_ACK ──
    // Broker will bump channel_version[K] from 0 → 1 as it adds
    // consumer.pubkey to authorized_consumer_pubkeys.
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel    = channel_name;
        in.role_uid   = consumer_uid;
        in.role_name  = "c";
        in.zmq_pubkey = cons_kp.public_z85;
        auto payload = pylabhub::hub::build_consumer_reg_payload(in);

        auto reply = cons_client.request(
            "CONSUMER_REG_REQ", payload, "CONSUMER_REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value()) << "consumer REG_REQ: no reply";
        ASSERT_EQ(reply->value("status", ""), "success")
            << "consumer REG_REQ failed: " << reply->dump();
    }

    // ── 6. Producer sends CHANNEL_AUTH_APPLIED_REQ(v=1, instance=1) ──
    // Simulates the producer's cache having caught up to the new
    // allowlist version.  Broker advances confirmed_version[K][P] = 1.
    //
    // instance_id: broker assigns instance=1 on first REG (§5.2 instance
    // monotonic starts at 1; §5.4 producer registration bumps).  Since
    // REG_ACK doesn't yet echo instance (Phase 3a TODO), the test uses
    // the known-first-registration value.
    {
        nlohmann::json applied;
        applied["channel_name"]     = channel_name;
        applied["producer_role_uid"] = producer_uid;
        applied["applied_version"]  = 1u;
        applied["instance_id"]      = 1u;

        auto reply = prod_client.request(
            "CHANNEL_AUTH_APPLIED_REQ", applied,
            "CHANNEL_AUTH_APPLIED_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "producer APPLIED_REQ: no reply";
        ASSERT_EQ(reply->value("status", ""), "ok")
            << "producer APPLIED_REQ failed: " << reply->dump();
        ASSERT_EQ(reply->value("applied_version", 0u), 1u);
    }

    // ── 7. Consumer sends CONSUMER_ATTACH_REQ_ZMQ → fast-path SUCCESS ──
    {
        nlohmann::json attach;
        attach["channel_name"]      = channel_name;
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = producer_uid;

        auto reply = cons_client.request(
            "CONSUMER_ATTACH_REQ_ZMQ", attach,
            "CONSUMER_ATTACH_ACK_ZMQ",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "consumer ATTACH_REQ: no reply";
        EXPECT_EQ(reply->value("status", ""), "success")
            << "expected §5.4 fast-path admit, got: " << reply->dump();
        EXPECT_EQ(reply->value("channel_name", ""), channel_name);
        EXPECT_EQ(reply->value("producer_role_uid", ""), producer_uid);
    }

    // ── 8. Teardown ──
    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── Denied: consumer pubkey not in channel allowlist (§5.4 step 2) ─────
//
// Setup:
//   - producer P registers on channel K + heartbeats (channel exists).
//   - consumer C's pubkey is NOT added to the allowlist (C never sends
//     CONSUMER_REG_REQ).
// Attach:
//   - C sends CONSUMER_ATTACH_REQ_ZMQ with its own pubkey.
//   - Broker: consumer_pubkey ∉ authorized_consumer_pubkeys →
//     {status="denied", reason="consumer_not_in_channel_allowlist"}.

TEST_F(Pattern4AttachCoordinationTest, DeniedConsumerNotInAllowlist)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = "prod.p.uid00000001";
    const std::string consumer_uid = "cons.c.uid00000002";
    const std::string channel_name = "ch.attach.denied_allowlist";

    const fs::path temp_dir = make_test_temp_dir("attach_denied_allowlist");
    const auto setup = make_pattern4_setup({producer_uid, consumer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const int producer_data_port = pick_unused_port();

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &prod_kp = setup.curve.role(producer_uid);
    const auto &cons_kp = setup.curve.role(consumer_uid);

    BrokerWireClient::Config prod_cfg;
    prod_cfg.broker_endpoint = setup.broker_endpoint;
    prod_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    prod_cfg.client_pubkey   = prod_kp.public_z85;
    prod_cfg.client_seckey   = prod_kp.secret_z85;
    BrokerWireClient prod_client(ctx, prod_cfg);

    BrokerWireClient::Config cons_cfg;
    cons_cfg.broker_endpoint = setup.broker_endpoint;
    cons_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cons_cfg.client_pubkey   = cons_kp.public_z85;
    cons_cfg.client_seckey   = cons_kp.secret_z85;
    BrokerWireClient cons_client(ctx, cons_cfg);

    // Producer REG + heartbeat — opens the channel.
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = producer_uid;
        in.role_name         = "p";
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(producer_data_port);
        in.zmq_pubkey        = prod_kp.public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        auto reply = prod_client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }
    {
        nlohmann::json hb;
        hb["channel_name"] = channel_name;
        hb["role_uid"]     = producer_uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = 1;
        prod_client.send("HEARTBEAT_REQ", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // Consumer attaches WITHOUT registering — its pubkey is not in
    // the channel's allowlist.
    {
        nlohmann::json attach;
        attach["channel_name"]      = channel_name;
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = producer_uid;

        auto reply = cons_client.request(
            "CONSUMER_ATTACH_REQ_ZMQ", attach,
            "CONSUMER_ATTACH_ACK_ZMQ",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "consumer ATTACH_REQ: no reply";
        EXPECT_EQ(reply->value("status", ""), "denied") << reply->dump();
        EXPECT_EQ(reply->value("reason", ""),
                   "consumer_not_in_channel_allowlist")
            << reply->dump();
    }

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── Denied: producer not live on the channel (§5.4 step 3) ────────────
//
// Setup:
//   - REAL producer P registers on channel K + heartbeats + is caught
//     up (so we have a valid channel with a valid allowlist).
//   - consumer C REGs → its pubkey IS in the allowlist.
// Attach:
//   - C sends ATTACH_REQ referencing a DIFFERENT producer_role_uid that
//     was never registered on K.
//   - Broker: producer_role_uid ∉ ch->producers[] →
//     {status="denied", reason="producer_not_live"}.
//
// The bogus uid uses the canonical `prod.<name>.<unique>` grammar so
// the wire-format guard doesn't short-circuit — we want the §5.4 step 3
// producer-live check to be the branch that runs.

TEST_F(Pattern4AttachCoordinationTest, DeniedProducerNotLive)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = "prod.p.uid00000001";
    const std::string consumer_uid = "cons.c.uid00000002";
    const std::string bogus_producer_uid = "prod.ghost.uid00000099";
    const std::string channel_name = "ch.attach.denied_notlive";

    const fs::path temp_dir = make_test_temp_dir("attach_denied_notlive");
    const auto setup = make_pattern4_setup({producer_uid, consumer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const int producer_data_port = pick_unused_port();

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &prod_kp = setup.curve.role(producer_uid);
    const auto &cons_kp = setup.curve.role(consumer_uid);

    BrokerWireClient::Config prod_cfg;
    prod_cfg.broker_endpoint = setup.broker_endpoint;
    prod_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    prod_cfg.client_pubkey   = prod_kp.public_z85;
    prod_cfg.client_seckey   = prod_kp.secret_z85;
    BrokerWireClient prod_client(ctx, prod_cfg);

    BrokerWireClient::Config cons_cfg;
    cons_cfg.broker_endpoint = setup.broker_endpoint;
    cons_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cons_cfg.client_pubkey   = cons_kp.public_z85;
    cons_cfg.client_seckey   = cons_kp.secret_z85;
    BrokerWireClient cons_client(ctx, cons_cfg);

    // Producer REG + heartbeat.
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = producer_uid;
        in.role_name         = "p";
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(producer_data_port);
        in.zmq_pubkey        = prod_kp.public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        auto reply = prod_client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }
    {
        nlohmann::json hb;
        hb["channel_name"] = channel_name;
        hb["role_uid"]     = producer_uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = 1;
        prod_client.send("HEARTBEAT_REQ", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // Consumer REG — bumps channel_version, gets pubkey on allowlist.
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel    = channel_name;
        in.role_uid   = consumer_uid;
        in.role_name  = "c";
        in.zmq_pubkey = cons_kp.public_z85;
        auto payload = pylabhub::hub::build_consumer_reg_payload(in);
        auto reply = cons_client.request(
            "CONSUMER_REG_REQ", payload, "CONSUMER_REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // Consumer attaches to a BOGUS producer_role_uid on the same channel.
    // The channel exists (producer P is registered) and the consumer is
    // on the allowlist, so §5.4 step 2 (allowlist) passes, but step 3
    // (producer-live) fails because bogus_producer_uid is not in
    // ch->producers[].
    {
        nlohmann::json attach;
        attach["channel_name"]      = channel_name;
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = bogus_producer_uid;

        auto reply = cons_client.request(
            "CONSUMER_ATTACH_REQ_ZMQ", attach,
            "CONSUMER_ATTACH_ACK_ZMQ",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "consumer ATTACH_REQ: no reply";
        EXPECT_EQ(reply->value("status", ""), "denied") << reply->dump();
        EXPECT_EQ(reply->value("reason", ""), "producer_not_live")
            << reply->dump();
    }

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── Wait-path: enqueue + NOTIFY + APPLIED_REQ drain (§5.4 step 5 + d) ──
//
// Setup:
//   - producer P REGs on channel K + heartbeats.
//   - consumer C REGs (bumps channel_version[K]: 0 → 1).
//     Producer's DEALER receives a CHANNEL_AUTH_CHANGED_NOTIFY fan-
//     out from the CONSUMER_REG mutation — the test ignores it (it
//     will be discarded by producer's next request() poll).
// Wait-path:
//   - consumer sends CONSUMER_ATTACH_REQ_ZMQ WITHOUT the producer
//     having sent APPLIED_REQ first.  Broker: fast-path miss
//     (confirmed_version 0 < channel_version 1) → enqueue in
//     pending_attach_queue_[K][P] + fire targeted NOTIFY
//     (reason="attach_wait_path") to producer + return
//     {status="pending"} sentinel (dispatcher sends NOTHING to
//     consumer — the reply is deferred).
//   - producer sends CHANNEL_AUTH_APPLIED_REQ(v=1, instance=1) →
//     broker advances confirmed_version = 1, walks pending_attach_
//     queue, drains the consumer's entry, sends deferred
//     CONSUMER_ATTACH_ACK_ZMQ{status="success"} to consumer.
//   - consumer's DEALER receives the deferred success reply.

TEST_F(Pattern4AttachCoordinationTest, WaitPathEnqueueAndDrainOnAppliedReq)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = "prod.p.uid00000001";
    const std::string consumer_uid = "cons.c.uid00000002";
    const std::string channel_name = "ch.attach.waitpath";

    const fs::path temp_dir = make_test_temp_dir("attach_waitpath");
    const auto setup = make_pattern4_setup({producer_uid, consumer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const int producer_data_port = pick_unused_port();

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &prod_kp = setup.curve.role(producer_uid);
    const auto &cons_kp = setup.curve.role(consumer_uid);

    BrokerWireClient::Config prod_cfg;
    prod_cfg.broker_endpoint = setup.broker_endpoint;
    prod_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    prod_cfg.client_pubkey   = prod_kp.public_z85;
    prod_cfg.client_seckey   = prod_kp.secret_z85;
    BrokerWireClient prod_client(ctx, prod_cfg);

    BrokerWireClient::Config cons_cfg;
    cons_cfg.broker_endpoint = setup.broker_endpoint;
    cons_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cons_cfg.client_pubkey   = cons_kp.public_z85;
    cons_cfg.client_seckey   = cons_kp.secret_z85;
    BrokerWireClient cons_client(ctx, cons_cfg);

    // Producer REG + heartbeat.
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = producer_uid;
        in.role_name         = "p";
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(producer_data_port);
        in.zmq_pubkey        = prod_kp.public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        auto reply = prod_client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }
    {
        nlohmann::json hb;
        hb["channel_name"] = channel_name;
        hb["role_uid"]     = producer_uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = 1;
        prod_client.send("HEARTBEAT_REQ", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // Consumer REG — bumps channel_version[K] 0→1, fires NOTIFY at
    // producer (fan-out from CONSUMER_REG's allowlist mutation).
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel    = channel_name;
        in.role_uid   = consumer_uid;
        in.role_name  = "c";
        in.zmq_pubkey = cons_kp.public_z85;
        auto payload = pylabhub::hub::build_consumer_reg_payload(in);
        auto reply = cons_client.request(
            "CONSUMER_REG_REQ", payload, "CONSUMER_REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // ── Wait-path trigger: consumer ATTACH_REQ without producer having
    //    APPLIED_REQ first ──
    //
    // Fire-and-forget send: the broker will enqueue instead of
    // replying immediately (§5.4 step 5).  The consumer's DEALER
    // sees no reply until the producer's APPLIED_REQ arrives + the
    // broker drains the queue.
    {
        nlohmann::json attach;
        attach["channel_name"]      = channel_name;
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = producer_uid;
        cons_client.send("CONSUMER_ATTACH_REQ_ZMQ", attach);
    }

    // Give the broker a beat to enqueue and fire NOTIFY.  Confirm the
    // consumer is NOT receiving a reply yet — the deferred-reply
    // contract mandates the dispatcher sends NOTHING on fast-path
    // miss.  A tiny non-blocking poll surfaces any premature reply.
    {
        auto premature = cons_client.receive(std::chrono::milliseconds{50});
        ASSERT_FALSE(premature.has_value())
            << "consumer received a reply BEFORE producer sent "
               "APPLIED_REQ — deferred-reply contract broken.  Frame: "
            << (premature->first + " " + premature->second.dump());
    }

    // ── Producer sends APPLIED_REQ → broker drains the queue ──
    // `request()` filters out any NOTIFY frames that the producer's
    // DEALER queued up (from CONSUMER_REG fan-out + the wait-path
    // targeted NOTIFY) so we cleanly see the APPLIED_ACK.
    {
        nlohmann::json applied;
        applied["channel_name"]      = channel_name;
        applied["producer_role_uid"] = producer_uid;
        applied["applied_version"]   = 1u;
        applied["instance_id"]       = 1u;

        auto reply = prod_client.request(
            "CHANNEL_AUTH_APPLIED_REQ", applied,
            "CHANNEL_AUTH_APPLIED_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "producer APPLIED_REQ: no ACK";
        ASSERT_EQ(reply->value("status", ""), "ok") << reply->dump();
        ASSERT_EQ(reply->value("applied_version", 0u), 1u);
    }

    // ── Consumer receives the deferred CONSUMER_ATTACH_ACK_ZMQ ──
    // The queue drain in handle_channel_auth_applied_req routes the
    // reply back to the consumer's ROUTER identity captured at
    // enqueue time (§5.4 step d).  Consumer's DEALER only receives
    // ACK-side frames (NOTIFYs are targeted at producers), so a
    // plain receive() is sufficient.
    {
        auto reply = cons_client.receive(
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "consumer did not receive deferred ATTACH_ACK — "
               "wait-path drain (§5.4 step d) likely broken.";
        EXPECT_EQ(reply->first, "CONSUMER_ATTACH_ACK_ZMQ")
            << "got msg_type='" << reply->first << "'";
        EXPECT_EQ(reply->second.value("status", ""), "success")
            << reply->second.dump();
        EXPECT_EQ(reply->second.value("channel_name", ""), channel_name);
        EXPECT_EQ(reply->second.value("producer_role_uid", ""), producer_uid);
    }

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── Wait-path drain: producer disconnects (§5.4 producer-not-live) ────
//
// Setup:
//   - producers P1 and P2 both REG + heartbeat on channel K
//     (multi-producer channel per HEP-CORE-0023 §2.1.1).
//   - consumer C REGs → channel_version[K]: 0 → 1.
// Wait-path + drain:
//   - consumer sends CONSUMER_ATTACH_REQ_ZMQ(K, P1) → fast-path miss
//     → enqueued in pending_attach_queue_[K][P1] + return pending.
//   - P1 sends DEREG_REQ → broker: non-last-producer branch (P2 keeps
//     channel alive), calls drain_pending_attach_queue_for_producer_
//     denied_(reason="producer_not_live") → sends deferred denied
//     reply to consumer's ROUTER identity.
//   - consumer's DEALER receives {status="denied",
//     reason="producer_not_live"}.

TEST_F(Pattern4AttachCoordinationTest, WaitPathDrainOnProducerDisconnect)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer1_uid = "prod.p1.uid00000001";
    const std::string producer2_uid = "prod.p2.uid00000002";
    const std::string consumer_uid  = "cons.c.uid00000003";
    const std::string channel_name  = "ch.attach.drain_pnotlive";

    const fs::path temp_dir = make_test_temp_dir("attach_drain_pnotlive");
    const auto setup = make_pattern4_setup(
        {producer1_uid, producer2_uid, consumer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const int producer1_data_port = pick_unused_port();
    const int producer2_data_port = pick_unused_port();

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &p1_kp   = setup.curve.role(producer1_uid);
    const auto &p2_kp   = setup.curve.role(producer2_uid);
    const auto &cons_kp = setup.curve.role(consumer_uid);

    auto build_client = [&](const pylabhub::tests::CurveKeypair &kp) {
        BrokerWireClient::Config c;
        c.broker_endpoint = setup.broker_endpoint;
        c.broker_pubkey   = setup.curve.hub.public_z85;
        c.client_pubkey   = kp.public_z85;
        c.client_seckey   = kp.secret_z85;
        return BrokerWireClient{ctx, c};
    };
    auto p1_client   = build_client(p1_kp);
    auto p2_client   = build_client(p2_kp);
    auto cons_client = build_client(cons_kp);

    auto reg_and_heartbeat = [&](BrokerWireClient &client,
                                  const std::string &uid,
                                  const std::string &pubkey,
                                  int data_port) {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = uid;
        in.role_name         = uid.substr(uid.rfind('.') + 1);
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(data_port);
        in.zmq_pubkey        = pubkey;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        auto reply = client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();

        nlohmann::json hb;
        hb["channel_name"] = channel_name;
        hb["role_uid"]     = uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = 1;
        client.send("HEARTBEAT_REQ", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    };
    reg_and_heartbeat(p1_client, producer1_uid, p1_kp.public_z85, producer1_data_port);
    reg_and_heartbeat(p2_client, producer2_uid, p2_kp.public_z85, producer2_data_port);

    // Consumer REG — bumps channel_version.
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel    = channel_name;
        in.role_uid   = consumer_uid;
        in.role_name  = "c";
        in.zmq_pubkey = cons_kp.public_z85;
        auto payload = pylabhub::hub::build_consumer_reg_payload(in);
        auto reply = cons_client.request(
            "CONSUMER_REG_REQ", payload, "CONSUMER_REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // Wait-path: consumer targets P1.  Fast-path misses because
    // neither producer has sent APPLIED_REQ.
    {
        nlohmann::json attach;
        attach["channel_name"]      = channel_name;
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = producer1_uid;
        cons_client.send("CONSUMER_ATTACH_REQ_ZMQ", attach);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // P1 deregisters — non-last-producer path (P2 still alive) →
    // producer-disconnect drain fires the deferred denied reply.
    {
        nlohmann::json dereg;
        dereg["channel_name"] = channel_name;
        dereg["role_uid"]     = producer1_uid;
        dereg["producer_pid"] = pylabhub::platform::get_pid();
        auto reply = p1_client.request(
            "DEREG_REQ", dereg, "DEREG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // Consumer receives the drain-denied reply.
    {
        auto reply = cons_client.receive(
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "consumer did not receive drain-denied reply — "
               "producer-disconnect drain (§5.4 producer_not_live) "
               "likely broken.";
        EXPECT_EQ(reply->first, "CONSUMER_ATTACH_ACK_ZMQ");
        EXPECT_EQ(reply->second.value("status", ""), "denied")
            << reply->second.dump();
        EXPECT_EQ(reply->second.value("reason", ""), "producer_not_live")
            << reply->second.dump();
    }

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── Wait-path drain: channel closes (§5.4 channel_closing) ────────────
//
// Setup:
//   - single producer P REGs + heartbeats on channel K.
//   - consumer C REGs → channel_version[K]: 0 → 1.
// Wait-path + drain:
//   - consumer sends CONSUMER_ATTACH_REQ_ZMQ → wait-path enqueue.
//   - P sends DEREG_REQ → LAST-producer branch → channel teardown +
//     drain_pending_attach_queue_for_channel_denied_(reason=
//     "channel_closing") → sends deferred denied reply to consumer.
//   - consumer's DEALER receives {status="denied",
//     reason="channel_closing"}.

TEST_F(Pattern4AttachCoordinationTest, WaitPathDrainOnChannelClose)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = "prod.p.uid00000001";
    const std::string consumer_uid = "cons.c.uid00000002";
    const std::string channel_name = "ch.attach.drain_chclose";

    const fs::path temp_dir = make_test_temp_dir("attach_drain_chclose");
    const auto setup = make_pattern4_setup({producer_uid, consumer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const int producer_data_port = pick_unused_port();

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &prod_kp = setup.curve.role(producer_uid);
    const auto &cons_kp = setup.curve.role(consumer_uid);

    BrokerWireClient::Config prod_cfg;
    prod_cfg.broker_endpoint = setup.broker_endpoint;
    prod_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    prod_cfg.client_pubkey   = prod_kp.public_z85;
    prod_cfg.client_seckey   = prod_kp.secret_z85;
    BrokerWireClient prod_client(ctx, prod_cfg);

    BrokerWireClient::Config cons_cfg;
    cons_cfg.broker_endpoint = setup.broker_endpoint;
    cons_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cons_cfg.client_pubkey   = cons_kp.public_z85;
    cons_cfg.client_seckey   = cons_kp.secret_z85;
    BrokerWireClient cons_client(ctx, cons_cfg);

    // Producer REG + heartbeat.
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = producer_uid;
        in.role_name         = "p";
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(producer_data_port);
        in.zmq_pubkey        = prod_kp.public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        auto reply = prod_client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }
    {
        nlohmann::json hb;
        hb["channel_name"] = channel_name;
        hb["role_uid"]     = producer_uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = 1;
        prod_client.send("HEARTBEAT_REQ", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // Consumer REG.
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel    = channel_name;
        in.role_uid   = consumer_uid;
        in.role_name  = "c";
        in.zmq_pubkey = cons_kp.public_z85;
        auto payload = pylabhub::hub::build_consumer_reg_payload(in);
        auto reply = cons_client.request(
            "CONSUMER_REG_REQ", payload, "CONSUMER_REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // Wait-path enqueue.
    {
        nlohmann::json attach;
        attach["channel_name"]      = channel_name;
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = producer_uid;
        cons_client.send("CONSUMER_ATTACH_REQ_ZMQ", attach);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // Producer DEREG — LAST-producer branch → channel closes → drain.
    {
        nlohmann::json dereg;
        dereg["channel_name"] = channel_name;
        dereg["role_uid"]     = producer_uid;
        dereg["producer_pid"] = pylabhub::platform::get_pid();
        auto reply = prod_client.request(
            "DEREG_REQ", dereg, "DEREG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // Consumer receives the channel-close drain reply.
    // On last-producer teardown the broker ALSO fires
    // CHANNEL_CLOSING_NOTIFY to consumers of K (HEP-CORE-0023 §2.1);
    // that frame arrives before the deferred CONSUMER_ATTACH_ACK_ZMQ,
    // so we loop until we find the ACK we're actually pinning.
    {
        std::optional<std::pair<std::string, nlohmann::json>> reply;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{kLongTimeoutMs};
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            auto next = cons_client.receive(remaining);
            if (!next.has_value())
                break;
            if (next->first == "CONSUMER_ATTACH_ACK_ZMQ")
            {
                reply = std::move(next);
                break;
            }
            // Silently drop other frames (e.g. CHANNEL_CLOSING_NOTIFY).
        }
        ASSERT_TRUE(reply.has_value())
            << "consumer did not receive drain-denied reply — "
               "channel-close drain (§5.4 channel_closing) likely broken.";
        EXPECT_EQ(reply->second.value("status", ""), "denied")
            << reply->second.dump();
        EXPECT_EQ(reply->second.value("reason", ""), "channel_closing")
            << reply->second.dump();
    }

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── Wait-path timeout (§5.4 pending-entry timeout + §5.6 budget) ──────
//
// Setup:
//   - producer P REGs + heartbeats + is caught up to channel_version=0.
//   - consumer C REGs → channel_version[K] = 1.
// Timeout:
//   - consumer sends CONSUMER_ATTACH_REQ_ZMQ → wait-path enqueue.
//   - producer NEVER sends APPLIED_REQ.
//   - after `producer_apply_wait_ms` (default 3000ms) elapses,
//     broker's `sweep_pending_attach_timeouts_` drains the entry
//     with {status="timeout",
//     reason="producer_did_not_confirm_within_budget"}.
//   - consumer receives the timeout reply.
//
// Wall-clock: ~3.5s (budget + ~500ms sweep cadence at heartbeat_interval).

TEST_F(Pattern4AttachCoordinationTest, WaitPathTimeoutOnMissingAppliedReq)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = "prod.p.uid00000001";
    const std::string consumer_uid = "cons.c.uid00000002";
    const std::string channel_name = "ch.attach.timeout";

    const fs::path temp_dir = make_test_temp_dir("attach_timeout");
    const auto setup = make_pattern4_setup({producer_uid, consumer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const int producer_data_port = pick_unused_port();

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &prod_kp = setup.curve.role(producer_uid);
    const auto &cons_kp = setup.curve.role(consumer_uid);

    BrokerWireClient::Config prod_cfg;
    prod_cfg.broker_endpoint = setup.broker_endpoint;
    prod_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    prod_cfg.client_pubkey   = prod_kp.public_z85;
    prod_cfg.client_seckey   = prod_kp.secret_z85;
    BrokerWireClient prod_client(ctx, prod_cfg);

    BrokerWireClient::Config cons_cfg;
    cons_cfg.broker_endpoint = setup.broker_endpoint;
    cons_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    cons_cfg.client_pubkey   = cons_kp.public_z85;
    cons_cfg.client_seckey   = cons_kp.secret_z85;
    BrokerWireClient cons_client(ctx, cons_cfg);

    // Producer REG + heartbeat.
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = producer_uid;
        in.role_name         = "p";
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(producer_data_port);
        in.zmq_pubkey        = prod_kp.public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        auto reply = prod_client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }
    {
        nlohmann::json hb;
        hb["channel_name"] = channel_name;
        hb["role_uid"]     = producer_uid;
        hb["role_type"]    = "producer";
        hb["producer_pid"] = 1;
        prod_client.send("HEARTBEAT_REQ", hb);
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // Consumer REG.
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel    = channel_name;
        in.role_uid   = consumer_uid;
        in.role_name  = "c";
        in.zmq_pubkey = cons_kp.public_z85;
        auto payload = pylabhub::hub::build_consumer_reg_payload(in);
        auto reply = cons_client.request(
            "CONSUMER_REG_REQ", payload, "CONSUMER_REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // Wait-path enqueue — producer will NEVER send APPLIED_REQ.
    {
        nlohmann::json attach;
        attach["channel_name"]      = channel_name;
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = producer_uid;
        cons_client.send("CONSUMER_ATTACH_REQ_ZMQ", attach);
    }

    // Consumer polls until the timeout sweep drains the entry.
    // Budget: default producer_apply_wait_ms=3000ms + heartbeat sweep
    // cadence (500ms) → observable at ~3.5s.  Poll for 6s to
    // absorb CI jitter.
    {
        auto reply = cons_client.receive(std::chrono::milliseconds{6000});
        ASSERT_TRUE(reply.has_value())
            << "consumer did not receive timeout reply within 6s — "
               "sweep_pending_attach_timeouts_ likely broken.";
        EXPECT_EQ(reply->first, "CONSUMER_ATTACH_ACK_ZMQ");
        EXPECT_EQ(reply->second.value("status", ""), "timeout")
            << reply->second.dump();
        EXPECT_EQ(reply->second.value("reason", ""),
                   "producer_did_not_confirm_within_budget")
            << reply->second.dump();
    }

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

// ─── Stale-instance guard (§5.4 step a on APPLIED_REQ) ──────────────────
//
// Setup: producer P REGs (instance[P] = 1).
// Stale attempt:
//   - P sends CHANNEL_AUTH_APPLIED_REQ with instance_id=999 (deliberately
//     wrong).
//   - Broker: incoming_instance (999) != current_instance (1) →
//     returns ERROR with error_code="STALE_INSTANCE".  Silently drops
//     the state advance — this is the guard that HEP-CORE-0042 §5.2
//     uses to protect against APPLIED_REQ from crashed prior
//     instances still in flight.
//
// (Realistic reproduction would DEREG + re-REG to bump instance to 2,
// then send with instance_id=1 — but the guard fires on any mismatch,
// so the simpler wrong-value pattern is sufficient to pin behaviour.)

TEST_F(Pattern4AttachCoordinationTest, StaleInstanceGuardOnAppliedReq)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = "prod.p.uid00000001";
    const std::string channel_name = "ch.attach.stale_instance";

    const fs::path temp_dir = make_test_temp_dir("attach_stale_instance");
    const auto setup = make_pattern4_setup({producer_uid});
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const int producer_data_port = pick_unused_port();

    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                             {temp_dir.string()});
    expect_log(broker, "Pattern4Broker: bound endpoint",
                std::chrono::milliseconds{kMidTimeoutMs});

    zmq::context_t ctx;
    const auto &prod_kp = setup.curve.role(producer_uid);

    BrokerWireClient::Config prod_cfg;
    prod_cfg.broker_endpoint = setup.broker_endpoint;
    prod_cfg.broker_pubkey   = setup.curve.hub.public_z85;
    prod_cfg.client_pubkey   = prod_kp.public_z85;
    prod_cfg.client_seckey   = prod_kp.secret_z85;
    BrokerWireClient prod_client(ctx, prod_cfg);

    // Producer REG.  Broker assigns instance[P] = 1 on first REG.
    {
        pylabhub::hub::ProducerRegInputs in;
        in.channel           = channel_name;
        in.role_uid          = producer_uid;
        in.role_name         = "p";
        in.role_type         = "producer";
        in.is_zmq_transport  = true;
        in.zmq_node_endpoint = "tcp://127.0.0.1:" + std::to_string(producer_data_port);
        in.zmq_pubkey        = prod_kp.public_z85;
        auto payload = pylabhub::hub::build_producer_reg_payload(in);
        auto reply = prod_client.request(
            "REG_REQ", payload, "REG_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value());
        ASSERT_EQ(reply->value("status", ""), "success") << reply->dump();
    }

    // Send APPLIED_REQ with a stale instance_id.  Broker returns ERROR
    // with error_code="STALE_INSTANCE" per handle_channel_auth_applied_req.
    // `request()` treats ERROR as an early-return outcome (Phase 2.4a
    // close-out), so the reply body IS the ERROR envelope.
    {
        nlohmann::json applied;
        applied["channel_name"]      = channel_name;
        applied["producer_role_uid"] = producer_uid;
        applied["applied_version"]   = 1u;
        applied["instance_id"]       = 999u;  // deliberately wrong

        auto reply = prod_client.request(
            "CHANNEL_AUTH_APPLIED_REQ", applied,
            "CHANNEL_AUTH_APPLIED_ACK",
            std::chrono::milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "producer APPLIED_REQ: no reply — stale-instance guard "
               "should surface as an ERROR, not silence.";
        // The reply IS the ERROR body (status="error",
        // error_code="STALE_INSTANCE").
        EXPECT_EQ(reply->value("status", ""), "error") << reply->dump();
        EXPECT_EQ(reply->value("error_code", ""), "STALE_INSTANCE")
            << reply->dump();
    }

    broker.signal_quit();
    ExpectWorkerOk(broker);
}

} // namespace
