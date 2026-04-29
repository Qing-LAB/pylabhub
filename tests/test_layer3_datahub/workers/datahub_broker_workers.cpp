// tests/test_layer3_datahub/workers/datahub_broker_workers.cpp
// Phase C — BrokerService integration tests.
#include "datahub_broker_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "plh_datahub.hpp"
#include "utils/schema_utils.hpp"  // compute_canonical_hash_from_wire (HEP-0034 §6.3)
#include "utils/format_tools.hpp"  // bytes_to_hex

#include <gtest/gtest.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <cppzmq/zmq.hpp>
#include <cppzmq/zmq_addon.hpp>
#include <zmq.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
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

/// Helper for HEP-0034 Phase 3 tests: given a wire-form blds + packing,
/// return the hex-encoded BLAKE2b-256 fingerprint the broker will
/// recompute and verify against the producer's `schema_hash`.  Without
/// this, tests using placeholder hashes like `aa_hex()` would all NACK
/// FINGERPRINT_INCONSISTENT under Phase 3 follow-up.
std::string canonical_hash_hex(const std::string &slot_blds,
                               const std::string &slot_packing,
                               const std::string &fz_blds   = {},
                               const std::string &fz_packing = {})
{
    const auto h = pylabhub::hub::compute_canonical_hash_from_wire(
        slot_blds, slot_packing, fz_blds, fz_packing);
    return pylabhub::format_tools::bytes_to_hex(
        {reinterpret_cast<const char *>(h.data()), h.size()});
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
            // Wire-form canonical BLDS (HEP-0034 §10.1): "name:type:count:length"
            // joined with "|".  Broker will recompute the hash from this and
            // compare to schema_hash — placeholder hashes no longer pass.
            const std::string blds    = "ts:f64:1:0|value:f32:1:0";
            const std::string packing = "aligned";

            auto req = baseline_reg_req(channel, uid);
            req["schema_id"]      = sid;
            req["schema_hash"]    = canonical_hash_hex(blds, packing);
            req["schema_packing"] = packing;
            req["schema_blds"]    = blds;

            auto resp = raw_req(broker.endpoint, "REG_REQ", req);
            ASSERT_FALSE(resp.is_null()) << "raw_req timed out";
            EXPECT_EQ(resp.value("status", std::string{}), "success")
                << "REG_REQ with full structure must succeed; got: " << resp.dump();

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

// ── Same uid + same schema_id on DIFFERENT channels, different fingerprints
//    → second REG_REQ NACKs SCHEMA_HASH_MISMATCH_SELF.  Two different
//    channels are required so the channel-mismatch check (which now
//    runs before schema-record creation) doesn't preempt with
//    SCHEMA_MISMATCH (audit fix A2 ordering).
int broker_sch_record_hash_mismatch_self()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch1 = "broker.sch.mismatch_self.a";
            const std::string ch2 = "broker.sch.mismatch_self.b";
            const std::string uid = "prod.broker.sch_mm.uid00000001";
            const std::string sid = "$lab.sch_mm.frame.v1";

            const std::string blds_a    = "ts:f64:1:0|value:f32:1:0";
            const std::string blds_b    = "ts:f64:1:0|value:i32:1:0"; // type differs → different hash
            const std::string packing   = "aligned";
            const std::string hash_a    = canonical_hash_hex(blds_a, packing);
            const std::string hash_b    = canonical_hash_hex(blds_b, packing);
            ASSERT_NE(hash_a, hash_b) << "Test fixtures must produce different hashes";

            auto req1 = baseline_reg_req(ch1, uid);
            req1["schema_id"]      = sid;
            req1["schema_hash"]    = hash_a;
            req1["schema_packing"] = packing;
            req1["schema_blds"]    = blds_a;
            auto r1 = raw_req(broker.endpoint, "REG_REQ", req1);
            ASSERT_EQ(r1.value("status", std::string{}), "success") << r1.dump();

            // Second REG_REQ — same (owner, schema_id) on a DIFFERENT
            // channel, with a different (internally-consistent)
            // fingerprint.  The schema record under (uid, sid) already
            // exists with hash_a; the new attempt with hash_b collides
            // → kHashMismatchSelf → SCHEMA_HASH_MISMATCH_SELF.
            auto req2 = baseline_reg_req(ch2, uid);
            req2["schema_id"]      = sid;
            req2["schema_hash"]    = hash_b;
            req2["schema_packing"] = packing;
            req2["schema_blds"]    = blds_b;
            auto r2 = raw_req(broker.endpoint, "REG_REQ", req2);
            ASSERT_FALSE(r2.is_null()) << "raw_req timed out";
            EXPECT_EQ(r2.value("status", std::string{}), "error")
                << "Second REG_REQ with different fingerprint must NACK; got: " << r2.dump();
            EXPECT_EQ(r2.value("error_code", std::string{}), "SCHEMA_HASH_MISMATCH_SELF")
                << "Error code must be SCHEMA_HASH_MISMATCH_SELF; got: " << r2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_record_hash_mismatch_self",
        logger_module(), zmq_module());
}

// ── Consumer named-citation (id + matching hash) → success ──────────────────

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
            const std::string blds    = "ts:f64:1:0|value:f32:1:0";
            const std::string packing = "aligned";
            const std::string hash    = canonical_hash_hex(blds, packing);

            auto reg = baseline_reg_req(channel, p_uid);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds;
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Named-citation mode (HEP-0034 §10.3): consumer cites by id
            // + hash.  Hash must equal channel's stored hash.
            nlohmann::json cons_req;
            cons_req["channel_name"]         = channel;
            cons_req["consumer_uid"]         = c_uid;
            cons_req["consumer_name"]        = "test_consumer";
            cons_req["consumer_pid"]         = pylabhub::platform::get_pid();
            cons_req["expected_schema_id"]   = sid;
            cons_req["expected_schema_hash"] = hash;
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons_req);
            ASSERT_FALSE(cr.is_null()) << "raw_req timed out";
            EXPECT_EQ(cr.value("status", std::string{}), "success")
                << "Named citation match must succeed; got: " << cr.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_consumer_citation_match",
        logger_module(), zmq_module());
}

// ── Consumer named-citation with WRONG hash → SCHEMA_CITATION_REJECTED ──────

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
            const std::string blds_p  = "ts:f64:1:0|value:f32:1:0";
            const std::string blds_c  = "ts:f64:1:0|value:i32:1:0"; // consumer thinks i32
            const std::string packing = "aligned";
            const std::string hash_p  = canonical_hash_hex(blds_p, packing);
            const std::string hash_c  = canonical_hash_hex(blds_c, packing);
            ASSERT_NE(hash_p, hash_c);

            auto reg = baseline_reg_req(channel, p_uid);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash_p;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds_p;
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Consumer cites the right id but a wrong hash (its local
            // expectation differs from what the producer registered).
            nlohmann::json cons_req;
            cons_req["channel_name"]         = channel;
            cons_req["consumer_uid"]         = c_uid;
            cons_req["consumer_name"]        = "test_consumer";
            cons_req["consumer_pid"]         = pylabhub::platform::get_pid();
            cons_req["expected_schema_id"]   = sid;
            cons_req["expected_schema_hash"] = hash_c;  // wrong
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons_req);
            ASSERT_FALSE(cr.is_null()) << "raw_req timed out";
            EXPECT_EQ(cr.value("status", std::string{}), "error")
                << "Hash mismatch must be NACKed; got: " << cr.dump();
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
            const std::string blds    = "ts:f64:1:0|value:f32:1:0";
            const std::string packing = "aligned";
            const std::string hash    = canonical_hash_hex(blds, packing);

            auto reg = baseline_reg_req(channel, uid);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = packing;
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

// ── Phase 3 follow-up — Stage-2 verification + tightened gates ──────────────

int broker_sch_reg_missing_packing()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            // schema_id non-empty but schema_packing missing → NACK MISSING_PACKING.
            auto reg = baseline_reg_req("broker.sch.miss_pack",
                                        "prod.broker.miss_pack.uid00000001");
            reg["schema_id"]      = "$lab.miss_pack.frame.v1";
            reg["schema_hash"]    = std::string(64, '0');
            reg["schema_blds"]    = "ts:f64:1:0";
            // intentionally no schema_packing
            auto r = raw_req(broker.endpoint, "REG_REQ", reg);
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "MISSING_PACKING");

            broker.stop_and_join();
        },
        "broker.broker_sch_reg_missing_packing",
        logger_module(), zmq_module());
}

int broker_sch_reg_fingerprint_inconsistent()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            // Producer claims a hash that does NOT match BLDS+packing.
            // Stage-2 broker recomputes and rejects.
            auto reg = baseline_reg_req("broker.sch.fp_bad",
                                        "prod.broker.fp_bad.uid00000001");
            reg["schema_id"]      = "$lab.fp_bad.frame.v1";
            reg["schema_packing"] = "aligned";
            reg["schema_blds"]    = "ts:f64:1:0|value:f32:1:0";
            reg["schema_hash"]    = aa_hex();  // bogus — doesn't match canonical
            auto r = raw_req(broker.endpoint, "REG_REQ", reg);
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "FINGERPRINT_INCONSISTENT");

            broker.stop_and_join();
        },
        "broker.broker_sch_reg_fingerprint_inconsistent",
        logger_module(), zmq_module());
}

int broker_sch_cons_named_missing_hash()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch  = "broker.sch.cons_no_hash";
            const std::string p   = "prod.broker.cnh.uid00000001";
            const std::string c   = "cons.broker.cnh.uid00000002";
            const std::string sid = "$lab.cnh.frame.v1";
            const std::string blds    = "ts:f64:1:0";
            const std::string packing = "aligned";

            auto reg = baseline_reg_req(ch, p);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = canonical_hash_hex(blds, packing);
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds;
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Consumer cites by id but omits the hash → MISSING_HASH_FOR_NAMED_CITATION.
            nlohmann::json cons;
            cons["channel_name"]       = ch;
            cons["consumer_uid"]       = c;
            cons["consumer_name"]      = "test_consumer";
            cons["consumer_pid"]       = pylabhub::platform::get_pid();
            cons["expected_schema_id"] = sid;
            // intentionally no expected_schema_hash
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons);
            EXPECT_EQ(cr.value("status", std::string{}), "error") << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}),
                      "MISSING_HASH_FOR_NAMED_CITATION");

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_named_missing_hash",
        logger_module(), zmq_module());
}

int broker_sch_cons_anonymous_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch  = "broker.sch.cons_anon_ok";
            const std::string p   = "prod.broker.canok.uid00000001";
            const std::string c   = "cons.broker.canok.uid00000002";
            const std::string blds    = "ts:f64:1:0|value:f32:1:0";
            const std::string packing = "aligned";
            const std::string hash    = canonical_hash_hex(blds, packing);

            // Producer registers with a named id (so the channel has a record).
            auto reg = baseline_reg_req(ch, p);
            reg["schema_id"]      = "$lab.canok.frame.v1";
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds;
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Consumer in anonymous mode: provides full structure (no id).
            // Hash optional — broker recomputes and compares to channel.
            nlohmann::json cons;
            cons["channel_name"]     = ch;
            cons["consumer_uid"]     = c;
            cons["consumer_name"]    = "test_consumer";
            cons["consumer_pid"]     = pylabhub::platform::get_pid();
            cons["expected_schema_blds"]    = blds;
            cons["expected_schema_packing"] = packing;
            // (no expected_schema_id, no expected_schema_hash)
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons);
            EXPECT_EQ(cr.value("status", std::string{}), "success") << cr.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_anonymous_happy_path",
        logger_module(), zmq_module());
}

int broker_sch_cons_anonymous_missing_packing()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch = "broker.sch.cons_anon_nopack";
            const std::string p  = "prod.broker.canp.uid00000001";
            const std::string c  = "cons.broker.canp.uid00000002";

            auto reg = baseline_reg_req(ch, p);
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Anonymous mode with blds but no packing → NACK.
            nlohmann::json cons;
            cons["channel_name"]  = ch;
            cons["consumer_uid"]  = c;
            cons["consumer_name"] = "test_consumer";
            cons["consumer_pid"]  = pylabhub::platform::get_pid();
            cons["expected_schema_blds"] = "ts:f64:1:0";
            // intentionally no expected_packing
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons);
            EXPECT_EQ(cr.value("status", std::string{}), "error") << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}),
                      "MISSING_PACKING_FOR_ANONYMOUS_CITATION");

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_anonymous_missing_packing",
        logger_module(), zmq_module());
}

int broker_sch_cons_named_with_structure_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker   = start_broker_in_thread(std::move(cfg));

            const std::string ch  = "broker.sch.cons_named_struct_bad";
            const std::string p   = "prod.broker.cnsb.uid00000001";
            const std::string c   = "cons.broker.cnsb.uid00000002";
            const std::string sid = "$lab.cnsb.frame.v1";
            const std::string blds_p    = "ts:f64:1:0|value:f32:1:0";
            const std::string blds_c    = "ts:f64:1:0|value:i32:1:0"; // consumer thinks i32
            const std::string packing   = "aligned";

            auto reg = baseline_reg_req(ch, p);
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = canonical_hash_hex(blds_p, packing);
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds_p;
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Consumer cites by id with correct hash, BUT also provides
            // a structure that doesn't match the channel's hash —
            // defense-in-depth check kicks in → FINGERPRINT_INCONSISTENT.
            nlohmann::json cons;
            cons["channel_name"]         = ch;
            cons["consumer_uid"]         = c;
            cons["consumer_name"]        = "test_consumer";
            cons["consumer_pid"]         = pylabhub::platform::get_pid();
            cons["expected_schema_id"]   = sid;
            cons["expected_schema_hash"] = canonical_hash_hex(blds_p, packing);
            cons["expected_schema_blds"]        = blds_c;   // diverges from producer
            cons["expected_schema_packing"]     = packing;
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons);
            EXPECT_EQ(cr.value("status", std::string{}), "error") << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}), "FINGERPRINT_INCONSISTENT");

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_named_with_structure_mismatch",
        logger_module(), zmq_module());
}

int broker_sch_inbox_evicts_on_disconnect()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string ch  = "broker.sch.inbox_evict";
            const std::string uid = "prod.broker.ibev.uid00000001";
            const auto producer_pid = pylabhub::platform::get_pid();

            // Register a producer with inbox metadata.
            auto reg = baseline_reg_req(ch, uid);
            reg["producer_pid"]      = producer_pid;
            reg["inbox_endpoint"]    = "tcp://127.0.0.1:9998";
            reg["inbox_schema_json"] = R"([{"type":"float64","count":1,"length":0}])";
            reg["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg)
                          .value("status", std::string{}),
                      "success");

            // Confirm inbox record exists before disconnect.
            {
                nlohmann::json sreq;
                sreq["owner"]     = uid;
                sreq["schema_id"] = "inbox";
                auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
                EXPECT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
            }

            // Trigger channel close via DEREG_REQ (cleaner than waiting
            // for heartbeat timeout — same `_on_channel_closed` cascade).
            nlohmann::json dereg;
            dereg["channel_name"] = ch;
            dereg["producer_pid"] = producer_pid;
            auto dr = raw_req(broker.endpoint, "DEREG_REQ", dereg);
            ASSERT_EQ(dr.value("status", std::string{}), "success") << dr.dump();

            // Inbox record must be evicted by the cascade.
            nlohmann::json sreq;
            sreq["owner"]     = uid;
            sreq["schema_id"] = "inbox";
            auto after = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
            EXPECT_EQ(after.value("status", std::string{}), "error") << after.dump();
            EXPECT_EQ(after.value("error_code", std::string{}), "SCHEMA_UNKNOWN")
                << "Inbox record should evict on producer disconnect "
                   "(HEP-CORE-0034 §7.2 cascade via _on_channel_closed)";

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_evicts_on_disconnect",
        logger_module(), zmq_module());
}

// ============================================================================
// HEP-0034 Phase 4b — hub-globals + path-C adoption
// ============================================================================

namespace
{

/// Create a temp dir + write `<dir>/<id>/<id>.v<N>.json` with the given
/// fields.  Returns the dir path.  Caller deletes via remove_all().
/// Mirrors what `<hub_dir>/schemas/` would look like in a real
/// deployment, so the broker's `load_hub_globals_()` walker
/// (HEP-CORE-0034 §2.4 I2) auto-loads it.
std::filesystem::path make_global_schema_dir(
    const std::string &id_dotted,         // e.g. "lab.demo.frame"
    int                version,
    const std::string &fields_array_json) // e.g. R"([{"name":"v","type":"float32"}])"
{
    auto root = std::filesystem::temp_directory_path() /
                ("plh_p4b_" + std::to_string(::getpid()) + "_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    std::filesystem::create_directories(root);

    // Convert dot-separated id to nested directory tree
    // (lab.demo.frame → lab/demo/frame.v1.json).
    std::vector<std::string> parts;
    {
        std::string s; std::stringstream ss(id_dotted);
        while (std::getline(ss, s, '.')) parts.push_back(s);
    }
    auto dir = root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) dir /= parts[i];
    std::filesystem::create_directories(dir);

    const std::string fname = parts.back() + ".v" + std::to_string(version) + ".json";
    std::ofstream f(dir / fname);
    f << R"({"id":")" << id_dotted << R"(","version":)" << version
      << R"(,"slot":{"packing":"aligned","fields":)" << fields_array_json << R"(}})";
    return root;
}

} // anonymous namespace

int broker_sch_hub_globals_loaded_at_startup()
{
    return run_gtest_worker(
        []()
        {
            // Stage a hub-global schema file at <tmp>/lab/demo/frame.v1.json
            const auto schema_root = make_global_schema_dir(
                "lab.demo.frame", 1,
                R"([{"name":"v","type":"float32"}])");

            BrokerService::Config cfg;
            cfg.endpoint           = "tcp://127.0.0.1:0";
            cfg.use_curve          = false;
            cfg.schema_search_dirs = {schema_root.string()};
            auto broker = start_broker_in_thread(std::move(cfg));

            // SCHEMA_REQ for (hub, $lab.demo.frame.v1) must succeed —
            // proves Phase 4b loaded the global into HubState.schemas.
            nlohmann::json sreq;
            sreq["owner"]     = "hub";
            sreq["schema_id"] = "$lab.demo.frame.v1";
            auto resp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("status", std::string{}), "success") << resp.dump();
            EXPECT_EQ(resp.value("owner",     std::string{}), "hub");
            EXPECT_EQ(resp.value("schema_id", std::string{}), "$lab.demo.frame.v1");
            EXPECT_EQ(resp.value("packing",   std::string{}), "aligned");

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_hub_globals_loaded_at_startup",
        logger_module(), zmq_module());
}

int broker_sch_path_c_adoption_succeeds()
{
    return run_gtest_worker(
        []()
        {
            const std::string sid_dotted = "lab.demo.adopt";
            const std::string sid        = "$lab.demo.adopt.v1";
            // HEP-0034 §6.3 — wire `type` is the JSON type name ("float32"),
            // NOT the BLDS token ("f32").  The hub-global is loaded from
            // JSON with `"type":"float32"`; producer must match exactly.
            const std::string blds       = "v:float32:1:0";
            const std::string packing    = "aligned";
            const std::string hash       = canonical_hash_hex(blds, packing);
            const auto schema_root = make_global_schema_dir(
                sid_dotted, 1, R"([{"name":"v","type":"float32"}])");

            BrokerService::Config cfg;
            cfg.endpoint           = "tcp://127.0.0.1:0";
            cfg.use_curve          = false;
            cfg.schema_search_dirs = {schema_root.string()};
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string channel = "broker.sch.adopt";
            const std::string uid     = "prod.broker.adopt.uid00000001";
            auto reg = baseline_reg_req(channel, uid);
            reg["schema_owner"]   = "hub";          // path C
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds;
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg);
            EXPECT_EQ(resp.value("status", std::string{}), "success") << resp.dump();

            // Verify channel.schema_owner == "hub" via legacy SCHEMA_REQ.
            nlohmann::json sreq;
            sreq["channel_name"] = channel;
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq);
            EXPECT_EQ(sresp.value("schema_owner", std::string{}), "hub")
                << "Path-C-adopted channel should report owner=hub";

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_path_c_adoption_succeeds",
        logger_module(), zmq_module());
}

int broker_sch_path_c_fingerprint_mismatch()
{
    return run_gtest_worker(
        []()
        {
            const std::string sid = "$lab.demo.mm.v1";
            // Hub-global has field "v:float32"; producer claims "v:int32".
            const auto schema_root = make_global_schema_dir(
                "lab.demo.mm", 1, R"([{"name":"v","type":"float32"}])");

            BrokerService::Config cfg;
            cfg.endpoint           = "tcp://127.0.0.1:0";
            cfg.use_curve          = false;
            cfg.schema_search_dirs = {schema_root.string()};
            auto broker = start_broker_in_thread(std::move(cfg));

            // HEP-0034 §6.3 — wire `type` is the JSON type name.
            const std::string blds_wrong    = "v:int32:1:0";
            const std::string packing       = "aligned";
            const std::string hash_wrong    = canonical_hash_hex(blds_wrong, packing);

            auto reg = baseline_reg_req("broker.sch.mm",
                                        "prod.broker.mm.uid00000001");
            reg["schema_owner"]   = "hub";
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash_wrong;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds_wrong;
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg);
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "FINGERPRINT_INCONSISTENT");

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_path_c_fingerprint_mismatch",
        logger_module(), zmq_module());
}

int broker_sch_path_c_unknown_global()
{
    return run_gtest_worker(
        []()
        {
            // No schema_search_dirs configured → no globals loaded.
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            // Use an explicit (empty) override so the default dirs aren't
            // searched (would be flaky if /usr/share/pylabhub has files).
            cfg.schema_search_dirs = {std::filesystem::temp_directory_path() /
                                      ("plh_p4b_empty_" +
                                       std::to_string(::getpid()))};
            std::filesystem::create_directories(cfg.schema_search_dirs[0]);
            auto schema_root = std::filesystem::path(cfg.schema_search_dirs[0]);
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string blds    = "v:f32:1:0";
            const std::string packing = "aligned";
            const std::string hash    = canonical_hash_hex(blds, packing);

            auto reg = baseline_reg_req("broker.sch.unk",
                                        "prod.broker.unk.uid00000001");
            reg["schema_owner"]   = "hub";
            reg["schema_id"]      = "$does.not.exist.v1";
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds;
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg);
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}), "SCHEMA_UNKNOWN");

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_path_c_unknown_global",
        logger_module(), zmq_module());
}

int broker_sch_path_x_forbidden_owner()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            cfg.use_curve = false;
            auto broker = start_broker_in_thread(std::move(cfg));

            const std::string blds    = "v:f32:1:0";
            const std::string packing = "aligned";
            const std::string hash    = canonical_hash_hex(blds, packing);

            auto reg = baseline_reg_req("broker.sch.fbd",
                                        "prod.broker.fbd.uid00000001");
            // Foreign owner (not self, not "hub") → must be rejected.
            reg["schema_owner"]   = "prod.someone.else.uid00000099";
            reg["schema_id"]      = "$lab.someone.frame.v1";
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds;
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg);
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "SCHEMA_FORBIDDEN_OWNER");

            broker.stop_and_join();
        },
        "broker.broker_sch_path_x_forbidden_owner",
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
                if (scenario == "broker_sch_reg_missing_packing")
                    return broker_sch_reg_missing_packing();
                if (scenario == "broker_sch_reg_fingerprint_inconsistent")
                    return broker_sch_reg_fingerprint_inconsistent();
                if (scenario == "broker_sch_cons_named_missing_hash")
                    return broker_sch_cons_named_missing_hash();
                if (scenario == "broker_sch_cons_anonymous_happy_path")
                    return broker_sch_cons_anonymous_happy_path();
                if (scenario == "broker_sch_cons_anonymous_missing_packing")
                    return broker_sch_cons_anonymous_missing_packing();
                if (scenario == "broker_sch_cons_named_with_structure_mismatch")
                    return broker_sch_cons_named_with_structure_mismatch();
                if (scenario == "broker_sch_inbox_evicts_on_disconnect")
                    return broker_sch_inbox_evicts_on_disconnect();
                if (scenario == "broker_sch_hub_globals_loaded_at_startup")
                    return broker_sch_hub_globals_loaded_at_startup();
                if (scenario == "broker_sch_path_c_adoption_succeeds")
                    return broker_sch_path_c_adoption_succeeds();
                if (scenario == "broker_sch_path_c_fingerprint_mismatch")
                    return broker_sch_path_c_fingerprint_mismatch();
                if (scenario == "broker_sch_path_c_unknown_global")
                    return broker_sch_path_c_unknown_global();
                if (scenario == "broker_sch_path_x_forbidden_owner")
                    return broker_sch_path_x_forbidden_owner();
                fmt::print(stderr, "ERROR: Unknown broker scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerWorkerRegistrar g_broker_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
