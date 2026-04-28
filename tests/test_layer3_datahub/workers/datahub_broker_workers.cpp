// tests/test_layer3_datahub/workers/datahub_broker_workers.cpp
// Phase C — BrokerService integration tests.
#include "datahub_broker_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <cppzmq/zmq.hpp>
#include <cppzmq/zmq_addon.hpp>
#include <zmq.h>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pylabhub::tests::helper;
using namespace pylabhub::broker;
using namespace pylabhub::hub;

namespace pylabhub::tests::worker::broker
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetDataBlockModule(); }
static auto zmq_module() { return ::pylabhub::hub::GetZMQContextModule(); }

// ============================================================================
// File-local helpers
// ============================================================================

namespace
{

// -----------------------------------------------------------------------------
// BrokerHandle: owns BrokerService + its background thread.
// start_broker_in_thread() blocks until on_ready fires (broker is bound).
// -----------------------------------------------------------------------------
struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread thread;
    std::string endpoint;
    std::string pubkey;

    void stop_and_join()
    {
        service->stop();
        if (thread.joinable())
        {
            thread.join();
        }
    }
};

BrokerHandle start_broker_in_thread(BrokerService::Config cfg)
{
    using ReadyInfo = std::pair<std::string, std::string>; // (endpoint, pubkey)
    auto ready_promise = std::make_shared<std::promise<ReadyInfo>>();
    auto ready_future = ready_promise->get_future();

    cfg.on_ready = [ready_promise](const std::string& ep, const std::string& pk)
    {
        ready_promise->set_value({ep, pk});
    };

    auto service = std::make_unique<BrokerService>(std::move(cfg));
    BrokerService* raw_ptr = service.get();
    std::thread t([raw_ptr]() { raw_ptr->run(); });

    auto info = ready_future.get(); // blocks until broker is bound and listening

    BrokerHandle handle;
    handle.service = std::move(service);
    handle.thread = std::move(t);
    handle.endpoint = info.first;
    handle.pubkey = info.second;
    return handle;
}

// -----------------------------------------------------------------------------
// raw_req: Sends a two-frame [msg_type, payload_json] to a DEALER socket and
// returns the parsed response body JSON.  Optionally enables CurveZMQ when
// server_pubkey is a 40-char Z85 string.
//
// Returns {} (null JSON) on timeout or receive error.
// -----------------------------------------------------------------------------
nlohmann::json raw_req(const std::string& endpoint,
                       const std::string& msg_type,
                       const nlohmann::json& payload,
                       int timeout_ms = 2000,
                       const std::string& server_pubkey = "")
{
    constexpr size_t kZ85KeyLen = 40;
    constexpr size_t kZ85BufLen = 41;

    zmq::context_t ctx(1);
    zmq::socket_t dealer(ctx, zmq::socket_type::dealer);

    if (server_pubkey.size() == kZ85KeyLen)
    {
        // Generate ephemeral client keypair for this request.
        std::array<char, kZ85BufLen> client_pub{};
        std::array<char, kZ85BufLen> client_sec{};
        if (zmq_curve_keypair(client_pub.data(), client_sec.data()) != 0)
        {
            return {};
        }
        dealer.set(zmq::sockopt::curve_serverkey, server_pubkey);
        dealer.set(zmq::sockopt::curve_publickey, std::string(client_pub.data(), kZ85KeyLen));
        dealer.set(zmq::sockopt::curve_secretkey, std::string(client_sec.data(), kZ85KeyLen));
    }

    dealer.connect(endpoint);

    // Frame 0: 'C' (control), Frame 1: type string, Frame 2: JSON body
    static constexpr char kCtrl = 'C';
    const std::string payload_str = payload.dump();
    std::vector<zmq::const_buffer> send_frames = {zmq::buffer(&kCtrl, 1),
                                                  zmq::buffer(msg_type),
                                                  zmq::buffer(payload_str)};
    if (!zmq::send_multipart(dealer, send_frames))
    {
        return {};
    }

    std::vector<zmq::pollitem_t> items = {{dealer.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, std::chrono::milliseconds(timeout_ms));
    if ((items[0].revents & ZMQ_POLLIN) == 0)
    {
        return {}; // timeout
    }

    std::vector<zmq::message_t> recv_frames;
    auto result = zmq::recv_multipart(dealer, std::back_inserter(recv_frames));
    // Reply layout: ['C', ack_type_string, body_JSON]
    if (!result || recv_frames.size() < 3)
    {
        return {};
    }

    // recv_frames[0] = 'C', recv_frames[1] = ack_type, recv_frames[2] = body JSON
    try
    {
        return nlohmann::json::parse(recv_frames.back().to_string());
    }
    catch (const nlohmann::json::exception&)
    {
        return {};
    }
}

// Hex string of N zero bytes (for use as a schema_hash in JSON payloads).
std::string zero_hex(size_t bytes = 32)
{
    return std::string(bytes * 2, '0');
}

// Hex string of N 'aa' bytes (for a different schema_hash).
std::string aa_hex(size_t bytes = 32)
{
    return std::string(bytes * 2, 'a');
}

} // anonymous namespace

// ============================================================================
// broker_reg_disc_happy_path — full REG/DISC round-trip via BrokerRequestComm
// ============================================================================

int broker_reg_disc_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.ch1";
            const std::string uid     = "prod.broker.ch1.uid00000001";

            BrokerRequestComm brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = uid;
            ASSERT_TRUE(brc.connect(brc_cfg));
            std::atomic<bool> running{true};
            std::thread t([&] { brc.run_poll_loop([&] { return running.load(); }); });

            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = ::getpid();
            reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_pubkey"]        = "";
            reg_opts["role_uid"]          = uid;
            reg_opts["shm_name"]          = "broker_reg_disc.shm";
            reg_opts["schema_version"]    = 7;
            auto reg = brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel must succeed";

            brc.send_heartbeat(channel, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            auto disc = brc.discover_channel(channel, {}, 5000);
            ASSERT_TRUE(disc.has_value()) << "discover_channel must find registered channel";
            EXPECT_EQ(disc->value("shm_name", ""), "broker_reg_disc.shm");
            EXPECT_EQ(disc->value("schema_version", 0), 7);

            running.store(false);
            brc.stop();
            if (t.joinable()) t.join();
            brc.disconnect();
            broker.stop_and_join();
        },
        "broker.broker_reg_disc_happy_path",
        logger_module(), crypto_module(), hub_module(), zmq_module());
}

// ============================================================================
// broker_schema_mismatch — re-register same channel with different schema_hash
// ============================================================================

int broker_schema_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.mismatch.ch";
            const uint64_t pid = pylabhub::platform::get_pid();

            // First registration — succeeds
            nlohmann::json req1;
            req1["channel_name"] = channel;
            req1["shm_name"] = "shm_mismatch";
            req1["schema_hash"] = zero_hex();
            req1["schema_version"] = 1;
            req1["producer_pid"] = pid;
            req1["producer_hostname"] = "localhost";

            nlohmann::json resp1 = raw_req(broker.endpoint, "REG_REQ", req1);
            ASSERT_FALSE(resp1.is_null()) << "raw_req timed out on first REG_REQ";
            EXPECT_EQ(resp1.value("status", std::string("")), "success")
                << "First registration must succeed; got: " << resp1.dump();

            // Second registration — different schema_hash → SCHEMA_MISMATCH
            nlohmann::json req2 = req1;
            req2["schema_hash"] = aa_hex(); // different hash
            nlohmann::json resp2 = raw_req(broker.endpoint, "REG_REQ", req2);
            ASSERT_FALSE(resp2.is_null()) << "raw_req timed out on second REG_REQ";
            EXPECT_EQ(resp2.value("status", std::string("")), "error")
                << "Second registration with mismatched hash must be rejected";
            EXPECT_EQ(resp2.value("error_code", std::string("")), "SCHEMA_MISMATCH")
                << "Error code must be SCHEMA_MISMATCH; got: " << resp2.dump();

            broker.stop_and_join();
        },
        "broker.broker_schema_mismatch",
        logger_module(), zmq_module());
}

// ============================================================================
// broker_channel_not_found — discover unknown channel → BRC returns nullopt
// ============================================================================

int broker_channel_not_found()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            BrokerRequestComm brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = "prod.querier.notfound.uid00000001";
            ASSERT_TRUE(brc.connect(brc_cfg));
            std::atomic<bool> running{true};
            std::thread t([&] { brc.run_poll_loop([&] { return running.load(); }); });

            auto info = brc.discover_channel("no.such.channel", {}, 2000);
            EXPECT_FALSE(info.has_value())
                << "discover_channel for unknown channel must return nullopt";

            running.store(false);
            brc.stop();
            if (t.joinable()) t.join();
            brc.disconnect();
            broker.stop_and_join();
        },
        "broker.broker_channel_not_found",
        logger_module(), crypto_module(), hub_module(), zmq_module());
}

// ============================================================================
// broker_dereg_happy_path — register, deregister (correct pid), then not found
// ============================================================================

int broker_dereg_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = true;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.dereg.ch";
            const std::string uid     = "prod.dereg.ch.uid00000001";

            BrokerRequestComm brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = uid;
            ASSERT_TRUE(brc.connect(brc_cfg));
            std::atomic<bool> running{true};
            std::thread t([&] { brc.run_poll_loop([&] { return running.load(); }); });

            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = ::getpid();
            reg_opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
            reg_opts["zmq_pubkey"]        = "";
            reg_opts["role_uid"]          = uid;
            auto reg = brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel must succeed";

            brc.send_heartbeat(channel, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Verify channel is discoverable
            auto found = brc.discover_channel(channel, {}, 5000);
            ASSERT_TRUE(found.has_value()) << "Channel must be discoverable before deregister";

            // Deregister
            EXPECT_TRUE(brc.deregister_channel(channel));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // After deregistration, discover must return nullopt
            auto after_dereg = brc.discover_channel(channel, {}, 1000);
            EXPECT_FALSE(after_dereg.has_value())
                << "discover_channel must return nullopt after deregistration";

            running.store(false);
            brc.stop();
            if (t.joinable()) t.join();
            brc.disconnect();
            broker.stop_and_join();
        },
        "broker.broker_dereg_happy_path",
        logger_module(), crypto_module(), hub_module(), zmq_module());
}

// ============================================================================
// broker_dereg_pid_mismatch — deregister with wrong pid → NOT_REGISTERED,
//                             channel still discoverable
// ============================================================================

int broker_dereg_pid_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.pid_mismatch.ch";
            const uint64_t correct_pid = 55555;
            const uint64_t wrong_pid = 99999;

            // Register via raw ZMQ.
            nlohmann::json reg_req;
            reg_req["channel_name"] = channel;
            reg_req["shm_name"] = "shm_pid_mismatch";
            reg_req["schema_hash"] = zero_hex();
            reg_req["schema_version"] = 1;
            reg_req["producer_pid"] = correct_pid;
            reg_req["producer_hostname"] = "localhost";
            nlohmann::json reg_resp = raw_req(broker.endpoint, "REG_REQ", reg_req);
            ASSERT_FALSE(reg_resp.is_null()) << "REG_REQ timed out";
            EXPECT_EQ(reg_resp.value("status", std::string("")), "success");

            // Send HEARTBEAT_REQ to transition channel from PendingReady → Ready.
            // HEARTBEAT_REQ is fire-and-forget (broker sends no reply); raw_req times
            // out quickly and returns empty json, which we discard.
            nlohmann::json hb_req;
            hb_req["channel_name"] = channel;
            hb_req["producer_pid"] = correct_pid;
            raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100);

            // DEREG_REQ with wrong pid → NOT_REGISTERED error.
            nlohmann::json dereg_req;
            dereg_req["channel_name"] = channel;
            dereg_req["producer_pid"] = wrong_pid;
            nlohmann::json dereg_resp = raw_req(broker.endpoint, "DEREG_REQ", dereg_req);
            ASSERT_FALSE(dereg_resp.is_null()) << "DEREG_REQ timed out";
            EXPECT_EQ(dereg_resp.value("status", std::string("")), "error")
                << "DEREG_REQ with wrong pid must be rejected; got: " << dereg_resp.dump();
            EXPECT_EQ(dereg_resp.value("error_code", std::string("")), "NOT_REGISTERED")
                << "Error code must be NOT_REGISTERED; got: " << dereg_resp.dump();

            // Channel still discoverable via DISC_REQ.
            nlohmann::json disc_req;
            disc_req["channel_name"] = channel;
            nlohmann::json disc_resp = raw_req(broker.endpoint, "DISC_REQ", disc_req);
            ASSERT_FALSE(disc_resp.is_null()) << "DISC_REQ timed out";
            EXPECT_EQ(disc_resp.value("status", std::string("")), "success")
                << "Channel must still be registered after pid-mismatch deregister attempt";
            EXPECT_EQ(disc_resp.value("shm_name", std::string("")), "shm_pid_mismatch");

            broker.stop_and_join();
        },
        "broker.broker_dereg_pid_mismatch",
        logger_module(), zmq_module());
}

// ============================================================================
// HEP-CORE-0034 Phase 3a — schema record + citation wire paths
// ============================================================================

namespace
{

/// Build a baseline REG_REQ payload that the broker accepts.  Tests below
/// add or override fields (notably `schema_packing`, `schema_id`, `schema_hash`).
nlohmann::json baseline_reg_req(const std::string &channel,
                                const std::string &uid,
                                const std::string &shm_name = "sch.shm")
{
    nlohmann::json req;
    req["channel_name"]      = channel;
    req["shm_name"]          = shm_name;
    req["schema_version"]    = 1;
    req["producer_pid"]      = pylabhub::platform::get_pid();
    req["producer_hostname"] = "localhost";
    req["role_uid"]          = uid;
    req["role_name"]         = "test_producer";
    return req;
}

} // anonymous namespace

// ── REG_REQ creates schema record (path B, owner=role_uid) ──────────────────

int broker_sch_record_path_b_created()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.path_b";
            const std::string uid     = "prod.broker.sch_b.uid00000001";
            const std::string sid     = "$lab.sch_b.frame.v1";

            auto req = baseline_reg_req(channel, uid);
            req["schema_id"]      = sid;
            req["schema_hash"]    = aa_hex();    // 32 bytes of 0xaa
            req["schema_packing"] = "aligned";
            req["schema_blds"]    = "ts:f64;value:f32";

            auto resp = raw_req(broker.endpoint, "REG_REQ", req);
            ASSERT_FALSE(resp.is_null()) << "raw_req timed out";
            EXPECT_EQ(resp.value("status", std::string{}), "success")
                << "REG_REQ with schema_packing must succeed; got: " << resp.dump();

            // Re-register with identical fields → still success.  At the
            // registry level this is kIdempotent; at the channel level it
            // is a same-hash re-registration that preserves consumers.
            auto resp2 = raw_req(broker.endpoint, "REG_REQ", req);
            EXPECT_EQ(resp2.value("status", std::string{}), "success")
                << "Idempotent re-register must succeed; got: " << resp2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_record_path_b_created",
        logger_module(), zmq_module());
}

// ── Same uid+schema_id, different hash, both with schema_packing → reject ───

int broker_sch_record_hash_mismatch_self()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.mismatch_self";
            const std::string uid     = "prod.broker.sch_mm.uid00000001";
            const std::string sid     = "$lab.sch_mm.frame.v1";

            auto req1 = baseline_reg_req(channel, uid);
            req1["schema_id"]      = sid;
            req1["schema_hash"]    = aa_hex();
            req1["schema_packing"] = "aligned";
            req1["schema_blds"]    = "ts:f64;value:f32";
            auto r1 = raw_req(broker.endpoint, "REG_REQ", req1);
            ASSERT_EQ(r1.value("status", std::string{}), "success") << r1.dump();

            // Second REG_REQ — same (owner, schema_id) but different hash
            // bytes → HubState rejects with kHashMismatchSelf, broker
            // returns SCHEMA_HASH_MISMATCH_SELF (HEP-CORE-0034 §10.4).
            auto req2 = req1;
            req2["schema_hash"] = zero_hex();  // different from aa_hex
            auto r2 = raw_req(broker.endpoint, "REG_REQ", req2);
            ASSERT_FALSE(r2.is_null()) << "raw_req timed out";
            EXPECT_EQ(r2.value("status", std::string{}), "error")
                << "Second REG_REQ with different hash must NACK; got: " << r2.dump();
            EXPECT_EQ(r2.value("error_code", std::string{}), "SCHEMA_HASH_MISMATCH_SELF")
                << "Error code must be SCHEMA_HASH_MISMATCH_SELF; got: " << r2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_record_hash_mismatch_self",
        logger_module(), zmq_module());
}

// ── Consumer citation: matching expected_packing → success ──────────────────

int broker_sch_consumer_citation_match()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.cons_ok";
            const std::string p_uid   = "prod.broker.sch_cok.uid00000001";
            const std::string c_uid   = "cons.broker.sch_cok.uid00000002";
            const std::string sid     = "$lab.sch_cok.frame.v1";
            const std::string hash    = aa_hex();

            auto reg = baseline_reg_req(channel, p_uid);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = "aligned";
            reg["schema_blds"]    = "ts:f64;value:f32";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            nlohmann::json cons_req;
            cons_req["channel_name"]         = channel;
            cons_req["consumer_uid"]         = c_uid;
            cons_req["consumer_name"]        = "test_consumer";
            cons_req["consumer_pid"]         = pylabhub::platform::get_pid();
            cons_req["expected_schema_hash"] = hash;
            cons_req["expected_packing"]     = "aligned";
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons_req);
            ASSERT_FALSE(cr.is_null()) << "raw_req timed out";
            EXPECT_EQ(cr.value("status", std::string{}), "success")
                << "Citation match must succeed; got: " << cr.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_consumer_citation_match",
        logger_module(), zmq_module());
}

// ── Consumer citation: WRONG expected_packing → SCHEMA_CITATION_REJECTED ────

int broker_sch_consumer_citation_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.cons_bad";
            const std::string p_uid   = "prod.broker.sch_cbad.uid00000001";
            const std::string c_uid   = "cons.broker.sch_cbad.uid00000002";
            const std::string sid     = "$lab.sch_cbad.frame.v1";
            const std::string hash    = aa_hex();

            auto reg = baseline_reg_req(channel, p_uid);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = "aligned";
            reg["schema_blds"]    = "ts:f64;value:f32";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Consumer expects same hash but DIFFERENT packing → citation
            // resolves to fingerprint_mismatch, broker NACKs with
            // SCHEMA_CITATION_REJECTED.
            nlohmann::json cons_req;
            cons_req["channel_name"]         = channel;
            cons_req["consumer_uid"]         = c_uid;
            cons_req["consumer_name"]        = "test_consumer";
            cons_req["consumer_pid"]         = pylabhub::platform::get_pid();
            cons_req["expected_schema_hash"] = hash;
            cons_req["expected_packing"]     = "packed";  // diverges
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons_req);
            ASSERT_FALSE(cr.is_null()) << "raw_req timed out";
            EXPECT_EQ(cr.value("status", std::string{}), "error")
                << "Mismatched packing must be NACKed; got: " << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}),
                      "SCHEMA_CITATION_REJECTED")
                << "Error code must be SCHEMA_CITATION_REJECTED; got: " << cr.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_consumer_citation_mismatch",
        logger_module(), zmq_module());
}

// ── REG_REQ without schema_packing → no record created (backward compat) ────

int broker_sch_no_packing_backward_compat()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.bc";
            const std::string uid     = "prod.broker.sch_bc.uid00000001";

            // No schema_packing → new schema-record block is skipped.
            auto req1 = baseline_reg_req(channel, uid);
            req1["schema_hash"] = zero_hex();
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", req1)
                          .value("status", std::string{}),
                      "success");

            // Re-register with different hash, again without schema_packing.
            // The OLD channel-mismatch path runs (SCHEMA_MISMATCH), proving
            // the new HEP-0034 path was not taken (would have been
            // SCHEMA_HASH_MISMATCH_SELF instead).
            auto req2 = req1;
            req2["schema_hash"] = aa_hex();
            auto r2 = raw_req(broker.endpoint, "REG_REQ", req2);
            ASSERT_EQ(r2.value("status", std::string{}), "error") << r2.dump();
            EXPECT_EQ(r2.value("error_code", std::string{}), "SCHEMA_MISMATCH")
                << "Backward-compat REG_REQ must use the legacy "
                   "SCHEMA_MISMATCH path, not the new "
                   "SCHEMA_HASH_MISMATCH_SELF; got: "
                << r2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_no_packing_backward_compat",
        logger_module(), zmq_module());
}

// ── SCHEMA_REQ owner+id keying (HEP-0034 §10.3) ─────────────────────────────

int broker_sch_schema_req_owner_id()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.schreq";
            const std::string uid     = "prod.broker.schreq.uid00000001";
            const std::string sid     = "$lab.schreq.frame.v1";
            const std::string hash    = aa_hex();
            const std::string blds    = "ts:f64;value:f32";

            auto reg = baseline_reg_req(channel, uid);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = "aligned";
            reg["schema_blds"]    = blds;
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // New form: (owner, schema_id) — direct registry lookup.
            nlohmann::json sreq;
            sreq["owner"]     = uid;
            sreq["schema_id"] = sid;
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
            ASSERT_FALSE(sresp.is_null());
            EXPECT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
            EXPECT_EQ(sresp.value("owner", std::string{}), uid);
            EXPECT_EQ(sresp.value("schema_id", std::string{}), sid);
            EXPECT_EQ(sresp.value("packing", std::string{}), "aligned");
            EXPECT_EQ(sresp.value("blds", std::string{}), blds);
            EXPECT_EQ(sresp.value("schema_hash", std::string{}), hash);

            // New form, unknown record → SCHEMA_UNKNOWN.
            nlohmann::json bad_sreq;
            bad_sreq["owner"]     = uid;
            bad_sreq["schema_id"] = "$lab.does_not_exist.v1";
            auto bad = raw_req(broker.endpoint, "SCHEMA_REQ", bad_sreq);
            EXPECT_EQ(bad.value("status", std::string{}), "error");
            EXPECT_EQ(bad.value("error_code", std::string{}), "SCHEMA_UNKNOWN");

            // Legacy form still works — channel_name returns the channel's
            // schema fields, and now also surfaces `schema_owner`.
            nlohmann::json legacy;
            legacy["channel_name"] = channel;
            auto lresp = raw_req(broker.endpoint, "SCHEMA_REQ", legacy);
            EXPECT_EQ(lresp.value("status", std::string{}), "success");
            EXPECT_EQ(lresp.value("schema_owner", std::string{}), uid)
                << "Phase 3 channels expose their schema_owner via legacy SCHEMA_REQ";
            EXPECT_EQ(lresp.value("schema_id", std::string{}), sid);
            EXPECT_EQ(lresp.value("schema_hash", std::string{}), hash);

            broker.stop_and_join();
        },
        "broker.broker_sch_schema_req_owner_id",
        logger_module(), zmq_module());
}

// ── Inbox path-A: REG_REQ inbox metadata creates record under (uid, "inbox") ─

int broker_sch_inbox_path_a()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel  = "broker.sch.inbox";
            const std::string uid      = "prod.broker.inbox.uid00000001";
            const std::string inbox_ep = "tcp://127.0.0.1:9988";
            const std::string ibj      = R"([{"type":"float64","count":1,"length":0}])";

            auto reg = baseline_reg_req(channel, uid);
            reg["inbox_endpoint"]    = inbox_ep;
            reg["inbox_schema_json"] = ibj;
            reg["inbox_packing"]     = "aligned";
            reg["inbox_checksum"]    = "enforced";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // SCHEMA_REQ for the inbox record returns the same fields the
            // broker recorded.  Hash and BLDS come back as-stored.
            nlohmann::json sreq;
            sreq["owner"]     = uid;
            sreq["schema_id"] = "inbox";
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
            ASSERT_FALSE(sresp.is_null());
            EXPECT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
            EXPECT_EQ(sresp.value("owner", std::string{}), uid);
            EXPECT_EQ(sresp.value("schema_id", std::string{}), "inbox");
            EXPECT_EQ(sresp.value("packing", std::string{}), "aligned");
            EXPECT_EQ(sresp.value("blds", std::string{}), ibj);
            // Hash is opaque (broker computed it); just assert it has a
            // reasonable shape (32 bytes / 64 hex chars).
            const auto hash_hex = sresp.value("schema_hash", std::string{});
            EXPECT_EQ(hash_hex.size(), 64u);

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_path_a",
        logger_module(), zmq_module());
}

// ── Inbox: same uid, different inbox schema → SCHEMA_HASH_MISMATCH_SELF ─────

int broker_sch_inbox_hash_mismatch_self()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch1 = "broker.sch.inbox_mm.a";
            const std::string ch2 = "broker.sch.inbox_mm.b";
            const std::string uid = "prod.broker.ibmm.uid00000001";

            auto reg1 = baseline_reg_req(ch1, uid);
            reg1["inbox_endpoint"]    = "tcp://127.0.0.1:9989";
            reg1["inbox_schema_json"] = R"([{"type":"float64","count":1,"length":0}])";
            reg1["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg1)
                          .value("status", std::string{}),
                      "success");

            // Same uid, NEW channel, different inbox schema.  HEP-0034
            // §11.4 says the inbox record is keyed by (role_uid, "inbox")
            // independent of channel name → second REG_REQ collides.
            auto reg2 = baseline_reg_req(ch2, uid);
            reg2["inbox_endpoint"]    = "tcp://127.0.0.1:9990";
            reg2["inbox_schema_json"] = R"([{"type":"int32","count":4,"length":0}])";
            reg2["inbox_packing"]     = "aligned";
            auto r2 = raw_req(broker.endpoint, "REG_REQ", reg2);
            ASSERT_FALSE(r2.is_null());
            EXPECT_EQ(r2.value("status", std::string{}), "error") << r2.dump();
            EXPECT_EQ(r2.value("error_code", std::string{}), "SCHEMA_HASH_MISMATCH_SELF");

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_hash_mismatch_self",
        logger_module(), zmq_module());
}

// ── Inbox: idempotent re-registration (same uid + same fields → success) ────

int broker_sch_inbox_idempotent()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch1 = "broker.sch.inbox_idem.a";
            const std::string ch2 = "broker.sch.inbox_idem.b";
            const std::string uid = "prod.broker.idem.uid00000001";
            const std::string ibj = R"([{"type":"float64","count":1,"length":0}])";

            auto reg1 = baseline_reg_req(ch1, uid);
            reg1["inbox_endpoint"]    = "tcp://127.0.0.1:9991";
            reg1["inbox_schema_json"] = ibj;
            reg1["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg1)
                          .value("status", std::string{}),
                      "success");

            // Same uid, same inbox schema, NEW channel — idempotent at
            // registry level, so the broker should accept (and HubState
            // returns kIdempotent without bumping the registered counter).
            auto reg2 = baseline_reg_req(ch2, uid);
            reg2["inbox_endpoint"]    = "tcp://127.0.0.1:9992";
            reg2["inbox_schema_json"] = ibj;       // identical fields
            reg2["inbox_packing"]     = "aligned"; // identical packing
            auto r2 = raw_req(broker.endpoint, "REG_REQ", reg2);
            EXPECT_EQ(r2.value("status", std::string{}), "success") << r2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_idempotent",
        logger_module(), zmq_module());
}

// ── Inbox: malformed inbox_schema_json → INBOX_SCHEMA_INVALID ───────────────

int broker_sch_inbox_invalid_json()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.inbox_bad_json";
            const std::string uid     = "prod.broker.ibj.uid00000001";

            // Parse error.
            auto reg = baseline_reg_req(channel, uid);
            reg["inbox_endpoint"]    = "tcp://127.0.0.1:9993";
            reg["inbox_schema_json"] = "not-json";
            reg["inbox_packing"]     = "aligned";
            auto r = raw_req(broker.endpoint, "REG_REQ", reg);
            ASSERT_FALSE(r.is_null());
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "INBOX_SCHEMA_INVALID");

            // Wrong shape (object, not array).
            auto reg2 = baseline_reg_req(channel + ".obj", uid + "1");
            reg2["inbox_endpoint"]    = "tcp://127.0.0.1:9994";
            reg2["inbox_schema_json"] = R"({"type":"float64"})"; // object, not array
            reg2["inbox_packing"]     = "aligned";
            auto r2 = raw_req(broker.endpoint, "REG_REQ", reg2);
            EXPECT_EQ(r2.value("error_code", std::string{}), "INBOX_SCHEMA_INVALID");

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_invalid_json",
        logger_module(), zmq_module());
}

// ── Inbox: two different roles, each with own inbox → both records exist ────

int broker_sch_inbox_two_owners()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch_a = "broker.sch.inbox_2a";
            const std::string ch_b = "broker.sch.inbox_2b";
            const std::string uid_a = "prod.broker.ib2a.uid00000001";
            const std::string uid_b = "prod.broker.ib2b.uid00000002";

            // Same schema fields under two different owners (uid_a and uid_b).
            // HEP-0034 §8: namespace-by-owner; both records coexist.
            const std::string ibj = R"([{"type":"float64","count":1,"length":0}])";

            auto rA = baseline_reg_req(ch_a, uid_a);
            rA["inbox_endpoint"]    = "tcp://127.0.0.1:9995";
            rA["inbox_schema_json"] = ibj;
            rA["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", rA)
                          .value("status", std::string{}),
                      "success");

            auto rB = baseline_reg_req(ch_b, uid_b);
            rB["inbox_endpoint"]    = "tcp://127.0.0.1:9996";
            rB["inbox_schema_json"] = ibj;            // SAME content
            rB["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", rB)
                          .value("status", std::string{}),
                      "success") << "Different owner with same fields must succeed";

            // Both records resolvable via SCHEMA_REQ.
            for (const auto &uid : {uid_a, uid_b})
            {
                nlohmann::json sreq;
                sreq["owner"]     = uid;
                sreq["schema_id"] = "inbox";
                auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
                EXPECT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
                EXPECT_EQ(sresp.value("owner", std::string{}), uid);
            }

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_two_owners",
        logger_module(), zmq_module());
}

// ── SCHEMA_REQ with no key fields → INVALID_REQUEST ─────────────────────────

int broker_sch_schema_req_invalid()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            // No owner, no schema_id, no channel_name — wire payload is
            // an object with no keys (NOT a null json — wire messages
            // are always objects).
            nlohmann::json sreq = nlohmann::json::object();
            auto resp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}), "INVALID_REQUEST");

            // Owner without schema_id → still INVALID_REQUEST (legacy form
            // requires channel_name; new form requires both owner + id).
            nlohmann::json half = nlohmann::json::object();
            half["owner"] = "prod.test.uid00000001";
            auto resp2 = raw_req(broker.endpoint, "SCHEMA_REQ", half);
            EXPECT_EQ(resp2.value("error_code", std::string{}), "INVALID_REQUEST");

            broker.stop_and_join();
        },
        "broker.broker_sch_schema_req_invalid",
        logger_module(), zmq_module());
}

// ── Inbox packing must be "aligned" or "packed" ─────────────────────────────

int broker_sch_inbox_invalid_packing()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.inbox_bad_pack";
            const std::string uid     = "prod.broker.ibp.uid00000001";

            auto reg = baseline_reg_req(channel, uid);
            reg["inbox_endpoint"]    = "tcp://127.0.0.1:9997";
            reg["inbox_schema_json"] = R"([{"type":"float64","count":1,"length":0}])";
            reg["inbox_packing"]     = "natural";  // invalid

            auto r = raw_req(broker.endpoint, "REG_REQ", reg);
            ASSERT_FALSE(r.is_null());
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "INVALID_INBOX_PACKING");

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_invalid_packing",
        logger_module(), zmq_module());
}

} // namespace pylabhub::tests::worker::broker

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct BrokerWorkerRegistrar
{
    BrokerWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "broker")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker;
                if (scenario == "broker_reg_disc_happy_path")
                    return broker_reg_disc_happy_path();
                if (scenario == "broker_schema_mismatch")
                    return broker_schema_mismatch();
                if (scenario == "broker_channel_not_found")
                    return broker_channel_not_found();
                if (scenario == "broker_dereg_happy_path")
                    return broker_dereg_happy_path();
                if (scenario == "broker_dereg_pid_mismatch")
                    return broker_dereg_pid_mismatch();
                if (scenario == "broker_sch_record_path_b_created")
                    return broker_sch_record_path_b_created();
                if (scenario == "broker_sch_record_hash_mismatch_self")
                    return broker_sch_record_hash_mismatch_self();
                if (scenario == "broker_sch_consumer_citation_match")
                    return broker_sch_consumer_citation_match();
                if (scenario == "broker_sch_consumer_citation_mismatch")
                    return broker_sch_consumer_citation_mismatch();
                if (scenario == "broker_sch_no_packing_backward_compat")
                    return broker_sch_no_packing_backward_compat();
                if (scenario == "broker_sch_schema_req_owner_id")
                    return broker_sch_schema_req_owner_id();
                if (scenario == "broker_sch_inbox_path_a")
                    return broker_sch_inbox_path_a();
                if (scenario == "broker_sch_inbox_hash_mismatch_self")
                    return broker_sch_inbox_hash_mismatch_self();
                if (scenario == "broker_sch_inbox_idempotent")
                    return broker_sch_inbox_idempotent();
                if (scenario == "broker_sch_inbox_invalid_json")
                    return broker_sch_inbox_invalid_json();
                if (scenario == "broker_sch_inbox_two_owners")
                    return broker_sch_inbox_two_owners();
                if (scenario == "broker_sch_schema_req_invalid")
                    return broker_sch_schema_req_invalid();
                if (scenario == "broker_sch_inbox_invalid_packing")
                    return broker_sch_inbox_invalid_packing();
                fmt::print(stderr, "ERROR: Unknown broker scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerWorkerRegistrar g_broker_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
