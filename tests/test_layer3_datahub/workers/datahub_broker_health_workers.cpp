// tests/test_layer3_datahub/workers/datahub_broker_health_workers.cpp
//
// Broker health and notification tests via BrokerRequestComm.
#include "datahub_broker_health_workers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/security/zap_router.hpp"  // HEP-CORE-0035 §4.2 deny-path observability
#include "plh_datahub.hpp"

#include <zmq.h>  // zmq_curve_keypair for the deny-path test

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;
using pylabhub::broker::ChannelSnapshot;

namespace pylabhub::tests::worker::broker_health
{

static auto logger_module()    { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto file_lock_module() { return ::pylabhub::utils::FileLock::GetLifecycleModule(); }
static auto json_module()      { return ::pylabhub::utils::JsonConfig::GetLifecycleModule(); }
static auto crypto_module()    { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto zmq_module()       { return ::pylabhub::hub::GetZMQContextModule(); }

// ============================================================================
// Shared helpers
// ============================================================================

namespace
{

// Real-HubHost RAII wrapper — replaces the legacy BrokerHandle mock per
// the test-design principle in `docs/todo/TESTING_TODO.md`
// §"Test Design Principles".  The wire entry points (endpoint, pubkey)
// are preserved; the assembly behind them is now the production
// HubHost (HubConfig → ThreadManager-launched broker thread).
struct BrokerHandle
{
    std::filesystem::path                            hub_dir;
    std::unique_ptr<::pylabhub::hub_host::HubHost>   host;
    std::string                                      endpoint;
    std::string                                      pubkey;

    ~BrokerHandle() { stop_and_join(); }

    BrokerHandle()                                = default;
    BrokerHandle(const BrokerHandle &)            = delete;
    BrokerHandle &operator=(const BrokerHandle &) = delete;
    BrokerHandle(BrokerHandle &&)                 = default;
    BrokerHandle &operator=(BrokerHandle &&)      = default;

    void stop_and_join()
    {
        if (host)
            host->shutdown();
        host.reset();
        if (!hub_dir.empty())
        {
            std::error_code ec;
            std::filesystem::remove_all(hub_dir, ec);
            hub_dir.clear();
        }
    }

    /// Convenience accessor — preserves the `broker.service->method()`
    /// shape the existing scenarios use, now backed by HubHost::broker().
    BrokerService &service_ref() { return host->broker(); }
};

namespace fs = std::filesystem;

BrokerHandle start_broker_with_cfg(BrokerService::Config legacy_cfg)
{
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_health_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    ::pylabhub::utils::HubDirectory::init_directory(dir, "HealthTestHub");

    const fs::path hub_json = dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json);
        if (f.is_open())
            j = nlohmann::json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";
    // Translate broker config overrides from the legacy
    // `BrokerService::Config` shape into HubConfig.broker.
    if (legacy_cfg.heartbeat_interval.count() > 0)
        j["broker"]["heartbeat_interval_ms"] =
            static_cast<int>(legacy_cfg.heartbeat_interval.count());
    if (legacy_cfg.ready_miss_heartbeats > 0)
        j["broker"]["ready_miss_heartbeats"] = legacy_cfg.ready_miss_heartbeats;
    if (legacy_cfg.pending_miss_heartbeats > 0)
        j["broker"]["pending_miss_heartbeats"] =
            legacy_cfg.pending_miss_heartbeats;
    if (legacy_cfg.ready_timeout_override.has_value())
        j["broker"]["ready_timeout_ms"] =
            static_cast<int>(legacy_cfg.ready_timeout_override->count());
    if (legacy_cfg.pending_timeout_override.has_value())
        j["broker"]["pending_timeout_ms"] =
            static_cast<int>(legacy_cfg.pending_timeout_override->count());
    if (legacy_cfg.checksum_repair_policy ==
        ::pylabhub::broker::ChecksumRepairPolicy::NotifyOnly)
        j["broker"]["checksum_repair_policy"] = "notify_only";
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
    fs::create_directories(dir / "schemas");

    BrokerHandle h;
    h.hub_dir = std::move(dir);
    h.host    = std::make_unique<::pylabhub::hub_host::HubHost>(
        ::pylabhub::config::HubConfig::load_from_directory(
            h.hub_dir.string()));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = h.host->broker_pubkey();
    return h;
}

// PURPOSE: broker_health tests exercise broker-internal lifecycle
//   (CHANNEL_CLOSING_NOTIFY, CONSUMER_DIED_NOTIFY, heartbeat-timeout
//   FSM sweeps), NOT the PeerAdmission CURVE gate.
// BYPASS: `start_broker_with_cfg` goes through HubHost, which
//   derives `use_curve` from the vault's auth.keyfile (empty in
//   this fixture → use_curve resolves to false in production code
//   path).  Per-call `cfg.use_curve = true` + `enforce = false` are
//   defensive belt-and-suspenders for the direct-Config paths
//   below that bypass HubHost.
// WHY: Spinning up a real vault (--keygen) for every health test
//   is excessive overhead for behaviors that have nothing to do
//   with CURVE.  The deny-PATH security pin lives in
//   `DatahubBrokerHealthTest.CtrlZapDenyPath`.
// CANONICAL STORAGE: `BrokerCtrlAdmission::current_`
//   (`broker_service.cpp`); permissive when `enforce=false`.
// RE-EXAMINE WHEN: a health behavior gains semantics that depend
//   on the role's CURVE pubkey (would surface as a CHANNEL_*_NOTIFY
//   wire-field amendment in HEP-CORE-0023).
BrokerHandle start_broker()
{
    BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0";
    cfg.use_curve = true;  // legacy flag — ignored under real HubHost
    cfg.enforce_ctrl_admission = false;  // see fixture block above
    return start_broker_with_cfg(std::move(cfg));
}

/// BRC wrapper with poll thread — same as test helper BrcHandle pattern.
struct BrcHandle
{
    BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread       thread;

    void start(const std::string &ep, const std::string &pk, const std::string &uid)
    {
        BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = ep;
        cfg.broker_pubkey   = pk;
        cfg.role_uid        = uid;
        ASSERT_TRUE(brc.connect(cfg));
        thread = std::thread([this] { brc.run_poll_loop([this] { return running.load(); }); });
    }

    void stop()
    {
        running.store(false);
        brc.stop();
        if (thread.joinable())
            thread.join();
        brc.disconnect();
    }

    ~BrcHandle()
    {
        if (thread.joinable())
            stop();
    }
};

nlohmann::json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

nlohmann::json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    nlohmann::json opts;
    opts["channel_name"]  = channel;
    opts["role_uid"]  = consumer_uid;
    opts["role_name"] = "test_consumer";
    opts["consumer_pid"]  = ::getpid();
    return opts;
}

} // anonymous namespace

// ============================================================================
// producer_gets_closing_notify
// ============================================================================

int producer_gets_closing_notify(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint        = "tcp://127.0.0.1:0";
            cfg.use_curve       = true;
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
            // Fast reclaim for test: total ~1s window.
            cfg.ready_timeout_override   = std::chrono::milliseconds(500);
            cfg.pending_timeout_override = std::chrono::milliseconds(500);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name = make_test_channel_name("health.closing_notify");
            const std::string uid     = "prod." + ch_name;

            std::atomic<bool> closing_fired{false};

            BrcHandle bh;
            bh.brc.on_notification([&](const std::string &type, const nlohmann::json &)
            {
                if (type == "CHANNEL_CLOSING_NOTIFY")
                    closing_fired.store(true);
            });
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch_name, uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            // Send one heartbeat to mark Ready, then stop sending.
            bh.brc.send_heartbeat(ch_name, uid, "producer", {});

            // Wait ~1s for state machine to demote Ready -> Pending -> deregister + CHANNEL_CLOSING_NOTIFY.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!closing_fired.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(closing_fired.load())
                << "CHANNEL_CLOSING_NOTIFY was not received within 5s";

            bh.stop();
            broker.stop_and_join();
        },
        "broker_health.producer_gets_closing_notify",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// ============================================================================
// consumer_auto_deregisters
// ============================================================================

int consumer_auto_deregisters(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            const std::string ch_name  = make_test_channel_name("health.consumer_dereg");
            const std::string prod_uid = "prod." + ch_name;
            const std::string cons_uid = "cons." + ch_name;

            // Register producer
            BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            prod_bh.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Register consumer
            BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid);
            auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(ch_name, cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value());

            // Consumer deregisters.  Post-Bucket-C: assert on
            // status="success" rather than relying on optional<json>
            // implicit-to-bool, which is true for ERROR responses too.
            {
                auto dereg = cons_bh.brc.deregister_consumer(ch_name);
                ASSERT_TRUE(dereg.has_value());
                EXPECT_EQ(dereg->value("status", std::string{}), "success");
            }

            // Wait until the broker reflects consumer_count=0 in its
            // admin snapshot.  Replaces a bare sleep_for(200ms) Class B
            // ordering pattern: a regression that delays deregister
            // processing would silently fail the EXPECT_EQ on a stale
            // count rather than failing here with a clear timeout
            // diagnostic.
            const auto consumer_count_for = [&](const std::string &name) -> int {
                ChannelSnapshot snap = broker.service_ref().query_channel_snapshot();
                for (const auto &ch : snap.channels)
                    if (ch.name == name) return ch.consumer_count;
                return -1;
            };
            const auto cdeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (consumer_count_for(ch_name) != 0 &&
                   std::chrono::steady_clock::now() < cdeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            EXPECT_EQ(consumer_count_for(ch_name), 0)
                << "broker did not reflect consumer_count=0 within 2s "
                   "after deregister_consumer";

            cons_bh.stop();
            prod_bh.stop();
            broker.stop_and_join();
        },
        "broker_health.consumer_auto_deregisters",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// ============================================================================
// producer_auto_deregisters
// ============================================================================

int producer_auto_deregisters(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint        = "tcp://127.0.0.1:0";
            cfg.use_curve       = true;
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
            cfg.ready_timeout_override   = std::chrono::milliseconds(15000);
            cfg.pending_timeout_override = std::chrono::milliseconds(15000);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name = make_test_channel_name("health.producer_dereg");

            // Register producer A
            {
                BrcHandle bh;
                bh.start(broker.endpoint, broker.pubkey, "prod.a." + ch_name);
                auto reg = bh.brc.register_channel(make_reg_opts(ch_name, "prod.a." + ch_name), 3000);
                ASSERT_TRUE(reg.has_value());

                // Deregister (assert status="success" explicitly).
                {
                    auto dereg = bh.brc.deregister_channel(ch_name);
                    ASSERT_TRUE(dereg.has_value());
                    EXPECT_EQ(dereg->value("status", std::string{}), "success");
                }
                bh.stop();

                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            // Producer B should register the same channel immediately
            {
                BrcHandle bh;
                bh.start(broker.endpoint, broker.pubkey, "prod.b." + ch_name);
                auto reg = bh.brc.register_channel(make_reg_opts(ch_name, "prod.b." + ch_name), 3000);
                EXPECT_TRUE(reg.has_value())
                    << "Producer B failed to register — DEREG_REQ was not processed";
                bh.stop();
            }

            broker.stop_and_join();
        },
        "broker_health.producer_auto_deregisters",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// ============================================================================
// dead_consumer_orchestrator
// ============================================================================

int dead_consumer_orchestrator(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: dead_consumer_orchestrator requires argv[2]: temp_file\n");
        return 1;
    }
    const std::string tmp_file = argv[2];

    return run_gtest_worker(
        [&tmp_file]() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
            cfg.ready_timeout_override           = std::chrono::milliseconds(15000);
            cfg.pending_timeout_override         = std::chrono::milliseconds(15000);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(1);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name  = make_test_channel_name("health.dead_consumer");
            const std::string prod_uid = "prod." + ch_name;

            std::atomic<bool> consumer_died{false};

            BrcHandle bh;
            bh.brc.on_notification([&](const std::string &type, const nlohmann::json &)
            {
                if (type == "CONSUMER_DIED_NOTIFY")
                    consumer_died.store(true);
            });
            bh.start(broker.endpoint, broker.pubkey, prod_uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Write connection info for the exiter
            {
                std::ofstream f(tmp_file);
                ASSERT_TRUE(f.is_open()) << "Failed to open temp file: " << tmp_file;
                f << broker.endpoint << "\n"
                  << broker.pubkey   << "\n"
                  << ch_name         << "\n";
            }

            signal_test_ready();

            // Wait for the exiter to connect and die
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));

            // Wait for CONSUMER_DIED_NOTIFY (broker detects dead PID)
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!consumer_died.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(consumer_died.load())
                << "CONSUMER_DIED_NOTIFY was not received within 5s after exiter died";

            bh.stop();
            broker.stop_and_join();
        },
        "broker_health.dead_consumer_orchestrator",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// ============================================================================
// dead_consumer_exiter
// ============================================================================

int dead_consumer_exiter(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: dead_consumer_exiter requires argv[2]: temp_file\n");
        return 1;
    }
    const std::string tmp_file = argv[2];

    return run_gtest_worker(
        [&tmp_file]() {
            std::string endpoint;
            std::string pubkey;
            std::string ch_name;
            {
                std::ifstream f(tmp_file);
                ASSERT_TRUE(f.is_open()) << "Exiter: cannot open temp file: " << tmp_file;
                std::getline(f, endpoint);
                std::getline(f, pubkey);
                std::getline(f, ch_name);
            }
            ASSERT_FALSE(endpoint.empty());
            ASSERT_FALSE(ch_name.empty());

            const std::string cons_uid = "cons.exiter." + ch_name;

            BrcHandle bh;
            bh.start(endpoint, pubkey, cons_uid);
            auto reg = bh.brc.register_consumer(make_cons_opts(ch_name, cons_uid), 5000);
            ASSERT_TRUE(reg.has_value()) << "Exiter: register_consumer failed";

            // Crash simulation: _exit(0) skips all destructors.
            // No CONSUMER_DEREG_REQ — broker must detect dead PID.
            _exit(0);
        },
        "broker_health.dead_consumer_exiter",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// ============================================================================
// schema_mismatch_notify
// ============================================================================

int schema_mismatch_notify(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            const std::string ch_name  = make_test_channel_name("health.schema_mismatch");
            const std::string uid_a    = "prod.a." + ch_name;
            const std::string uid_b    = "prod.b." + ch_name;
            const std::string hash_a   = std::string(64, 'a');
            const std::string hash_b   = std::string(64, 'b');

            std::atomic<bool> error_fired{false};

            // Producer A registers with schema_hash A
            BrcHandle bh_a;
            bh_a.brc.on_notification([&](const std::string &type, const nlohmann::json &)
            {
                if (type == "CHANNEL_ERROR_NOTIFY")
                    error_fired.store(true);
            });
            bh_a.start(broker.endpoint, broker.pubkey, uid_a);

            auto opts_a = make_reg_opts(ch_name, uid_a);
            opts_a["schema_hash"] = hash_a;
            auto reg_a = bh_a.brc.register_channel(opts_a, 3000);
            ASSERT_TRUE(reg_a.has_value());

            // Producer B tries same channel with different schema_hash → rejected
            BrcHandle bh_b;
            bh_b.start(broker.endpoint, broker.pubkey, uid_b);

            auto opts_b = make_reg_opts(ch_name, uid_b);
            opts_b["schema_hash"] = hash_b;
            auto reg_b = bh_b.brc.register_channel(opts_b, 3000);
            // Schema-hash conflict on duplicate registration → SCHEMA_MISMATCH
            // per HEP-CORE-0007 §12.4a.  Post-Stage-2 contract: broker
            // surfaces ERROR body via optional<json>; nullopt is reserved
            // for transport failure.
            ASSERT_TRUE(reg_b.has_value())
                << "Broker should respond with ERROR, not silent timeout";
            EXPECT_EQ(reg_b->value("status", std::string{}), "error");
            EXPECT_EQ(reg_b->value("error_code", std::string{}), "SCHEMA_MISMATCH");

            // Wait for CHANNEL_EVENT_NOTIFY to reach producer A
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!error_fired.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(error_fired.load())
                << "CHANNEL_EVENT_NOTIFY (schema mismatch) was not received within 3s";

            bh_b.stop();
            bh_a.stop();
            broker.stop_and_join();
        },
        "broker_health.schema_mismatch_notify",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// ============================================================================
// HEP-CORE-0039 P8 migration prerequisites — heartbeat-timeout sweep behavior
// pins that must hold before `check_heartbeat_timeouts` is migrated to
// `for_each_presence_matching`.  See `docs/todo/QUERY_LAYER_TODO.md` P8.
// ============================================================================

// Multi-producer partial pending-timeout: one producer stops heartbeating,
// the OTHER keeps heartbeating; that producer's presence times out and is
// removed but the channel SURVIVES (last-producer atomic teardown does NOT
// fire because there's still a healthy producer).  A migration that
// accidentally fires `_on_channel_closed` on any producer drop would tear
// the channel down here and the surviving producer would get a stray
// CHANNEL_CLOSING_NOTIFY.
int multi_producer_partial_pending_timeout(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
            cfg.ready_timeout_override           = std::chrono::milliseconds(500);
            cfg.pending_timeout_override         = std::chrono::milliseconds(500);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name =
                make_test_channel_name("health.multi_prod_partial_timeout");
            const std::string prod_a_uid = "prod.a." + ch_name;
            const std::string prod_b_uid = "prod.b." + ch_name;

            // Producer A — the survivor.  Keeps heartbeating.
            std::atomic<bool> a_got_closing{false};
            BrcHandle prod_a;
            prod_a.brc.on_notification(
                [&](const std::string &type, const nlohmann::json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY")
                        a_got_closing.store(true);
                });
            prod_a.start(broker.endpoint, broker.pubkey, prod_a_uid);
            auto reg_a = prod_a.brc.register_channel(
                make_reg_opts(ch_name, prod_a_uid), 3000);
            ASSERT_TRUE(reg_a.has_value()) << "register A failed";

            // Producer B — the one that times out.  Sends one heartbeat
            // then stops.
            BrcHandle prod_b;
            prod_b.start(broker.endpoint, broker.pubkey, prod_b_uid);
            auto reg_b = prod_b.brc.register_channel(
                make_reg_opts(ch_name, prod_b_uid), 3000);
            ASSERT_TRUE(reg_b.has_value()) << "register B failed";

            prod_a.brc.send_heartbeat(ch_name, prod_a_uid, "producer", {});
            prod_b.brc.send_heartbeat(ch_name, prod_b_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Survivor heartbeats every 100ms; B never again.
            std::atomic<bool> a_stop{false};
            std::thread a_thread([&] {
                while (!a_stop.load()) {
                    prod_a.brc.send_heartbeat(ch_name, prod_a_uid,
                                               "producer", {});
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(100));
                }
            });

            // Wait for B to time out: ready_timeout + pending_timeout
            // ≈ 1000ms + sweep slop.
            const auto producer_count_for =
                [&](const std::string &name) -> int {
                ChannelSnapshot snap =
                    broker.service_ref().query_channel_snapshot();
                for (const auto &ch : snap.channels)
                    if (ch.name == name)
                        return static_cast<int>(ch.producer_uids.size());
                return -1;
            };

            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (producer_count_for(ch_name) != 1 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
            }

            EXPECT_EQ(producer_count_for(ch_name), 1)
                << "B's presence must be removed; survivor A remains";
            EXPECT_FALSE(a_got_closing.load())
                << "Channel must SURVIVE — surviving producer A must NOT "
                   "receive CHANNEL_CLOSING_NOTIFY when B times out";

            a_stop.store(true);
            a_thread.join();
            prod_a.stop();
            prod_b.stop();
            broker.stop_and_join();
        },
        "broker_health.multi_producer_partial_pending_timeout",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// Consumer heartbeat-timeout fires CONSUMER_DIED_NOTIFY with
// reason="heartbeat_timeout" (NOT "process_dead").  Distinguishes from the
// PID-death path in `dead_consumer_orchestrator` above.  Pins the exact
// body shape required by HEP-CORE-0023 §2.1.1 + the broker_proto 4→5
// audit (role_uid, consumer_pid, consumer_hostname, reason).  A migration
// that calls the wrong fan-out reason or omits a field would fail here.
int consumer_heartbeat_timeout_notify(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
            cfg.ready_timeout_override           = std::chrono::milliseconds(500);
            cfg.pending_timeout_override         = std::chrono::milliseconds(500);
            // Disable PID liveness so the only path to CONSUMER_DIED_NOTIFY
            // is the heartbeat-timeout path under test.
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name =
                make_test_channel_name("health.cons_hb_timeout");
            const std::string prod_uid = "prod." + ch_name;
            const std::string cons_uid = "cons." + ch_name;

            std::atomic<bool>     died_fired{false};
            std::mutex            notify_mu;
            nlohmann::json        died_body;

            // Producer keeps heartbeating; receives CONSUMER_DIED_NOTIFY
            // when the consumer's heartbeat-timeout fires.
            BrcHandle prod;
            prod.brc.on_notification(
                [&](const std::string &type, const nlohmann::json &body) {
                    if (type == "CONSUMER_DIED_NOTIFY") {
                        std::lock_guard<std::mutex> lk(notify_mu);
                        died_body = body;
                        died_fired.store(true);
                    }
                });
            prod.start(broker.endpoint, broker.pubkey, prod_uid);
            auto reg = prod.brc.register_channel(
                make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());
            prod.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Consumer: register, heartbeat once, then go silent.
            BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid);
            auto creg = cons.brc.register_consumer(
                make_cons_opts(ch_name, cons_uid), 3000);
            ASSERT_TRUE(creg.has_value());
            cons.brc.send_heartbeat(ch_name, cons_uid, "consumer", {});

            // Producer keeps heartbeating in a side thread.
            std::atomic<bool> prod_stop{false};
            std::thread prod_thread([&] {
                while (!prod_stop.load()) {
                    prod.brc.send_heartbeat(ch_name, prod_uid,
                                             "producer", {});
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(100));
                }
            });

            // Wait for NOTIFY (ready + pending ≈ 1000ms + sweep slop).
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (!died_fired.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
            }

            ASSERT_TRUE(died_fired.load())
                << "CONSUMER_DIED_NOTIFY (heartbeat_timeout) not received "
                   "within 5s";

            // Pin body shape — exact fields, exact reason value.
            std::lock_guard<std::mutex> lk(notify_mu);
            EXPECT_EQ(died_body.value("channel_name", std::string{}),
                      ch_name);
            EXPECT_EQ(died_body.value("role_uid", std::string{}), cons_uid);
            EXPECT_EQ(died_body.value("reason", std::string{}),
                      "heartbeat_timeout")
                << "reason MUST be \"heartbeat_timeout\" — distinguishes "
                   "from the PID-death path (\"process_dead\")";
            EXPECT_TRUE(died_body.contains("consumer_pid"))
                << "consumer_pid field present per broker_proto 5";
            EXPECT_TRUE(died_body.contains("consumer_hostname"))
                << "consumer_hostname field present";

            prod_stop.store(true);
            prod_thread.join();
            cons.stop();
            prod.stop();
            broker.stop_and_join();
        },
        "broker_health.consumer_heartbeat_timeout_notify",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// Two-snapshot invariant: a presence that demotes Connected→Pending in
// tick T MUST NOT also be terminated (Pending→Disconnected) in the same
// tick T.  Pin the timing: with ready_timeout==pending_timeout==500ms
// and sweep ~100ms, CHANNEL_CLOSING_NOTIFY must NOT fire before
// (ready_timeout + safety margin) — only after (ready_timeout +
// pending_timeout) does it fire.  A migration that collapses Pass-1 and
// Pass-2 into one snapshot would terminate the just-demoted presence in
// the same tick and NOTIFY would fire ~500ms too early.
int two_snapshot_invariant(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
            cfg.ready_timeout_override           = std::chrono::milliseconds(500);
            cfg.pending_timeout_override         = std::chrono::milliseconds(500);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name =
                make_test_channel_name("health.two_snapshot");
            const std::string prod_uid = "prod." + ch_name;

            std::atomic<bool>     closing_fired{false};
            std::chrono::steady_clock::time_point closing_at{};
            std::mutex            t_mu;

            BrcHandle prod;
            prod.brc.on_notification(
                [&](const std::string &type, const nlohmann::json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY") {
                        std::lock_guard<std::mutex> lk(t_mu);
                        closing_at = std::chrono::steady_clock::now();
                        closing_fired.store(true);
                    }
                });
            prod.start(broker.endpoint, broker.pubkey, prod_uid);
            auto reg = prod.brc.register_channel(
                make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            // One heartbeat, then go silent.  Record T0 = the moment
            // AFTER the heartbeat completes — that's the broker's
            // last_heartbeat for this presence.
            prod.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            const auto t0 = std::chrono::steady_clock::now();

            // Lower bound: NOTIFY must NOT fire before
            // (ready_timeout + pending_timeout) ≈ 1000ms.
            // We check at t0 + 700ms (well inside the legitimate
            // Pending phase) — closing MUST still be false.
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            EXPECT_FALSE(closing_fired.load())
                << "CHANNEL_CLOSING_NOTIFY fired too early — Pass-2 "
                   "termination must NOT happen in the same sweep tick "
                   "as the Pass-1 Connected→Pending demotion.  This is "
                   "the two-snapshot invariant; a single-snapshot "
                   "migration would fail here.";

            // Upper bound: NOTIFY must fire by t0 + 2000ms.
            const auto deadline = t0 + std::chrono::seconds(3);
            while (!closing_fired.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
            }
            ASSERT_TRUE(closing_fired.load())
                << "CHANNEL_CLOSING_NOTIFY did not fire within 3s";

            // Pin the exact lower bound on the firing time: at least
            // (ready_timeout + pending_timeout) - small_slack must
            // have elapsed from t0.  We use 800ms (less than 1000ms
            // to tolerate sweep-grain timing, but well above the
            // 500ms a collapsed-pass migration would produce).
            std::chrono::milliseconds elapsed_at_close{};
            {
                std::lock_guard<std::mutex> lk(t_mu);
                elapsed_at_close =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        closing_at - t0);
            }
            EXPECT_GE(elapsed_at_close, std::chrono::milliseconds(800))
                << "NOTIFY fired at " << elapsed_at_close.count()
                << "ms after last heartbeat — expected ≥800ms "
                   "(ready_timeout + pending_timeout - sweep slop). "
                   "A single-snapshot migration would produce ≈500-600ms.";

            prod.stop();
            broker.stop_and_join();
        },
        "broker_health.two_snapshot_invariant",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// `channel_torn_down` short-circuit (HEP-0039 P8 Step B prerequisite).
// Producer + Consumer both registered on ONE channel; both go silent at
// the same time.  Both presences enter Pending in the same sweep tick;
// in the NEXT sweep tick (after pending_timeout) the producer Pass-2
// fires the last-producer atomic teardown — channel evicted +
// CHANNEL_CLOSING_NOTIFY fanned out.  The `channel_torn_down`
// short-circuit at `broker_service.cpp:3012` then SKIPS the consumer
// Pass-2 iteration for this channel — no stray CONSUMER_DIED_NOTIFY
// fires (the channel and the consumer are both gone via the
// `_on_channel_closed` cascade).
//
// A migration that loses the short-circuit would call Pass-2 consumer
// against a vanished channel.  The mutator's idempotency would
// (probably) gracefully no-op, but the broker-side `CONSUMER_DIED_NOTIFY`
// fan-out (which iterates `pre_drop_channel.producers` from the
// snapshot, NOT live state) would silently emit the NOTIFY to the
// producer's BRC ROUTER identity — which the producer's BRC observes
// as an extra unexpected message after CHANNEL_CLOSING_NOTIFY.  This
// test pins "no CONSUMER_DIED_NOTIFY ever arrives" — bug catch.
int channel_torn_down_consumer_pass2_skipped(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
            cfg.ready_timeout_override           = std::chrono::milliseconds(500);
            cfg.pending_timeout_override         = std::chrono::milliseconds(500);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name =
                make_test_channel_name("health.channel_torn_down");
            const std::string prod_uid = "prod." + ch_name;
            const std::string cons_uid = "cons." + ch_name;

            std::atomic<int> closing_count{0};
            std::atomic<int> consumer_died_count{0};

            // Producer's BRC subscribes to both notification types.
            // CHANNEL_CLOSING_NOTIFY should fire (channel evicted by
            // producer's own last-producer teardown).
            // CONSUMER_DIED_NOTIFY should NEVER fire (consumer Pass-2
            // skipped by channel_torn_down).
            BrcHandle prod;
            prod.brc.on_notification(
                [&](const std::string &type, const nlohmann::json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY")
                        closing_count.fetch_add(1);
                    if (type == "CONSUMER_DIED_NOTIFY")
                        consumer_died_count.fetch_add(1);
                });
            prod.start(broker.endpoint, broker.pubkey, prod_uid);
            auto reg = prod.brc.register_channel(
                make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            // Register consumer too.
            BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid);
            auto creg = cons.brc.register_consumer(
                make_cons_opts(ch_name, cons_uid), 3000);
            ASSERT_TRUE(creg.has_value());

            // Both heartbeat once, then go silent.  Same start moment
            // so both enter Pending in the same sweep tick and both
            // time out in the same Pass-2 tick.
            prod.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            cons.brc.send_heartbeat(ch_name, cons_uid, "consumer", {});

            // Wait for the eviction + NOTIFY cascade.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (closing_count.load() == 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
            }

            ASSERT_GE(closing_count.load(), 1)
                << "CHANNEL_CLOSING_NOTIFY did not fire after the "
                   "last-producer atomic teardown";

            // Hold steady for an additional pending_timeout window
            // to give a buggy migration the chance to fire a stray
            // CONSUMER_DIED_NOTIFY in a later sweep iteration.
            std::this_thread::sleep_for(std::chrono::milliseconds(700));

            EXPECT_EQ(consumer_died_count.load(), 0)
                << "CONSUMER_DIED_NOTIFY must NOT fire — the consumer "
                   "Pass-2 iteration was supposed to be skipped via "
                   "the `channel_torn_down` short-circuit because the "
                   "producer Pass-2 already evicted the channel in the "
                   "same sweep tick.  A migration that drops this "
                   "short-circuit would fail this assertion.";

            cons.stop();
            prod.stop();
            broker.stop_and_join();
        },
        "broker_health.channel_torn_down_consumer_pass2_skipped",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

// ============================================================================
// ctrl_zap_deny_path — D2 default-deny security gate
// ============================================================================
//
// PURPOSE: Pin that the broker's CTRL ROUTER ZAP gate (HEP-CORE-0035
//   §4.2 + §4.8 + PeerAdmission Phase D step D2) actually fires DENY
//   when a CURVE peer's pubkey is NOT in the operator-defined
//   allowlist.  Without this, the deny-by-default contract has zero
//   path-level coverage (audit finding B2).
//
// BYPASS: Constructs `BrokerService` directly with
//   `enforce_ctrl_admission = true` + empty `known_roles` + empty
//   `peers[]`.  Bypasses HubHost (which would derive use_curve from
//   the auth keyfile path), letting the test exercise the production
//   gate without standing up a vault on disk.
//
// WHY: HubHost requires a valid vault path + password to enable
//   CURVE, which is overkill for an L3 ZAP-fired pin and would
//   couple this test to the vault subsystem's lifecycle.  Direct
//   `BrokerService::Config` construction is the documented L3
//   admission-testing pattern (see `tests/test_layer2_service/
//   workers/zap_router_workers.cpp` for the Phase C analogue).
//
// CANONICAL STORAGE: The gate's allowlist storage is
//   `BrokerCtrlAdmission::current_` (`broker_service.cpp:226`).
//   Observability is via `sec::ZapRouter::instance().denied_count()`
//   — a singleton counter incremented by `ZapRouter::pump_one` on
//   every DENY reply.
//
// RE-EXAMINE WHEN: AUTH_TODO D3 / D4 land + the allow-path L3 test
//   exists alongside this one (this test currently has no allow-path
//   sibling because BRC's CURVE keypair plumbing is the same gap
//   tracked in AUTH_TODO under "Phase D close-out follow-on" —
//   the deny path doesn't need BRC's keys to fail).

int ctrl_zap_deny_path(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            namespace sec = pylabhub::utils::security;

            // Capture pre-test denied counter (process-singleton).
            // SpawnWorker runs each test in its own subprocess so the
            // counter starts at 0 for this scenario, but reading
            // before-state is the more robust pattern.
            const auto before_denied = sec::ZapRouter::instance().denied_count();

            // Generate an explicit CURVE keypair for the test client.
            // The test owns the client keypair so it can verify the
            // CORRECT pubkey was denied — and that pubkey is
            // deliberately NOT inserted into `known_roles[]`.
            std::array<char, 41> client_pub{};
            std::array<char, 41> client_sec{};
            ASSERT_EQ(zmq_curve_keypair(client_pub.data(), client_sec.data()), 0);
            const std::string client_pub_z85(client_pub.data(), 40);
            const std::string client_sec_z85(client_sec.data(), 40);

            // Spin up broker with the gate ENFORCED + empty allowlist.
            using ReadyInfo = std::pair<std::string, std::string>;
            auto rp = std::make_shared<std::promise<ReadyInfo>>();
            auto rf = rp->get_future();
            BrokerService::Config bcfg;
            bcfg.endpoint              = "tcp://127.0.0.1:0";
            bcfg.use_curve             = true;
            bcfg.enforce_ctrl_admission = true;   // production deny-all
            // known_roles + peers are both empty by default → empty
            // PeerAllowlist → every CURVE handshake is DENY.
            bcfg.on_ready = [rp](const std::string &ep, const std::string &pk)
            { rp->set_value({ep, pk}); };
            auto state = std::make_unique<pylabhub::hub::HubState>();
            auto svc   = std::make_unique<BrokerService>(std::move(bcfg), *state);
            auto *raw  = svc.get();
            std::thread t([raw] { raw->run(); });
            auto info = rf.get();

            // BRC connect with our keypair → CURVE handshake on first
            // send → broker ZAP gate sees our pubkey (NOT in
            // allowlist) → DENY → handshake never completes → REG_REQ
            // times out.  This is the production deny path.
            BrokerRequestComm brc;
            BrokerRequestComm::Config ccfg;
            ccfg.broker_endpoint = info.first;
            ccfg.broker_pubkey   = info.second;
            ccfg.client_pubkey   = client_pub_z85;
            ccfg.client_seckey   = client_sec_z85;
            ccfg.role_uid        = "prod.deny.test.uid00000001";
            ccfg.role_name       = "deny_role";
            EXPECT_TRUE(brc.connect(ccfg))
                << "BRC TCP connect should succeed even when ZAP denies; "
                   "the CURVE handshake fails on first send, not on connect.";

            // Attempt REG_REQ — handshake fires here and ZAP denies.
            // Short timeout (1500ms): tight enough to keep CI fast,
            // wide enough over libzmq's default ZAP_REPLY_TIMEOUT to
            // avoid flake.
            const auto reg_result = brc.register_channel(
                make_reg_opts("ch.deny.path", "prod.deny.test.uid00000001"),
                /*timeout_ms=*/1500);
            EXPECT_FALSE(reg_result.has_value())
                << "register_channel succeeded under deny-all allowlist — "
                   "the ZAP gate did NOT fire.";

            // PATH PIN: the denial fired through ZapRouter.  This is
            // what distinguishes a real ZAP deny from a happens-to-
            // time-out failure on some other path.
            const auto after_denied = sec::ZapRouter::instance().denied_count();
            EXPECT_GT(after_denied, before_denied)
                << "ZapRouter::denied_count() did not increase: "
                << before_denied << " -> " << after_denied
                << " — the deny did not come from the ZAP gate.";

            brc.disconnect();
            raw->stop();
            if (t.joinable()) t.join();
        },
        "broker_health.ctrl_zap_deny_path",
        logger_module(), file_lock_module(), json_module(),
        crypto_module(), zmq_module());
}

} // namespace pylabhub::tests::worker::broker_health

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct BrokerHealthWorkerRegistrar
{
    BrokerHealthWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto       dot  = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_health")
                {
                    return -1;
                }
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_health;
                if (scenario == "producer_gets_closing_notify")
                    return producer_gets_closing_notify(argc, argv);
                if (scenario == "consumer_auto_deregisters")
                    return consumer_auto_deregisters(argc, argv);
                if (scenario == "producer_auto_deregisters")
                    return producer_auto_deregisters(argc, argv);
                if (scenario == "dead_consumer_orchestrator")
                    return dead_consumer_orchestrator(argc, argv);
                if (scenario == "dead_consumer_exiter")
                    return dead_consumer_exiter(argc, argv);
                if (scenario == "schema_mismatch_notify")
                    return schema_mismatch_notify(argc, argv);
                if (scenario == "multi_producer_partial_pending_timeout")
                    return multi_producer_partial_pending_timeout(argc, argv);
                if (scenario == "consumer_heartbeat_timeout_notify")
                    return consumer_heartbeat_timeout_notify(argc, argv);
                if (scenario == "two_snapshot_invariant")
                    return two_snapshot_invariant(argc, argv);
                if (scenario == "channel_torn_down_consumer_pass2_skipped")
                    return channel_torn_down_consumer_pass2_skipped(argc, argv);
                if (scenario == "ctrl_zap_deny_path")
                    return ctrl_zap_deny_path(argc, argv);
                fmt::print(stderr, "ERROR: Unknown broker_health scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerHealthWorkerRegistrar g_broker_health_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
