// tests/test_layer3_datahub/workers/datahub_broker_workers.cpp
// Phase C — BrokerService integration tests.
#include "datahub_broker_workers.h"
#include "curve_test_setup.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "plh_datahub.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/schema_utils.hpp"  // compute_canonical_hash_from_wire (HEP-0034 §6.3)
#include "utils/format_tools.hpp"  // bytes_to_hex
#include "utils/security/key_store.hpp"
#include "utils/security/known_roles.hpp"
#include "utils/security/shm_capability_channel.hpp"

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

static auto logger_module()    { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto file_lock_module() { return ::pylabhub::utils::FileLock::GetLifecycleModule(); }
static auto json_module()      { return ::pylabhub::utils::JsonConfig::GetLifecycleModule(); }
static auto crypto_module()    { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()       { return ::pylabhub::hub::GetDataBlockModule(); }
static auto zmq_module()       { return ::pylabhub::hub::GetZMQContextModule(); }

// ============================================================================
// File-local helpers
// ============================================================================

namespace
{

// -----------------------------------------------------------------------------
// BrokerHandle: thin RAII wrapper around a real `HubHost`.
//
// Per the test-design principle in `docs/todo/TESTING_TODO.md`
// §"Test Design Principles", broker tests must run against the real
// production assembly, not a hand-rolled host mock.  The previous
// `BrokerHandle` constructed `BrokerService` + `HubState` directly from
// low-level APIs — that bypassed HubHost (and ThreadManager, lifecycle
// integration, AdminService, role_uid generation, etc.) and produced
// false test signal.  This handle now owns a real `HubHost`; the
// `start_broker_in_thread` API is preserved for caller compatibility but
// the broker behind it is the same broker production runs.
//
// The legacy `BrokerService::Config` is translated to the on-disk
// `HubConfig` shape via `make_test_hub_directory` — random ephemeral
// port, no admin, no script, optional CURVE.
// -----------------------------------------------------------------------------
struct BrokerHandle
{
    std::filesystem::path                    hub_dir;
    std::unique_ptr<pylabhub::hub_host::HubHost> host;
    std::string                              endpoint;
    std::string                              pubkey;

    BrokerHandle() = default;
    BrokerHandle(BrokerHandle &&) noexcept = default;
    BrokerHandle &operator=(BrokerHandle &&) noexcept = default;
    ~BrokerHandle() { stop_and_join(); }

    void stop_and_join()
    {
        if (host)
        {
            host->shutdown(); // ~HubHost calls ThreadManager::drain anyway,
                              // but explicit shutdown is the documented
                              // happy-path teardown.
            host.reset();
        }
        if (!hub_dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(hub_dir, ec);
            hub_dir.clear();
        }
    }
};

namespace
{

std::filesystem::path make_test_hub_directory(const std::vector<std::string> &schema_search_dirs)
{
    namespace fs = std::filesystem;
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_broker_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    pylabhub::utils::HubDirectory::init_directory(dir, "BrokerTestHub");

    // Patch hub.json to test-friendly shape: ephemeral port, no admin,
    // no script.  Post-HEP-CORE-0035 §2 CURVE is unconditional in the
    // production broker, so there is no `use_curve` switch here — the
    // broker always comes up under CURVE and `start_broker_in_thread`
    // seeds the matching `known_roles.json` for ZAP admission.
    const fs::path hub_json = dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json);
        if (f.is_open())
            j = nlohmann::json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"] = false;
    j["script"]["path"]   = "";
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }

    // Hub-globals layout per HEP-CORE-0034 §12: `<hub_dir>/schemas/` is
    // the canonical filesystem-authoritative source.  Real `HubHost`
    // wires `bcfg.schema_search_dirs` to this path (hub_host.cpp).
    // Test scenarios that legacy-style passed their own
    // `cfg.schema_search_dirs = {temp_dir}` get their content seeded
    // into the canonical hub_dir/schemas/ here so the broker's
    // `load_hub_globals_` finds them at the production-shaped location.
    fs::path schemas_dir = dir / "schemas";
    fs::create_directories(schemas_dir);
    for (const auto &src : schema_search_dirs)
    {
        fs::path src_path(src);
        if (!fs::exists(src_path) || !fs::is_directory(src_path))
            continue;
        for (const auto &entry : fs::recursive_directory_iterator(src_path))
        {
            if (!entry.is_regular_file())
                continue;
            const auto rel = fs::relative(entry.path(), src_path);
            const auto dst = schemas_dir / rel;
            fs::create_directories(dst.parent_path());
            fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing);
        }
    }
    return dir;
}

} // anonymous namespace

// Start a broker for the test.  Post-HEP-CORE-0035 §4.8 the broker's
// Layer-1 ZAP gate admits only roles listed in `vault/known_roles.json`;
// production callers (`start_hubhost_broker`) write that file from a
// `CurveSetup`.  This helper does the same so admission accepts every
// uid the caller declares — required for every BRC `connect()` AND
// every `raw_req()` that uses a `role_identity_name` (the seckey it
// pulls from `key_store()` only works through ZAP if the matching
// pubkey is also in `known_roles.json`).
//
// Callers seed their `CurveSetup` via `CurveKeyStoreFixture` in scope
// BEFORE calling — that fixture seeds `kHubIdentityName` + `role.<uid>`
// into `key_store()` (HEP-CORE-0040 §172).  HubHost::startup() reads
// `kHubIdentityName` from the KeyStore to wire its own CURVE identity.
BrokerHandle start_broker_in_thread(BrokerService::Config cfg,
                                    const pylabhub::tests::CurveSetup &curve)
{
    BrokerHandle h;
    h.hub_dir = make_test_hub_directory(cfg.schema_search_dirs);

    // Write known_roles.json via production `KnownRolesStore::save_to_file`
    // so the file format (version + roles array + atomic-write + 0600
    // perms) is defined in exactly one place.  Mirrors the production
    // path used by `start_hubhost_broker` in `broker_test_harness.cpp`.
    {
        pylabhub::utils::security::KnownRolesStore store;
        for (const auto &[uid, kp] : curve.role_keys)
            store.add(pylabhub::tests::make_known_role(uid, kp.public_z85));
        store.save_to_file(h.hub_dir / "vault" / "known_roles.json");
    }

    h.host    = std::make_unique<pylabhub::hub_host::HubHost>(
        pylabhub::config::HubConfig::load_from_directory(h.hub_dir.string()));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = h.host->broker_pubkey();
    return h;
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// raw_req: Sends a two-frame [msg_type, payload_json] to a DEALER socket and
// returns the parsed response body JSON.  Optionally enables CurveZMQ when
// server_pubkey is a 40-char Z85 string.
//
// `role_identity_name`: identity name in `key_store()` to authenticate as.
// When set, the CURVE client side uses that role's seeded keypair so the
// broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8) admits the connection.
// Required post-strict-CURVE (#157 + HEP-CORE-0040 §172); ephemeral
// keys no longer pass admission because they're not in
// `known_roles.json`.  When empty, the ephemeral-key path remains for
// plaintext-broker tests (server_pubkey also empty).
//
// Returns {} (null JSON) on timeout or receive error.
//
// Lifted out of the anonymous namespace 2026-05-20 (audit M1 follow-up)
// so `datahub_broker_protocol_workers.cpp` can use it via forward
// declaration without copying the body.  Reachable as
// `pylabhub::tests::worker::broker::raw_req`.
// -----------------------------------------------------------------------------
// Decorate a REG_REQ payload with the HEP-CORE-0036 §5b canonical
// fields (`zmq_pubkey`, `data_transport`, `has_shm`,
// `shm_capability_endpoint`, `role_type`) that the broker rejects as
// missing post-#290.  Idempotent — leaves any field the caller already
// set untouched, so tests deliberately exercising a malformed shape
// (negative-path gate tests) keep their explicit overrides.  `is_cons`
// switches the producer-side hub-managed fields off for CONSUMER_REG_REQ.
static void apply_5b_canonical_fields(nlohmann::json &req,
                                      const std::string &role_uid,
                                      bool is_cons)
{
    namespace sec = pylabhub::utils::security;
    const std::string ks_name = pylabhub::tests::role_keystore_name(role_uid);
    if (!req.contains("zmq_pubkey") && sec::key_store().has(ks_name))
        req["zmq_pubkey"] = std::string{sec::key_store().pubkey(ks_name)};
    if (!req.contains("role_type"))
        req["role_type"] = is_cons ? "consumer" : "producer";
    if (!is_cons)
    {
        if (!req.contains("data_transport")) req["data_transport"] = "shm";
        if (!req.contains("has_shm"))        req["has_shm"]        = true;
        if (!req.contains("shm_capability_endpoint"))
            req["shm_capability_endpoint"] =
                sec::default_shm_capability_endpoint(
                    req.value("channel_name", std::string{}));
    }
}

nlohmann::json raw_req(const std::string& endpoint,
                       const std::string& msg_type,
                       const nlohmann::json& payload_in,
                       int timeout_ms = 2000,
                       const std::string& server_pubkey = "",
                       const std::string& role_identity_name = "")
{
    constexpr size_t kZ85KeyLen = 40;
    constexpr size_t kZ85BufLen = 41;

    // Auto-decorate REG_REQ / CONSUMER_REG_REQ payloads with the §5b
    // canonical fields the broker requires.  Only when the caller
    // passed a real `role_identity_name` (not the gate-test sentinel)
    // AND the payload actually has a `role_uid` — gate tests with
    // empty/malformed uids must reach the broker unchanged so the
    // uid-shape gate fires.
    nlohmann::json payload = payload_in;
    const bool is_reg  = (msg_type == "REG_REQ");
    const bool is_cons = (msg_type == "CONSUMER_REG_REQ");
    if ((is_reg || is_cons)
     && role_identity_name != "wire.gate.uid00000099"
     && payload.contains("role_uid")
     && payload["role_uid"].is_string())
    {
        const auto &uid = payload["role_uid"].get_ref<const std::string &>();
        if (!uid.empty())
            apply_5b_canonical_fields(payload, uid, is_cons);
    }

    zmq::context_t ctx(1);
    zmq::socket_t dealer(ctx, zmq::socket_type::dealer);

    if (server_pubkey.size() == kZ85KeyLen)
    {
        dealer.set(zmq::sockopt::curve_serverkey, server_pubkey);
        if (!role_identity_name.empty())
        {
            // Authenticate as a registered role — broker ZAP gate
            // matches `known_roles.json`.  HEP-CORE-0040 §172: the
            // seckey lives only inside the callback; ZMQ copies it
            // into its socket-internal storage during set() so it
            // survives the callback's sodium_memzero.
            namespace sec = pylabhub::utils::security;
            // `role_identity_name` is the raw uid string the caller
            // owns; the KeyStore entry name is the prefixed
            // `role.<uid>` form seeded by `CurveKeyStoreFixture`
            // (see `pylabhub::tests::role_keystore_name`).
            const std::string ks_name =
                pylabhub::tests::role_keystore_name(role_identity_name);
            const std::string client_pub{sec::key_store().pubkey(ks_name)};
            sec::key_store().with_seckey_z85(
                ks_name,
                [&](std::string_view seckey_z85) {
                    dealer.set(zmq::sockopt::curve_publickey, client_pub);
                    dealer.set(zmq::sockopt::curve_secretkey,
                               std::string(seckey_z85));
                });
        }
        else
        {
            // Ephemeral client keypair (legacy plaintext-admission path).
            std::array<char, kZ85BufLen> client_pub{};
            std::array<char, kZ85BufLen> client_sec{};
            if (zmq_curve_keypair(client_pub.data(), client_sec.data()) != 0)
                return {};
            dealer.set(zmq::sockopt::curve_publickey, std::string(client_pub.data(), kZ85KeyLen));
            dealer.set(zmq::sockopt::curve_secretkey, std::string(client_sec.data(), kZ85KeyLen));
        }
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

namespace
{

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
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.ch1.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_reg_disc_happy_path", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel = "broker.ch1";
            const std::string uid     = "prod.broker.ch1.uid00000001";

            BrokerRequestComm brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = uid;
            brc_cfg.keystore_name   = pylabhub::tests::role_keystore_name(uid);
            ASSERT_TRUE(brc.connect(brc_cfg));
            std::atomic<bool> running{true};
            std::thread t([&] { brc.run_poll_loop([&] { return running.load(); }); });

            // Build §5b-canonical reg payload via the production helper
            // — auto-fills zmq_pubkey (from key_store), role_type,
            // data_transport, has_shm, shm_capability_endpoint.
            namespace sec = pylabhub::utils::security;
            auto reg_opts = pylabhub::hub::build_producer_reg_payload(
                pylabhub::hub::ProducerRegInputs{
                    .channel    = channel,
                    .role_uid   = uid,
                    .role_name  = "test_producer",
                    .role_type  = "producer",
                    .has_shm    = true,
                    .is_zmq_transport  = false,
                    .zmq_node_endpoint = {},
                    .zmq_pubkey = std::string{sec::key_store().pubkey(
                        pylabhub::tests::role_keystore_name(uid))},
                    .shm_capability_endpoint =
                        sec::default_shm_capability_endpoint(channel),
                });
            reg_opts["producer_pid"]   = ::getpid();
            reg_opts["schema_version"] = 7;
            auto reg = brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel must succeed";

            brc.send_heartbeat(channel, uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            auto disc = brc.discover_channel(channel, {}, 5000);
            ASSERT_TRUE(disc.has_value()) << "discover_channel must find registered channel";
            // HEP-CORE-0036 §5b.4: shm_name retired (was always == channel_name).
            EXPECT_EQ(disc->value("schema_version", 0), 7);

            running.store(false);
            brc.stop();
            if (t.joinable()) t.join();
            brc.disconnect();
            broker.stop_and_join();
        },
        "broker.broker_reg_disc_happy_path",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.mismatch.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_schema_mismatch", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel  = "broker.mismatch.ch";
            const std::string role_uid = "prod.broker.mismatch.uid00000001";
            const uint64_t    pid      = pylabhub::platform::get_pid();

            // First registration — succeeds.  broker_proto 5 (R3.5b)
            // requires a valid role_uid at the gate even under default
            // Open policy.
            nlohmann::json req1;
            req1["channel_name"]     = channel;
            req1["schema_hash"]      = zero_hex();
            req1["schema_version"]   = 1;
            req1["producer_pid"]     = pid;
            req1["producer_hostname"] = "localhost";
            req1["role_uid"]         = role_uid;

            nlohmann::json resp1 = raw_req(broker.endpoint, "REG_REQ", req1, 2000, broker.pubkey, "prod.broker.mismatch.uid00000001");
            ASSERT_FALSE(resp1.is_null()) << "raw_req timed out on first REG_REQ";
            EXPECT_EQ(resp1.value("status", std::string("")), "success")
                << "First registration must succeed; got: " << resp1.dump();

            // Second registration — different schema_hash → SCHEMA_MISMATCH
            nlohmann::json req2 = req1;
            req2["schema_hash"] = aa_hex(); // different hash
            nlohmann::json resp2 = raw_req(broker.endpoint, "REG_REQ", req2, 2000, broker.pubkey, "prod.broker.mismatch.uid00000001");
            ASSERT_FALSE(resp2.is_null()) << "raw_req timed out on second REG_REQ";
            EXPECT_EQ(resp2.value("status", std::string("")), "error")
                << "Second registration with mismatched hash must be rejected";
            EXPECT_EQ(resp2.value("error_code", std::string("")), "SCHEMA_MISMATCH")
                << "Error code must be SCHEMA_MISMATCH; got: " << resp2.dump();

            broker.stop_and_join();
        },
        "broker.broker_schema_mismatch",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.querier.notfound.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_channel_not_found", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            BrokerRequestComm brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = "prod.querier.notfound.uid00000001";
            brc_cfg.keystore_name   =
                pylabhub::tests::role_keystore_name(brc_cfg.role_uid);
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
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.dereg.ch.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_dereg_happy_path", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel = "broker.dereg.ch";
            const std::string uid     = "prod.dereg.ch.uid00000001";

            BrokerRequestComm brc;
            BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = broker.endpoint;
            brc_cfg.broker_pubkey   = broker.pubkey;
            brc_cfg.role_uid        = uid;
            brc_cfg.keystore_name   = pylabhub::tests::role_keystore_name(uid);
            ASSERT_TRUE(brc.connect(brc_cfg));
            std::atomic<bool> running{true};
            std::thread t([&] { brc.run_poll_loop([&] { return running.load(); }); });

            namespace sec = pylabhub::utils::security;
            auto reg_opts = pylabhub::hub::build_producer_reg_payload(
                pylabhub::hub::ProducerRegInputs{
                    .channel    = channel,
                    .role_uid   = uid,
                    .role_name  = "test_producer",
                    .role_type  = "producer",
                    .has_shm    = true,
                    .is_zmq_transport  = false,
                    .zmq_node_endpoint = {},
                    .zmq_pubkey = std::string{sec::key_store().pubkey(
                        pylabhub::tests::role_keystore_name(uid))},
                    .shm_capability_endpoint =
                        sec::default_shm_capability_endpoint(channel),
                });
            reg_opts["producer_pid"] = ::getpid();
            auto reg = brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel must succeed";

            brc.send_heartbeat(channel, uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Verify channel is discoverable
            auto found = brc.discover_channel(channel, {}, 5000);
            ASSERT_TRUE(found.has_value()) << "Channel must be discoverable before deregister";

            // Deregister.  Post-Bucket-C contract: assert on
            // status="success" explicitly; the implicit optional<json>
            // → bool conversion only checks `has_value()` (true for
            // both ACK and ERROR responses).
            {
                auto dereg = brc.deregister_channel(channel);
                ASSERT_TRUE(dereg.has_value());
                EXPECT_EQ(dereg->value("status", std::string{}), "success");
            }
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
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.pid_mismatch.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_dereg_pid_mismatch", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel  = "broker.pid_mismatch.ch";
            const std::string role_uid = "prod.broker.pid_mismatch.uid00000001";
            const uint64_t correct_pid = 55555;
            const uint64_t wrong_pid   = 99999;

            // Register via raw ZMQ.  Per HEP-CORE-0033 §G2.2.0a +
            // HEP-CORE-0023 §2.6, REG_REQ MUST carry `role_uid` for the
            // broker to create the producer-presence row that
            // subsequent DISC_REQ derives its observable from
            // (HEP-CORE-0023 §2.2 — Phase 4 protocol).
            nlohmann::json reg_req;
            reg_req["channel_name"]      = channel;
            reg_req["schema_hash"]       = zero_hex();
            reg_req["schema_version"]    = 1;
            reg_req["producer_pid"]      = correct_pid;
            reg_req["producer_hostname"] = "localhost";
            reg_req["role_uid"]          = role_uid;
            nlohmann::json reg_resp = raw_req(broker.endpoint, "REG_REQ", reg_req, 2000, broker.pubkey, "prod.broker.pid_mismatch.uid00000001");
            ASSERT_FALSE(reg_resp.is_null()) << "REG_REQ timed out";
            EXPECT_EQ(reg_resp.value("status", std::string("")), "success");

            // Send HEARTBEAT_REQ — must carry `role_uid` + `role_type`
            // per HEP-CORE-0019 §4.1 (Phase 6) + broker_proto 5
            // (R3.5b unification) so the broker flips the producer-
            // presence's `first_heartbeat_seen=true`, allowing
            // DISC_REQ later to resolve to DISC_ACK (presence Live).
            // Fire-and-forget: broker sends no reply; raw_req times out
            // quickly and we discard the empty json.
            nlohmann::json hb_req;
            hb_req["channel_name"] = channel;
            hb_req["producer_pid"] = correct_pid;
            hb_req["role_uid"]     = role_uid;
            hb_req["role_type"]    = "producer";
            raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.pid_mismatch.uid00000001");
            // Allow the heartbeat to be processed before DISC_REQ.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // DEREG_REQ with wrong pid + correct role_uid → NOT_REGISTERED.
            // broker_proto 2→3 (2026-05-15 audit C3): `role_uid` is now
            // REQUIRED; the broker resolves the target by (pid, role_uid)
            // tuple — a mismatch on EITHER half is NOT_REGISTERED.  This
            // test pins the pid-mismatch half; missing-role_uid is pinned
            // by `broker_dereg_missing_role_uid_rejected` below.
            nlohmann::json dereg_req;
            dereg_req["channel_name"] = channel;
            dereg_req["role_uid"]     = role_uid;
            dereg_req["producer_pid"] = wrong_pid;
            nlohmann::json dereg_resp = raw_req(broker.endpoint, "DEREG_REQ", dereg_req, 2000, broker.pubkey, "prod.broker.pid_mismatch.uid00000001");
            ASSERT_FALSE(dereg_resp.is_null()) << "DEREG_REQ timed out";
            EXPECT_EQ(dereg_resp.value("status", std::string("")), "error")
                << "DEREG_REQ with wrong pid must be rejected; got: " << dereg_resp.dump();
            EXPECT_EQ(dereg_resp.value("error_code", std::string("")), "NOT_REGISTERED")
                << "Error code must be NOT_REGISTERED; got: " << dereg_resp.dump();

            // Channel still discoverable via DISC_REQ.
            nlohmann::json disc_req;
            disc_req["channel_name"] = channel;
            nlohmann::json disc_resp = raw_req(broker.endpoint, "DISC_REQ", disc_req, 2000, broker.pubkey, "prod.broker.pid_mismatch.uid00000001");
            ASSERT_FALSE(disc_resp.is_null()) << "DISC_REQ timed out";
            EXPECT_EQ(disc_resp.value("status", std::string("")), "success")
                << "Channel must still be registered after pid-mismatch deregister attempt";
            // HEP-CORE-0036 §5b.4: shm_name retired (was always == channel_name).
            // The success status above already proves the channel is still registered.

            broker.stop_and_join();
        },
        "broker.broker_dereg_pid_mismatch",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ============================================================================
// broker_dereg_missing_role_uid_rejected — broker_proto 2→3 enforcement.
// DEREG_REQ + CONSUMER_DEREG_REQ now REQUIRE the `role_uid` field on the
// wire; missing-field clients receive INVALID_REQUEST instead of the
// previous pid-only-resolution fallback.  Audit C3 (2026-05-15) +
// HEP-CORE-0023 §2.1.1 multi-producer rationale.
// ============================================================================

int broker_dereg_missing_role_uid_rejected()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.missing_uid.uid00000001", "cons.broker.missing_uid.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_dereg_missing_role_uid_rejected", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel  = "broker.missing_uid.ch";
            const std::string role_uid = "prod.broker.missing_uid.uid00000001";
            const uint64_t    pid      = 44444;

            // Register a producer so the channel exists.
            nlohmann::json reg_req;
            reg_req["channel_name"]      = channel;
            reg_req["schema_hash"]       = zero_hex();
            reg_req["schema_version"]    = 1;
            reg_req["producer_pid"]      = pid;
            reg_req["producer_hostname"] = "localhost";
            reg_req["role_uid"]          = role_uid;
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg_req, 2000, broker.pubkey, "prod.broker.missing_uid.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // HEP-CORE-0036 §5.2 R6: drive producer presence to kLive
            // so the CONSUMER_REG_REQ below isn't deferred.  The
            // heartbeat must carry the producer's REG_REQ pid (44444)
            // so the broker matches it against the channel record.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = channel;
                hb_req["producer_pid"] = pid;
                hb_req["role_uid"]     = role_uid;
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100,
                        broker.pubkey, "prod.broker.missing_uid.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // DEREG_REQ with NO role_uid → INVALID_REQUEST (pre-C3 this
            // path succeeded by deriving role_uid from the matching pid).
            nlohmann::json dereg;
            dereg["channel_name"] = channel;
            dereg["producer_pid"] = pid;
            // deliberately omit role_uid
            nlohmann::json resp = raw_req(broker.endpoint, "DEREG_REQ", dereg, 2000, broker.pubkey, "prod.broker.missing_uid.uid00000001");
            ASSERT_FALSE(resp.is_null()) << "DEREG_REQ timed out";
            EXPECT_EQ(resp.value("status", std::string{}), "error")
                << "Missing role_uid must be rejected; got: " << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}), "INVALID_REQUEST")
                << "Error code must be INVALID_REQUEST; got: " << resp.dump();

            // Same shape for CONSUMER_DEREG_REQ — register a consumer
            // first (CONSUMER_DEREG without a registered consumer is
            // ambiguous between INVALID_REQUEST and NOT_REGISTERED;
            // registering pins the path through the field-validation
            // gate, not the resolution gate).
            const std::string cons_uid = "cons.broker.missing_uid.uid00000001";
            nlohmann::json cons_reg;
            cons_reg["channel_name"]      = channel;
            cons_reg["consumer_pid"]      = pid;
            cons_reg["consumer_hostname"] = "localhost";
            cons_reg["role_uid"]      = cons_uid;
            ASSERT_EQ(raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons_reg, 2000, broker.pubkey, cons_uid)
                          .value("status", std::string{}),
                      "success");

            nlohmann::json cons_dereg;
            cons_dereg["channel_name"] = channel;
            cons_dereg["consumer_pid"] = pid;
            // deliberately omit role_uid
            nlohmann::json cresp =
                raw_req(broker.endpoint, "CONSUMER_DEREG_REQ", cons_dereg, 2000, broker.pubkey, cons_uid);
            ASSERT_FALSE(cresp.is_null()) << "CONSUMER_DEREG_REQ timed out";
            EXPECT_EQ(cresp.value("status", std::string{}), "error")
                << "Missing role_uid on CONSUMER_DEREG must be rejected; got: "
                << cresp.dump();
            EXPECT_EQ(cresp.value("error_code", std::string{}), "INVALID_REQUEST")
                << "Error code must be INVALID_REQUEST; got: " << cresp.dump();

            broker.stop_and_join();
        },
        "broker.broker_dereg_missing_role_uid_rejected",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ============================================================================
// R3.5b (2026-05-19) — wire-boundary identifier validation at every gate.
// HEP-CORE-0033 §G2.2.0b grammar + side-aware role-tag policy.
// One worker per gate × failure mode to keep diagnostics focused.
// ============================================================================

namespace
{

/// Common broker fixture for R3.5b gate tests.  Returns a thread-owned
/// broker handle; the worker is responsible for `stop_and_join()`.
auto start_r35b_broker(const pylabhub::tests::CurveSetup &curve)
{
    BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0";
    return start_broker_in_thread(std::move(cfg), curve);
}

/// Boilerplate REG_REQ that's ALWAYS valid except for the fields a
/// test overrides explicitly.
nlohmann::json reg_req_template(const std::string &channel,
                                const std::string &role_uid)
{
    nlohmann::json r;
    r["channel_name"]     = channel;
    r["schema_hash"]      = zero_hex();
    r["schema_version"]   = 1;
    r["producer_pid"]     = pylabhub::platform::get_pid();
    r["producer_hostname"] = "localhost";
    r["role_uid"]         = role_uid;
    return r;
}

} // namespace

int broker_gate_reg_req_rejects_empty_uid()
{
    return run_gtest_worker(
        []() {
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"wire.gate.uid00000099"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_gate_reg_req_rejects_empty_uid", curve);
            auto broker = start_r35b_broker(curve);
            // REG_REQ with empty role_uid → INVALID_REQUEST (grammar).
            auto req = reg_req_template("r35b.empty_uid.ch", "");
            auto resp = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "wire.gate.uid00000099");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("status", std::string{}), "error");
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "INVALID_REQUEST");
            EXPECT_NE(resp.value("message", std::string{}).find("role_uid"),
                      std::string::npos)
                << "Error message should mention the offending field; got: "
                << resp.dump();
            broker.stop_and_join();
        },
        "broker.gate_reg_req_rejects_empty_uid",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_gate_reg_req_rejects_malformed_uid()
{
    return run_gtest_worker(
        []() {
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"wire.gate.uid00000099"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_gate_reg_req_rejects_malformed_uid", curve);
            auto broker = start_r35b_broker(curve);
            // "not-a-uid" → single component, no '.' → fails grammar.
            auto req = reg_req_template("r35b.malformed_uid.ch",
                                        "not-a-uid");
            auto resp = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "wire.gate.uid00000099");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "INVALID_REQUEST");

            // "prod." — has prefix tag but no name/unique components.
            req = reg_req_template("r35b.malformed_uid.ch2", "prod.");
            resp = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "wire.gate.uid00000099");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "INVALID_REQUEST");

            // "prod.x" — only 2 components; RoleUid requires ≥3.
            req = reg_req_template("r35b.malformed_uid.ch3", "prod.x");
            resp = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "wire.gate.uid00000099");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "INVALID_REQUEST");
            broker.stop_and_join();
        },
        "broker.gate_reg_req_rejects_malformed_uid",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_gate_reg_req_rejects_consumer_tag()
{
    return run_gtest_worker(
        []() {
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"wire.gate.uid00000099"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_gate_reg_req_rejects_consumer_tag", curve);
            auto broker = start_r35b_broker(curve);
            // role_uid="cons.x.y" on REG_REQ → INVALID_ROLE_TAG.
            // REG_REQ accepts {prod, proc}; consumer tag is wrong side.
            auto req = reg_req_template("r35b.wrong_tag.ch",
                                        "cons.r35b.uid00000001");
            auto resp = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "wire.gate.uid00000099");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "INVALID_ROLE_TAG");
            broker.stop_and_join();
        },
        "broker.gate_reg_req_rejects_consumer_tag",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_gate_reg_req_accepts_proc_tag()
{
    return run_gtest_worker(
        []() {
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"proc.r35b.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_gate_reg_req_accepts_proc_tag", curve);
            auto broker = start_r35b_broker(curve);
            // role_uid="proc.x.y" on REG_REQ → success.  Processor
            // roles register on the producer side for their output
            // channels per HEP-CORE-0011 Phase 6 dual-side model.
            auto req = reg_req_template("r35b.proc_as_prod.ch",
                                        "proc.r35b.uid00000001");
            auto resp = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "proc.r35b.uid00000001");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("status", std::string{}), "success")
                << "proc.* uid should be accepted on REG_REQ; got: "
                << resp.dump();
            broker.stop_and_join();
        },
        "broker.gate_reg_req_accepts_proc_tag",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_gate_consumer_reg_req_rejects_producer_tag()
{
    return run_gtest_worker(
        []() {
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.r35b.cregwt.uid00000001", "prod.r35b.intruder.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_gate_consumer_reg_req_rejects_producer_tag", curve);
            auto broker = start_r35b_broker(curve);
            // Need a channel to register a consumer to.
            const std::string channel  = "r35b.creg_wrong_tag.ch";
            const std::string prod_uid = "prod.r35b.cregwt.uid00000001";
            auto reg = reg_req_template(channel, prod_uid);
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.r35b.cregwt.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // CONSUMER_REG_REQ with role_uid="prod.x.y" → INVALID_ROLE_TAG.
            nlohmann::json creg;
            creg["channel_name"] = channel;
            creg["consumer_pid"] = pylabhub::platform::get_pid();
            creg["role_uid"]     = "prod.r35b.intruder.uid00000002";
            creg["role_name"]    = "intruder";
            auto resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", creg, 2000, broker.pubkey, "prod.r35b.cregwt.uid00000001");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "INVALID_ROLE_TAG");
            broker.stop_and_join();
        },
        "broker.gate_consumer_reg_req_rejects_producer_tag",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_gate_consumer_reg_req_accepts_proc_tag()
{
    return run_gtest_worker(
        []() {
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.r35b.cregproc.uid00000001", "proc.r35b.cregproc.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_gate_consumer_reg_req_accepts_proc_tag", curve);
            auto broker = start_r35b_broker(curve);
            const std::string channel  = "r35b.cregproc.ch";
            const std::string prod_uid = "prod.r35b.cregproc.uid00000001";
            auto reg = reg_req_template(channel, prod_uid);
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.r35b.cregproc.uid00000001")
                          .value("status", std::string{}),
                      "success");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "r35b.cregproc.ch";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.r35b.cregproc.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.r35b.cregproc.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // CONSUMER_REG_REQ with role_uid="proc.x.y" → success.
            nlohmann::json creg;
            creg["channel_name"] = channel;
            creg["consumer_pid"] = pylabhub::platform::get_pid();
            creg["role_uid"]     = "proc.r35b.cregproc.uid00000002";
            creg["role_name"]    = "proc_as_cons";
            auto resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", creg, 2000, broker.pubkey, "proc.r35b.cregproc.uid00000002");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("status", std::string{}), "success")
                << "proc.* uid should be accepted on CONSUMER_REG_REQ; got: "
                << resp.dump();
            broker.stop_and_join();
        },
        "broker.gate_consumer_reg_req_accepts_proc_tag",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.sch_b.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_record_path_b_created", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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

            auto resp = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "prod.broker.sch_b.uid00000001");
            ASSERT_FALSE(resp.is_null()) << "raw_req timed out";
            EXPECT_EQ(resp.value("status", std::string{}), "success")
                << "REG_REQ with full structure must succeed; got: " << resp.dump();

            // Wave M2.5 (controlled-access API design §6.2): same-uid
            // re-register is REJECTED with UID_CONFLICT, regardless of
            // whether the existing entry is active or stale-residue.
            // The schema record stays Created from the first REG_REQ
            // (the broker's anomaly handling preserves it); only the
            // admission itself fails.
            auto resp2 = raw_req(broker.endpoint, "REG_REQ", req, 2000, broker.pubkey, "prod.broker.sch_b.uid00000001");
            EXPECT_EQ(resp2.value("status", std::string{}), "error")
                << "Same-uid re-register must reject; got: " << resp2.dump();
            EXPECT_EQ(resp2.value("error_code", std::string{}), "UID_CONFLICT")
                << "Same-uid re-register error must be UID_CONFLICT; got: " << resp2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_record_path_b_created",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.sch_mm.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_record_hash_mismatch_self", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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
            auto r1 = raw_req(broker.endpoint, "REG_REQ", req1, 2000, broker.pubkey, "prod.broker.sch_mm.uid00000001");
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
            auto r2 = raw_req(broker.endpoint, "REG_REQ", req2, 2000, broker.pubkey, "prod.broker.sch_mm.uid00000001");
            ASSERT_FALSE(r2.is_null()) << "raw_req timed out";
            EXPECT_EQ(r2.value("status", std::string{}), "error")
                << "Second REG_REQ with different fingerprint must NACK; got: " << r2.dump();
            EXPECT_EQ(r2.value("error_code", std::string{}), "SCHEMA_HASH_MISMATCH_SELF")
                << "Error code must be SCHEMA_HASH_MISMATCH_SELF; got: " << r2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_record_hash_mismatch_self",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// broker_sch_consumer_citation_match: RETIRED 2026-06-28 under AUTH-6
// batch-2a C2.  Duplicate of
// `broker_schema_workers.cpp::consumer_schema_id_match_succeeds`
// (verified 2026-06-28 by reading both bodies + HEP-CORE-0034 §10.3
// — same protocol path, same assertion specificity).  Removed per
// `feedback_test_retirement_tracked_handoff`; the surviving twin in
// broker_schema_workers.cpp already pins the protocol contract.

// ── Consumer named-citation with WRONG hash → SCHEMA_CITATION_REJECTED ──────

int broker_sch_consumer_citation_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.sch_cbad.uid00000001", "cons.broker.sch_cbad.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_consumer_citation_mismatch", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.sch_cbad.uid00000001")
                          .value("status", std::string{}),
                      "success");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "broker.sch.cons_bad";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.broker.sch_cbad.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.sch_cbad.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Consumer cites the right id but a wrong hash (its local
            // expectation differs from what the producer registered).
            nlohmann::json cons_req;
            cons_req["channel_name"]         = channel;
            cons_req["role_uid"]         = c_uid;
            cons_req["role_name"]        = "test_consumer";
            cons_req["consumer_pid"]         = pylabhub::platform::get_pid();
            cons_req["expected_schema_id"]   = sid;
            cons_req["expected_schema_hash"] = hash_c;  // wrong
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons_req, 2000, broker.pubkey, "cons.broker.sch_cbad.uid00000002");
            ASSERT_FALSE(cr.is_null()) << "raw_req timed out";
            EXPECT_EQ(cr.value("status", std::string{}), "error")
                << "Hash mismatch must be NACKed; got: " << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}),
                      "SCHEMA_CITATION_REJECTED")
                << "Error code must be SCHEMA_CITATION_REJECTED; got: " << cr.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_consumer_citation_mismatch",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── REG_REQ without schema_packing → no record created (backward compat) ────

int broker_sch_no_packing_backward_compat()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.sch_bc.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_no_packing_backward_compat", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel = "broker.sch.bc";
            const std::string uid     = "prod.broker.sch_bc.uid00000001";

            // No schema_packing → new schema-record block is skipped.
            auto req1 = baseline_reg_req(channel, uid);
            req1["schema_hash"] = zero_hex();
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", req1, 2000, broker.pubkey, "prod.broker.sch_bc.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // Re-register with different hash, again without schema_packing.
            // The OLD channel-mismatch path runs (SCHEMA_MISMATCH), proving
            // the new HEP-0034 path was not taken (would have been
            // SCHEMA_HASH_MISMATCH_SELF instead).
            auto req2 = req1;
            req2["schema_hash"] = aa_hex();
            auto r2 = raw_req(broker.endpoint, "REG_REQ", req2, 2000, broker.pubkey, "prod.broker.sch_bc.uid00000001");
            ASSERT_EQ(r2.value("status", std::string{}), "error") << r2.dump();
            EXPECT_EQ(r2.value("error_code", std::string{}), "SCHEMA_MISMATCH")
                << "Backward-compat REG_REQ must use the legacy "
                   "SCHEMA_MISMATCH path, not the new "
                   "SCHEMA_HASH_MISMATCH_SELF; got: "
                << r2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_no_packing_backward_compat",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── SCHEMA_REQ owner+id keying (HEP-0034 §10.3) ─────────────────────────────

int broker_sch_schema_req_owner_id()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.schreq.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_schema_req_owner_id", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.schreq.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // New form: (owner, schema_id) — direct registry lookup.
            nlohmann::json sreq;
            sreq["owner"]     = uid;
            sreq["schema_id"] = sid;
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.schreq.uid00000001");
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
            auto bad = raw_req(broker.endpoint, "SCHEMA_REQ", bad_sreq, 2000, broker.pubkey, "prod.broker.schreq.uid00000001");
            EXPECT_EQ(bad.value("status", std::string{}), "error");
            EXPECT_EQ(bad.value("error_code", std::string{}), "SCHEMA_UNKNOWN");

            // Legacy form still works — channel_name returns the channel's
            // schema fields, and now also surfaces `schema_owner`.
            nlohmann::json legacy;
            legacy["channel_name"] = channel;
            auto lresp = raw_req(broker.endpoint, "SCHEMA_REQ", legacy, 2000, broker.pubkey, "prod.broker.schreq.uid00000001");
            EXPECT_EQ(lresp.value("status", std::string{}), "success");
            EXPECT_EQ(lresp.value("schema_owner", std::string{}), uid)
                << "Phase 3 channels expose their schema_owner via legacy SCHEMA_REQ";
            EXPECT_EQ(lresp.value("schema_id", std::string{}), sid);
            EXPECT_EQ(lresp.value("schema_hash", std::string{}), hash);

            broker.stop_and_join();
        },
        "broker.broker_sch_schema_req_owner_id",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Inbox path-A: REG_REQ inbox metadata creates record under (uid, "inbox") ─

int broker_sch_inbox_path_a()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.inbox.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_inbox_path_a", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel  = "broker.sch.inbox";
            const std::string uid      = "prod.broker.inbox.uid00000001";
            const std::string inbox_ep = "tcp://127.0.0.1:9988";
            const std::string ibj      = R"([{"type":"float64","count":1,"length":0}])";

            auto reg = baseline_reg_req(channel, uid);
            reg["inbox_endpoint"]    = inbox_ep;
            reg["inbox_schema_json"] = ibj;
            reg["inbox_packing"]     = "aligned";
            reg["inbox_checksum"]    = "enforced";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.inbox.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // SCHEMA_REQ for the inbox record returns the same fields the
            // broker recorded.  Hash and BLDS come back as-stored.
            nlohmann::json sreq;
            sreq["owner"]     = uid;
            sreq["schema_id"] = "inbox";
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.inbox.uid00000001");
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
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Inbox: same uid, different inbox schema → SCHEMA_HASH_MISMATCH_SELF ─────

int broker_sch_inbox_hash_mismatch_self()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.ibmm.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_inbox_hash_mismatch_self", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string ch1 = "broker.sch.inbox_mm.a";
            const std::string ch2 = "broker.sch.inbox_mm.b";
            const std::string uid = "prod.broker.ibmm.uid00000001";

            auto reg1 = baseline_reg_req(ch1, uid);
            reg1["inbox_endpoint"]    = "tcp://127.0.0.1:9989";
            reg1["inbox_schema_json"] = R"([{"type":"float64","count":1,"length":0}])";
            reg1["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg1, 2000, broker.pubkey, "prod.broker.ibmm.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // Same uid, NEW channel, different inbox schema.  HEP-0034
            // §11.4 says the inbox record is keyed by (role_uid, "inbox")
            // independent of channel name → second REG_REQ collides.
            auto reg2 = baseline_reg_req(ch2, uid);
            reg2["inbox_endpoint"]    = "tcp://127.0.0.1:9990";
            reg2["inbox_schema_json"] = R"([{"type":"int32","count":4,"length":0}])";
            reg2["inbox_packing"]     = "aligned";
            auto r2 = raw_req(broker.endpoint, "REG_REQ", reg2, 2000, broker.pubkey, "prod.broker.ibmm.uid00000001");
            ASSERT_FALSE(r2.is_null());
            EXPECT_EQ(r2.value("status", std::string{}), "error") << r2.dump();
            EXPECT_EQ(r2.value("error_code", std::string{}), "SCHEMA_HASH_MISMATCH_SELF");

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_hash_mismatch_self",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Inbox: idempotent re-registration (same uid + same fields → success) ────

int broker_sch_inbox_idempotent()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.idem.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_inbox_idempotent", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string ch1 = "broker.sch.inbox_idem.a";
            const std::string ch2 = "broker.sch.inbox_idem.b";
            const std::string uid = "prod.broker.idem.uid00000001";
            const std::string ibj = R"([{"type":"float64","count":1,"length":0}])";

            auto reg1 = baseline_reg_req(ch1, uid);
            reg1["inbox_endpoint"]    = "tcp://127.0.0.1:9991";
            reg1["inbox_schema_json"] = ibj;
            reg1["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg1, 2000, broker.pubkey, "prod.broker.idem.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // Same uid, same inbox schema, NEW channel — idempotent at
            // registry level, so the broker should accept (and HubState
            // returns kIdempotent without bumping the registered counter).
            auto reg2 = baseline_reg_req(ch2, uid);
            reg2["inbox_endpoint"]    = "tcp://127.0.0.1:9992";
            reg2["inbox_schema_json"] = ibj;       // identical fields
            reg2["inbox_packing"]     = "aligned"; // identical packing
            auto r2 = raw_req(broker.endpoint, "REG_REQ", reg2, 2000, broker.pubkey, "prod.broker.idem.uid00000001");
            EXPECT_EQ(r2.value("status", std::string{}), "success") << r2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_idempotent",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Inbox: malformed inbox_schema_json → INBOX_SCHEMA_INVALID ───────────────

int broker_sch_inbox_invalid_json()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.ibj.uid00000001", "prod.broker.ibj.uid000000011"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_inbox_invalid_json", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel = "broker.sch.inbox_bad_json";
            const std::string uid     = "prod.broker.ibj.uid00000001";

            // Parse error.
            auto reg = baseline_reg_req(channel, uid);
            reg["inbox_endpoint"]    = "tcp://127.0.0.1:9993";
            reg["inbox_schema_json"] = "not-json";
            reg["inbox_packing"]     = "aligned";
            auto r = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.ibj.uid00000001");
            ASSERT_FALSE(r.is_null());
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "INBOX_SCHEMA_INVALID");

            // Wrong shape (object, not array).
            auto reg2 = baseline_reg_req(channel + ".obj", uid + "1");
            reg2["inbox_endpoint"]    = "tcp://127.0.0.1:9994";
            reg2["inbox_schema_json"] = R"({"type":"float64"})"; // object, not array
            reg2["inbox_packing"]     = "aligned";
            auto r2 = raw_req(broker.endpoint, "REG_REQ", reg2, 2000, broker.pubkey, "prod.broker.ibj.uid00000001");
            EXPECT_EQ(r2.value("error_code", std::string{}), "INBOX_SCHEMA_INVALID");

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_invalid_json",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Inbox: two different roles, each with own inbox → both records exist ────

int broker_sch_inbox_two_owners()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.ib2a.uid00000001", "prod.broker.ib2b.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_inbox_two_owners", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", rA, 2000, broker.pubkey, "prod.broker.ib2a.uid00000001")
                          .value("status", std::string{}),
                      "success");

            auto rB = baseline_reg_req(ch_b, uid_b);
            rB["inbox_endpoint"]    = "tcp://127.0.0.1:9996";
            rB["inbox_schema_json"] = ibj;            // SAME content
            rB["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", rB, 2000, broker.pubkey, "prod.broker.ib2a.uid00000001")
                          .value("status", std::string{}),
                      "success") << "Different owner with same fields must succeed";

            // Both records resolvable via SCHEMA_REQ.
            for (const auto &uid : {uid_a, uid_b})
            {
                nlohmann::json sreq;
                sreq["owner"]     = uid;
                sreq["schema_id"] = "inbox";
                auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.ib2a.uid00000001");
                EXPECT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
                EXPECT_EQ(sresp.value("owner", std::string{}), uid);
            }

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_two_owners",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── SCHEMA_REQ with no key fields → INVALID_REQUEST ─────────────────────────

int broker_sch_schema_req_invalid()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.test.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_schema_req_invalid", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            // No owner, no schema_id, no channel_name — wire payload is
            // an object with no keys (NOT a null json — wire messages
            // are always objects).
            nlohmann::json sreq = nlohmann::json::object();
            auto resp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.test.uid00000001");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}), "INVALID_REQUEST");

            // Owner without schema_id → still INVALID_REQUEST (legacy form
            // requires channel_name; new form requires both owner + id).
            nlohmann::json half = nlohmann::json::object();
            half["owner"] = "prod.test.uid00000001";
            auto resp2 = raw_req(broker.endpoint, "SCHEMA_REQ", half, 2000, broker.pubkey, "prod.test.uid00000001");
            EXPECT_EQ(resp2.value("error_code", std::string{}), "INVALID_REQUEST");

            broker.stop_and_join();
        },
        "broker.broker_sch_schema_req_invalid",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Inbox packing must be "aligned" or "packed" ─────────────────────────────

int broker_sch_inbox_invalid_packing()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.ibp.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_inbox_invalid_packing", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel = "broker.sch.inbox_bad_pack";
            const std::string uid     = "prod.broker.ibp.uid00000001";

            auto reg = baseline_reg_req(channel, uid);
            reg["inbox_endpoint"]    = "tcp://127.0.0.1:9997";
            reg["inbox_schema_json"] = R"([{"type":"float64","count":1,"length":0}])";
            reg["inbox_packing"]     = "natural";  // invalid

            auto r = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.ibp.uid00000001");
            ASSERT_FALSE(r.is_null());
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "INVALID_INBOX_PACKING");

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_invalid_packing",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Phase 3 follow-up — Stage-2 verification + tightened gates ──────────────

int broker_sch_reg_missing_packing()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.miss_pack.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_reg_missing_packing", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            // schema_id non-empty but schema_packing missing → NACK MISSING_PACKING.
            auto reg = baseline_reg_req("broker.sch.miss_pack",
                                        "prod.broker.miss_pack.uid00000001");
            reg["schema_id"]      = "$lab.miss_pack.frame.v1";
            reg["schema_hash"]    = std::string(64, '0');
            reg["schema_blds"]    = "ts:f64:1:0";
            // intentionally no schema_packing
            auto r = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.miss_pack.uid00000001");
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "MISSING_PACKING");

            broker.stop_and_join();
        },
        "broker.broker_sch_reg_missing_packing",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_reg_fingerprint_inconsistent()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.fp_bad.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_reg_fingerprint_inconsistent", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            // Producer claims a hash that does NOT match BLDS+packing.
            // Stage-2 broker recomputes and rejects.
            auto reg = baseline_reg_req("broker.sch.fp_bad",
                                        "prod.broker.fp_bad.uid00000001");
            reg["schema_id"]      = "$lab.fp_bad.frame.v1";
            reg["schema_packing"] = "aligned";
            reg["schema_blds"]    = "ts:f64:1:0|value:f32:1:0";
            reg["schema_hash"]    = aa_hex();  // bogus — doesn't match canonical
            auto r = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.fp_bad.uid00000001");
            EXPECT_EQ(r.value("status", std::string{}), "error") << r.dump();
            EXPECT_EQ(r.value("error_code", std::string{}), "FINGERPRINT_INCONSISTENT");

            broker.stop_and_join();
        },
        "broker.broker_sch_reg_fingerprint_inconsistent",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_cons_named_missing_hash()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.cnh.uid00000001", "cons.broker.cnh.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_cons_named_missing_hash", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.cnh.uid00000001")
                          .value("status", std::string{}),
                      "success");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "broker.sch.cons_no_hash";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.broker.cnh.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.cnh.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Consumer cites by id but omits the hash → MISSING_HASH_FOR_NAMED_CITATION.
            nlohmann::json cons;
            cons["channel_name"]       = ch;
            cons["role_uid"]       = c;
            cons["role_name"]      = "test_consumer";
            cons["consumer_pid"]       = pylabhub::platform::get_pid();
            cons["expected_schema_id"] = sid;
            // intentionally no expected_schema_hash
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons, 2000, broker.pubkey, "cons.broker.cnh.uid00000002");
            EXPECT_EQ(cr.value("status", std::string{}), "error") << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}),
                      "MISSING_HASH_FOR_NAMED_CITATION");

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_named_missing_hash",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_cons_anonymous_happy_path()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.canok.uid00000001", "cons.broker.canok.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_cons_anonymous_happy_path", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.canok.uid00000001")
                          .value("status", std::string{}),
                      "success");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "broker.sch.cons_anon_ok";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.broker.canok.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.canok.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Consumer in anonymous mode: provides full structure (no id).
            // Hash optional — broker recomputes and compares to channel.
            nlohmann::json cons;
            cons["channel_name"]     = ch;
            cons["role_uid"]     = c;
            cons["role_name"]    = "test_consumer";
            cons["consumer_pid"]     = pylabhub::platform::get_pid();
            cons["expected_schema_blds"]    = blds;
            cons["expected_schema_packing"] = packing;
            // (no expected_schema_id, no expected_schema_hash)
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons, 2000, broker.pubkey, "cons.broker.canok.uid00000002");
            EXPECT_EQ(cr.value("status", std::string{}), "success") << cr.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_anonymous_happy_path",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_cons_anonymous_missing_packing()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.canp.uid00000001", "cons.broker.canp.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_cons_anonymous_missing_packing", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string ch = "broker.sch.cons_anon_nopack";
            const std::string p  = "prod.broker.canp.uid00000001";
            const std::string c  = "cons.broker.canp.uid00000002";

            auto reg = baseline_reg_req(ch, p);
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.canp.uid00000001")
                          .value("status", std::string{}),
                      "success");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "broker.sch.cons_anon_nopack";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.broker.canp.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.canp.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Anonymous mode with blds but no packing → NACK.
            nlohmann::json cons;
            cons["channel_name"]  = ch;
            cons["role_uid"]  = c;
            cons["role_name"] = "test_consumer";
            cons["consumer_pid"]  = pylabhub::platform::get_pid();
            cons["expected_schema_blds"] = "ts:f64:1:0";
            // intentionally no expected_packing
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons, 2000, broker.pubkey, "cons.broker.canp.uid00000002");
            EXPECT_EQ(cr.value("status", std::string{}), "error") << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}),
                      "MISSING_PACKING_FOR_ANONYMOUS_CITATION");

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_anonymous_missing_packing",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_cons_named_with_structure_mismatch()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.cnsb.uid00000001", "cons.broker.cnsb.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_cons_named_with_structure_mismatch", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

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
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.cnsb.uid00000001")
                          .value("status", std::string{}),
                      "success");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "broker.sch.cons_named_struct_bad";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.broker.cnsb.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.cnsb.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Consumer cites by id with correct hash, BUT also provides
            // a structure that doesn't match the channel's hash —
            // defense-in-depth check kicks in → FINGERPRINT_INCONSISTENT.
            nlohmann::json cons;
            cons["channel_name"]         = ch;
            cons["role_uid"]         = c;
            cons["role_name"]        = "test_consumer";
            cons["consumer_pid"]         = pylabhub::platform::get_pid();
            cons["expected_schema_id"]   = sid;
            cons["expected_schema_hash"] = canonical_hash_hex(blds_p, packing);
            cons["expected_schema_blds"]        = blds_c;   // diverges from producer
            cons["expected_schema_packing"]     = packing;
            auto cr = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons, 2000, broker.pubkey, "cons.broker.cnsb.uid00000002");
            EXPECT_EQ(cr.value("status", std::string{}), "error") << cr.dump();
            EXPECT_EQ(cr.value("error_code", std::string{}), "FINGERPRINT_INCONSISTENT");

            broker.stop_and_join();
        },
        "broker.broker_sch_cons_named_with_structure_mismatch",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_inbox_evicts_on_disconnect()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.ibev.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_inbox_evicts_on_disconnect", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            const std::string ch  = "broker.sch.inbox_evict";
            const std::string uid = "prod.broker.ibev.uid00000001";
            const auto producer_pid = pylabhub::platform::get_pid();

            // Register a producer with inbox metadata.
            auto reg = baseline_reg_req(ch, uid);
            reg["producer_pid"]      = producer_pid;
            reg["inbox_endpoint"]    = "tcp://127.0.0.1:9998";
            reg["inbox_schema_json"] = R"([{"type":"float64","count":1,"length":0}])";
            reg["inbox_packing"]     = "aligned";
            ASSERT_EQ(raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.ibev.uid00000001")
                          .value("status", std::string{}),
                      "success");

            // Confirm inbox record exists before disconnect.
            {
                nlohmann::json sreq;
                sreq["owner"]     = uid;
                sreq["schema_id"] = "inbox";
                auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.ibev.uid00000001");
                EXPECT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
            }

            // Trigger channel close via DEREG_REQ (cleaner than waiting
            // for heartbeat timeout — same `_on_channel_closed` cascade).
            // broker_proto 2→3: `role_uid` REQUIRED on DEREG_REQ.
            nlohmann::json dereg;
            dereg["channel_name"] = ch;
            dereg["role_uid"]     = uid;
            dereg["producer_pid"] = producer_pid;
            auto dr = raw_req(broker.endpoint, "DEREG_REQ", dereg, 2000, broker.pubkey, "prod.broker.ibev.uid00000001");
            ASSERT_EQ(dr.value("status", std::string{}), "success") << dr.dump();

            // Inbox record must be evicted by the cascade.
            nlohmann::json sreq;
            sreq["owner"]     = uid;
            sreq["schema_id"] = "inbox";
            auto after = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.ibev.uid00000001");
            EXPECT_EQ(after.value("status", std::string{}), "error") << after.dump();
            EXPECT_EQ(after.value("error_code", std::string{}), "SCHEMA_UNKNOWN")
                << "Inbox record should evict on producer disconnect "
                   "(HEP-CORE-0034 §7.2 cascade via _on_channel_closed)";

            broker.stop_and_join();
        },
        "broker.broker_sch_inbox_evicts_on_disconnect",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            cfg.schema_search_dirs = {schema_root.string()};
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"wire.gate.uid00000099"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_hub_globals_loaded_at_startup", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            // SCHEMA_REQ for (hub, $lab.demo.frame.v1) must succeed —
            // proves Phase 4b loaded the global into HubState.schemas.
            nlohmann::json sreq;
            sreq["owner"]     = "hub";
            sreq["schema_id"] = "$lab.demo.frame.v1";
            auto resp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "wire.gate.uid00000099");
            ASSERT_FALSE(resp.is_null());
            EXPECT_EQ(resp.value("status", std::string{}), "success") << resp.dump();
            EXPECT_EQ(resp.value("owner",     std::string{}), "hub");
            EXPECT_EQ(resp.value("schema_id", std::string{}), "$lab.demo.frame.v1");
            EXPECT_EQ(resp.value("packing",   std::string{}), "aligned");

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_hub_globals_loaded_at_startup",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            cfg.schema_search_dirs = {schema_root.string()};
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.adopt.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_path_c_adoption_succeeds", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel = "broker.sch.adopt";
            const std::string uid     = "prod.broker.adopt.uid00000001";
            auto reg = baseline_reg_req(channel, uid);
            reg["schema_owner"]   = "hub";          // path C
            reg["schema_id"]      = sid;
            reg["schema_hash"]    = hash;
            reg["schema_packing"] = packing;
            reg["schema_blds"]    = blds;
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.adopt.uid00000001");
            EXPECT_EQ(resp.value("status", std::string{}), "success") << resp.dump();

            // Verify channel.schema_owner == "hub" via legacy SCHEMA_REQ.
            nlohmann::json sreq;
            sreq["channel_name"] = channel;
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.adopt.uid00000001");
            EXPECT_EQ(sresp.value("schema_owner", std::string{}), "hub")
                << "Path-C-adopted channel should report owner=hub";

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_path_c_adoption_succeeds",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
            cfg.schema_search_dirs = {schema_root.string()};
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.mm.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_path_c_fingerprint_mismatch", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

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
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.mm.uid00000001");
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "FINGERPRINT_INCONSISTENT");

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_path_c_fingerprint_mismatch",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_path_c_unknown_global()
{
    return run_gtest_worker(
        []()
        {
            // No schema_search_dirs configured → no globals loaded.
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // Use an explicit (empty) override so the default dirs aren't
            // searched (would be flaky if /usr/share/pylabhub has files).
            cfg.schema_search_dirs = {std::filesystem::temp_directory_path() /
                                      ("plh_p4b_empty_" +
                                       std::to_string(::getpid()))};
            std::filesystem::create_directories(cfg.schema_search_dirs[0]);
            auto schema_root = std::filesystem::path(cfg.schema_search_dirs[0]);
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.unk.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_path_c_unknown_global", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

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
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.unk.uid00000001");
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}), "SCHEMA_UNKNOWN");

            broker.stop_and_join();
            std::filesystem::remove_all(schema_root);
        },
        "broker.broker_sch_path_c_unknown_global",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

int broker_sch_path_x_forbidden_owner()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.fbd.uid00000001"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_path_x_forbidden_owner", curve);
            auto broker = start_broker_in_thread(std::move(cfg), curve);

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
            auto resp = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.fbd.uid00000001");
            EXPECT_EQ(resp.value("status", std::string{}), "error") << resp.dump();
            EXPECT_EQ(resp.value("error_code", std::string{}),
                      "SCHEMA_FORBIDDEN_OWNER");

            broker.stop_and_join();
        },
        "broker.broker_sch_path_x_forbidden_owner",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// ── Wire-fields helpers × broker integration tests ──────────────────────────
//
// The wire-fields helpers (`make_wire_schema_fields`,
// `apply_producer_schema_fields`, `apply_consumer_schema_fields` in
// `schema_utils.hpp`) translate role-side schema state (config JSON +
// resolved SchemaSpec) into the HEP-CORE-0034 §10 wire payloads that
// REG_REQ / CONSUMER_REG_REQ carry.  The helpers are unit-tested in
// `test_schema_validation.cpp` (L2) at the JSON-output level; broker-side
// gates are tested elsewhere in this file with hand-crafted JSON.  These
// three workers test the seam between the two — a payload built entirely
// by the production helpers, sent to a real `BrokerService`, with full
// round-trip verification of the canonical-form bytes.

// Worker A — named citation, slot-only.
//
// Pins:
//   - HEP-0034 §6.3 type-token convention: helper emits JSON name form
//     ("float32"), broker recomputes the same bytes, hashes match.
//   - HEP-0034 §10.1 producer wire field set: schema_id / schema_blds /
//     schema_packing / schema_hash / schema_owner ("" for path B).
//   - HEP-0034 §10.2 consumer named-citation: expected_schema_id +
//     expected_schema_hash required, expected_schema_blds +
//     expected_schema_packing optional defense-in-depth.
//   - HEP-0034 §10.3 SCHEMA_REQ owner+id round-trip: helper-emitted
//     blds/hash equal what HubState stores.
//   - Idempotent re-register at the channel + record level.
int broker_sch_wire_helpers_register_and_cite()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.helpers_n.uid00000001", "cons.broker.helpers_n.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_wire_helpers_register_and_cite", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel  = "broker.sch.helpers.named";
            const std::string prod_uid = "prod.broker.helpers_n.uid00000001";
            const std::string cons_uid = "cons.broker.helpers_n.uid00000002";
            const std::string sid      = "$lab.helpers.frame.v1";

            pylabhub::hub::SchemaSpec slot_spec;
            slot_spec.has_schema = true;
            slot_spec.packing    = "aligned";
            slot_spec.fields.push_back({"ts",    "float64", 1u, 0u});
            slot_spec.fields.push_back({"value", "float32", 1u, 0u});
            const pylabhub::hub::SchemaSpec fz_spec; // no flexzone

            // Producer-side helper output: passing `sid` as string makes
            // make_wire_schema_fields populate w.schema_id (named mode).
            const auto w = pylabhub::hub::make_wire_schema_fields(
                nlohmann::json(sid), slot_spec, fz_spec);

            // Helper-output sanity: HEP-0034 §6.3 canonical bytes.
            const std::string blds_json_form = "ts:float64:1:0|value:float32:1:0";
            const std::string expected_hash =
                canonical_hash_hex(blds_json_form, "aligned");
            ASSERT_EQ(w.schema_blds, blds_json_form)
                << "make_wire_schema_fields must emit JSON-name canonical form (§6.3)";
            ASSERT_EQ(w.schema_packing, "aligned");
            ASSERT_EQ(w.schema_id, sid);
            ASSERT_EQ(w.schema_hash, expected_hash);
            ASSERT_TRUE(w.flexzone_blds.empty());
            ASSERT_TRUE(w.flexzone_packing.empty());

            // ── Producer REG_REQ via apply_producer_schema_fields ─────
            auto reg = baseline_reg_req(channel, prod_uid);
            pylabhub::hub::apply_producer_schema_fields(reg, w);

            // Helper-emitted JSON keys must match the §10.1 wire field set.
            ASSERT_TRUE(reg.contains("schema_id"));
            ASSERT_TRUE(reg.contains("schema_hash"));
            ASSERT_TRUE(reg.contains("schema_blds"));
            ASSERT_TRUE(reg.contains("schema_packing"));
            ASSERT_FALSE(reg.contains("schema_owner"))
                << "Empty schema_owner must NOT be emitted (path B is implicit)";
            ASSERT_FALSE(reg.contains("flexzone_blds"));
            ASSERT_FALSE(reg.contains("flexzone_packing"));

            auto reg_resp = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.helpers_n.uid00000001");
            ASSERT_FALSE(reg_resp.is_null()) << "REG_REQ raw_req timed out";
            ASSERT_EQ(reg_resp.value("status", std::string{}), "success")
                << "Helper-built REG_REQ must succeed; got: " << reg_resp.dump();

            // HEP-CORE-0036 §5.2 R6: producer must reach kLive (first
            // heartbeat seen) before broker admits CONSUMER_REG_REQ.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = channel;
                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = prod_uid;
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100,
                        broker.pubkey, "prod.broker.helpers_n.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // ── SCHEMA_REQ owner+id round-trip ─────────────────────────
            // path B → owner = role uid; helper hash/blds must equal what
            // HubState stores (HEP-0034 §10.3).
            nlohmann::json sreq;
            sreq["owner"]     = prod_uid;
            sreq["schema_id"] = sid;
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.helpers_n.uid00000001");
            ASSERT_FALSE(sresp.is_null()) << "SCHEMA_REQ raw_req timed out";
            ASSERT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
            EXPECT_EQ(sresp.value("owner", std::string{}), prod_uid);
            EXPECT_EQ(sresp.value("schema_id", std::string{}), sid);
            EXPECT_EQ(sresp.value("packing", std::string{}), "aligned");
            EXPECT_EQ(sresp.value("blds", std::string{}), w.schema_blds)
                << "SchemaRecord.blds must equal helper-emitted canonical bytes";
            EXPECT_EQ(sresp.value("schema_hash", std::string{}), w.schema_hash)
                << "SchemaRecord.hash (hex) must equal helper-emitted fingerprint";

            // ── Consumer CONSUMER_REG_REQ via apply_consumer_schema_fields ─
            const auto wc = pylabhub::hub::make_wire_schema_fields(
                nlohmann::json(sid), slot_spec, fz_spec);
            ASSERT_EQ(wc.schema_hash, w.schema_hash)
                << "Consumer-side helper must produce same fingerprint as producer";

            nlohmann::json cons;
            cons["channel_name"]      = channel;
            cons["consumer_pid"]      = pylabhub::platform::get_pid();
            cons["consumer_hostname"] = "localhost";
            cons["role_uid"]      = cons_uid;
            cons["role_name"]     = "test_consumer";
            pylabhub::hub::apply_consumer_schema_fields(cons, wc);

            // Helper must emit the §10.2 expected_schema_* prefix consistently.
            ASSERT_TRUE(cons.contains("expected_schema_id"));
            ASSERT_TRUE(cons.contains("expected_schema_hash"));
            ASSERT_TRUE(cons.contains("expected_schema_blds"));
            ASSERT_TRUE(cons.contains("expected_schema_packing"));
            EXPECT_FALSE(cons.contains("expected_blds"))
                << "wire field rename (§10.2): bare expected_blds must not be emitted";
            EXPECT_FALSE(cons.contains("expected_packing"))
                << "wire field rename (§10.2): bare expected_packing must not be emitted";

            auto creg_resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons, 2000, broker.pubkey, "cons.broker.helpers_n.uid00000002");
            ASSERT_FALSE(creg_resp.is_null()) << "CONSUMER_REG_REQ raw_req timed out";
            EXPECT_EQ(creg_resp.value("status", std::string{}), "success")
                << "Helper-built CONSUMER_REG_REQ (named, with defense-in-depth "
                   "structure) must succeed; got: " << creg_resp.dump();

            // Wave M2.5 (controlled-access API design §6.2): same-uid
            // re-register is REJECTED with UID_CONFLICT.  The schema
            // record stays Created from the first REG_REQ; the channel
            // record and the already-admitted consumer are unaffected
            // (admission fails before any state mutation).
            //
            // Pre-MP2.5 contract was "idempotent re-register succeeds";
            // the new policy treats same-uid re-registration as a
            // bookkeeping anomaly (residue or breach) — see
            // docs/tech_draft/controlled_access_api_design.md §6.2.
            auto reg_resp2 = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.helpers_n.uid00000001");
            ASSERT_FALSE(reg_resp2.is_null());
            EXPECT_EQ(reg_resp2.value("status", std::string{}), "error")
                << "Same-uid re-register must reject; got: "
                << reg_resp2.dump();
            EXPECT_EQ(reg_resp2.value("error_code", std::string{}),
                      "UID_CONFLICT")
                << "Same-uid re-register error must be UID_CONFLICT; got: "
                << reg_resp2.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_wire_helpers_register_and_cite",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// Worker B — anonymous citation via helpers.
//
// Producer registers a named-mode schema (any channel needs an owner +
// fingerprint).  Consumer cites in anonymous mode by passing a non-string
// `slot_schema_json` to the helper, which leaves `WireSchemaFields::schema_id`
// empty while still populating `schema_blds` / `schema_packing` /
// `schema_hash` from the SchemaSpec.  Pins:
//   - Helper mode-selection seam: `if (slot_schema_json.is_string())` in
//     make_wire_schema_fields (schema_utils.hpp:412).  Non-string input
//     → anonymous output.
//   - HEP-0034 §10.3 anonymous-citation broker path: full structure
//     required, broker recomputes hash and matches channel's stored hash.
//   - apply_consumer_schema_fields skips the empty-id branch (no
//     `expected_schema_id` key emitted) — broker dispatches via the
//     `else if` arm at handle_consumer_reg_req.
int broker_sch_wire_helpers_anonymous_citation()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.helpers_a.uid00000001", "cons.broker.helpers_a.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_wire_helpers_anonymous_citation", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel  = "broker.sch.helpers.anon";
            const std::string prod_uid = "prod.broker.helpers_a.uid00000001";
            const std::string cons_uid = "cons.broker.helpers_a.uid00000002";
            const std::string sid      = "$lab.helpers_anon.frame.v1";

            pylabhub::hub::SchemaSpec slot_spec;
            slot_spec.has_schema = true;
            slot_spec.packing    = "aligned";
            slot_spec.fields.push_back({"ts",    "float64", 1u, 0u});
            slot_spec.fields.push_back({"value", "float32", 1u, 0u});
            const pylabhub::hub::SchemaSpec fz_spec;

            // Producer registers under named mode (the channel needs a
            // schema_owner for any subsequent citation to resolve).
            const auto wp = pylabhub::hub::make_wire_schema_fields(
                nlohmann::json(sid), slot_spec, fz_spec);
            auto reg = baseline_reg_req(channel, prod_uid);
            pylabhub::hub::apply_producer_schema_fields(reg, wp);
            auto reg_resp = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.helpers_a.uid00000001");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "broker.sch.helpers.anon";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.broker.helpers_a.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.helpers_a.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ASSERT_FALSE(reg_resp.is_null());
            ASSERT_EQ(reg_resp.value("status", std::string{}), "success") << reg_resp.dump();

            // Consumer-side: feed a non-string sentinel (empty JSON
            // object) so make_wire_schema_fields skips the `is_string()`
            // branch and leaves schema_id empty.  Same SchemaSpec →
            // same fingerprint as the producer.  In production this
            // path is taken when a role's config carries inline
            // structure (`"slot_schema": {"fields": [...]}`) instead
            // of a schema_id string.
            const auto wc = pylabhub::hub::make_wire_schema_fields(
                nlohmann::json::object(), slot_spec, fz_spec);
            ASSERT_TRUE(wc.schema_id.empty())
                << "non-string slot_schema_json must leave schema_id empty";
            ASSERT_FALSE(wc.schema_blds.empty());
            ASSERT_FALSE(wc.schema_packing.empty());
            ASSERT_FALSE(wc.schema_hash.empty());
            EXPECT_EQ(wc.schema_hash, wp.schema_hash)
                << "Same SchemaSpec must produce same fingerprint regardless of id";

            nlohmann::json cons;
            cons["channel_name"]      = channel;
            cons["consumer_pid"]      = pylabhub::platform::get_pid();
            cons["consumer_hostname"] = "localhost";
            cons["role_uid"]      = cons_uid;
            cons["role_name"]     = "test_consumer_anon";
            pylabhub::hub::apply_consumer_schema_fields(cons, wc);

            // Anonymous-mode shape: id NOT emitted, structure fields emitted.
            EXPECT_FALSE(cons.contains("expected_schema_id"))
                << "Empty schema_id must not be emitted as a key";
            ASSERT_TRUE(cons.contains("expected_schema_blds"));
            ASSERT_TRUE(cons.contains("expected_schema_packing"));
            ASSERT_TRUE(cons.contains("expected_schema_hash"))
                << "Helper still emits hash; broker uses it for §10.3 self-consistency";

            auto creg_resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons, 2000, broker.pubkey, "cons.broker.helpers_a.uid00000002");
            ASSERT_FALSE(creg_resp.is_null()) << "CONSUMER_REG_REQ raw_req timed out";
            EXPECT_EQ(creg_resp.value("status", std::string{}), "success")
                << "Helper-built anonymous CONSUMER_REG_REQ must succeed (HEP-0034 "
                   "§10.3 anonymous mode); got: " << creg_resp.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_wire_helpers_anonymous_citation",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
}

// Worker D — slot + flexzone via helpers, full round-trip.
//
// Pins:
//   - HEP-0034 §6.3 fz section: helper folds flexzone_blds /
//     flexzone_packing into the canonical bytes
//     (compute_canonical_hash_from_wire), broker recomputes the same
//     bytes, hashes match.
//   - apply_producer_schema_fields emits the flexzone_* keys (the gap
//     Phase 4a closed at the broker side, mirrored on the producer
//     side here).
//   - SchemaRecord stores slot-only `blds` / `packing` (per
//     to_hub_schema_record), but `hash` includes the flexzone — verified
//     by SCHEMA_REQ owner+id round-trip below (response.schema_hash ==
//     helper-computed hash that includes the fz section).
//   - apply_consumer_schema_fields emits expected_flexzone_blds /
//     expected_flexzone_packing; broker's CONSUMER_REG_REQ handler
//     reads and folds them into the recompute (Phase 5a fix mirrored
//     for the consumer side).
int broker_sch_wire_helpers_flexzone_round_trip()
{
    return run_gtest_worker(
        []()
        {
            BrokerService::Config cfg;
            cfg.endpoint  = "tcp://127.0.0.1:0";
            // CURVE setup: seed `key_store()` + `vault/known_roles.json`
            // so the broker's Layer-1 ZAP gate (HEP-CORE-0035 §4.8)
            // admits each role uid we'll register below.
            auto curve = pylabhub::tests::make_curve_setup({"prod.broker.helpers_fz.uid00000001", "cons.broker.helpers_fz.uid00000002"});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture("test.l3", "broker.broker_sch_wire_helpers_flexzone_round_trip", curve);
            auto broker   = start_broker_in_thread(std::move(cfg), curve);

            const std::string channel  = "broker.sch.helpers.fz";
            const std::string prod_uid = "prod.broker.helpers_fz.uid00000001";
            const std::string cons_uid = "cons.broker.helpers_fz.uid00000002";
            const std::string sid      = "$lab.helpers_fz.frame.v1";

            // Slot: ts (float64), value (float32) — packing aligned.
            pylabhub::hub::SchemaSpec slot_spec;
            slot_spec.has_schema = true;
            slot_spec.packing    = "aligned";
            slot_spec.fields.push_back({"ts",    "float64", 1u, 0u});
            slot_spec.fields.push_back({"value", "float32", 1u, 0u});

            // Flexzone: cal (float64[8]) — packing aligned.  Distinct from
            // slot to verify the canonical form preserves field order +
            // section semantics.
            pylabhub::hub::SchemaSpec fz_spec;
            fz_spec.has_schema = true;
            fz_spec.packing    = "aligned";
            fz_spec.fields.push_back({"cal", "float64", 8u, 0u});

            // Helper-emitted full payload, slot + fz folded into the hash.
            const auto w = pylabhub::hub::make_wire_schema_fields(
                nlohmann::json(sid), slot_spec, fz_spec);

            const std::string slot_blds = "ts:float64:1:0|value:float32:1:0";
            const std::string fz_blds   = "cal:float64:8:0";
            const std::string expected_hash =
                canonical_hash_hex(slot_blds, "aligned", fz_blds, "aligned");
            ASSERT_EQ(w.schema_blds,    slot_blds);
            ASSERT_EQ(w.schema_packing, "aligned");
            ASSERT_EQ(w.flexzone_blds,    fz_blds);
            ASSERT_EQ(w.flexzone_packing, "aligned");
            ASSERT_EQ(w.schema_hash, expected_hash)
                << "Helper hash must include flexzone canonical bytes (§6.3 fz section)";

            // ── Producer REG_REQ ─────────────────────────────────────────
            auto reg = baseline_reg_req(channel, prod_uid);
            pylabhub::hub::apply_producer_schema_fields(reg, w);
            ASSERT_TRUE(reg.contains("flexzone_blds"))
                << "Helper must emit flexzone_blds key when fz_spec.has_schema";
            ASSERT_TRUE(reg.contains("flexzone_packing"));

            auto reg_resp = raw_req(broker.endpoint, "REG_REQ", reg, 2000, broker.pubkey, "prod.broker.helpers_fz.uid00000001");
            // HEP-CORE-0036 §5.2 R6: broker rejects CONSUMER_REG_REQ until
            // producer's presence is kLive (first heartbeat seen).  Tests
            // pre-dated R6; this heartbeat unblocks the consumer reg below.
            {
                nlohmann::json hb_req;
                hb_req["channel_name"] = "broker.sch.helpers.fz";

                hb_req["producer_pid"] = pylabhub::platform::get_pid();
                hb_req["role_uid"]     = "prod.broker.helpers_fz.uid00000001";
                hb_req["role_type"]    = "producer";
                raw_req(broker.endpoint, "HEARTBEAT_REQ", hb_req, 100, broker.pubkey, "prod.broker.helpers_fz.uid00000001");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ASSERT_FALSE(reg_resp.is_null());
            ASSERT_EQ(reg_resp.value("status", std::string{}), "success")
                << "Slot+flexzone REG_REQ via helpers must succeed; broker recomputes "
                   "the full canonical form (Phase 4a fix); got: " << reg_resp.dump();

            // ── SCHEMA_REQ owner+id: hash includes flexzone ──────────────
            nlohmann::json sreq;
            sreq["owner"]     = prod_uid;
            sreq["schema_id"] = sid;
            auto sresp = raw_req(broker.endpoint, "SCHEMA_REQ", sreq, 2000, broker.pubkey, "prod.broker.helpers_fz.uid00000001");
            ASSERT_FALSE(sresp.is_null());
            ASSERT_EQ(sresp.value("status", std::string{}), "success") << sresp.dump();
            EXPECT_EQ(sresp.value("schema_hash", std::string{}), w.schema_hash)
                << "SchemaRecord.hash must include flexzone in canonical form";

            // ── Consumer CONSUMER_REG_REQ with slot + flexzone ───────────
            // Defense-in-depth: consumer supplies the full structure; broker
            // recomputes including flexzone fields and matches the channel's
            // hash.  Verifies expected_flexzone_blds / expected_flexzone_packing
            // path (mirror of Phase 4a on the consumer side).
            const auto wc = pylabhub::hub::make_wire_schema_fields(
                nlohmann::json(sid), slot_spec, fz_spec);
            ASSERT_EQ(wc.schema_hash, w.schema_hash);

            nlohmann::json cons;
            cons["channel_name"]      = channel;
            cons["consumer_pid"]      = pylabhub::platform::get_pid();
            cons["consumer_hostname"] = "localhost";
            cons["role_uid"]      = cons_uid;
            cons["role_name"]     = "test_consumer_fz";
            pylabhub::hub::apply_consumer_schema_fields(cons, wc);

            ASSERT_TRUE(cons.contains("expected_flexzone_blds"))
                << "Consumer helper must emit expected_flexzone_blds for fz spec";
            ASSERT_TRUE(cons.contains("expected_flexzone_packing"));

            auto creg_resp = raw_req(broker.endpoint, "CONSUMER_REG_REQ", cons, 2000, broker.pubkey, "cons.broker.helpers_fz.uid00000002");
            ASSERT_FALSE(creg_resp.is_null());
            EXPECT_EQ(creg_resp.value("status", std::string{}), "success")
                << "Helper-built CONSUMER_REG_REQ with slot+flexzone (named, "
                   "defense-in-depth) must succeed; broker must include flexzone "
                   "fields in the recomputed fingerprint; got: " << creg_resp.dump();

            broker.stop_and_join();
        },
        "broker.broker_sch_wire_helpers_flexzone_round_trip",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), hub_module(), zmq_module());
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
                if (scenario == "broker_dereg_missing_role_uid_rejected")
                    return broker_dereg_missing_role_uid_rejected();
                if (scenario == "gate_reg_req_rejects_empty_uid")
                    return broker_gate_reg_req_rejects_empty_uid();
                if (scenario == "gate_reg_req_rejects_malformed_uid")
                    return broker_gate_reg_req_rejects_malformed_uid();
                if (scenario == "gate_reg_req_rejects_consumer_tag")
                    return broker_gate_reg_req_rejects_consumer_tag();
                if (scenario == "gate_reg_req_accepts_proc_tag")
                    return broker_gate_reg_req_accepts_proc_tag();
                if (scenario == "gate_consumer_reg_req_rejects_producer_tag")
                    return broker_gate_consumer_reg_req_rejects_producer_tag();
                if (scenario == "gate_consumer_reg_req_accepts_proc_tag")
                    return broker_gate_consumer_reg_req_accepts_proc_tag();
                if (scenario == "broker_sch_record_path_b_created")
                    return broker_sch_record_path_b_created();
                if (scenario == "broker_sch_record_hash_mismatch_self")
                    return broker_sch_record_hash_mismatch_self();
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
                if (scenario == "broker_sch_wire_helpers_register_and_cite")
                    return broker_sch_wire_helpers_register_and_cite();
                if (scenario == "broker_sch_wire_helpers_anonymous_citation")
                    return broker_sch_wire_helpers_anonymous_citation();
                if (scenario == "broker_sch_wire_helpers_flexzone_round_trip")
                    return broker_sch_wire_helpers_flexzone_round_trip();
                fmt::print(stderr, "ERROR: Unknown broker scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerWorkerRegistrar g_broker_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
