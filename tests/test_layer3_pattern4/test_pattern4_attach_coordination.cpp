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

} // namespace
