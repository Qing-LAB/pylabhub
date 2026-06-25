/**
 * @file datahub_broker_protocol_workers.cpp
 * @brief Worker bodies for broker control-plane protocol tests
 *        (CHECKSUM_ERROR, CHANNEL_CLOSING_NOTIFY, REG_REQ duplicate /
 *         schema mismatch, HEARTBEAT_REQ, ROLE_PRESENCE_REQ,
 *         ROLE_INFO_REQ, transport arbitration, REG_ACK heartbeat
 *         negotiation, CHANNEL_BROADCAST_REQ fan-out).
 *
 * Migrated 2026-05-14 from `tests/test_layer3_datahub/test_datahub_broker_protocol.cpp`
 * where it used the in-process `SetUpTestSuite`-owned `LifecycleGuard`
 * antipattern.  Each worker function is a 1:1 translation of the
 * originally-named TEST_F body; the helper shape (`HubHostHandle`,
 * `BrcHandle`, `EventCollector`, `make_reg_opts`, `make_cons_opts`,
 * `pid_chan`) was lifted verbatim from the parent's anonymous
 * namespace so the bodies' semantics are unchanged.
 *
 * The bodies that previously called `broker_.reset()` mid-test to
 * swap in a custom `BrokerService::Config` keep that idiom — the
 * `run_with_host` template hands the body an
 * `std::optional<HubHostHandle> &` so the body may reset/emplace as
 * the original suite did.
 */

#include "datahub_broker_protocol_workers.h"

#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_sync_utils.h"
#include "wire_conformance.h"  // Audit TR1 — HEP-spec wire-key helpers
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/scope_guard.hpp"
#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <cppzmq/zmq_addon.hpp>   // zmq::send_multipart (R3.6)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// Forward declaration — `raw_req` is defined in
// `datahub_broker_workers.cpp` (same `test_layer3_datahub` binary).
// Used by `wire_conformance_band_corr_id_echo` to send BAND_*_REQ
// messages with explicit `correlation_id` payloads.  BRC's
// `do_request` matches replies by message-type today (NOT by
// correlation_id), so a BRC-based test cannot exercise the
// echo path — we need raw-ZMQ to inject corr_id and observe the
// broker's echo.  Closes M1 finding from the 2026-05-20 review
// of commit `d759424` ("Review A1+B1: ... broker corr_id
// thread-through").
namespace pylabhub::tests::worker::broker
{
nlohmann::json raw_req(const std::string& endpoint,
                       const std::string& msg_type,
                       const nlohmann::json& payload,
                       int timeout_ms = 2000,
                       const std::string& server_pubkey = "");
}

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::poll_until;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pylabhub::tests::worker
{
namespace broker_protocol
{

namespace
{

// ─── Real-HubHost RAII wrapper ────────────────────────────────────────────────

struct HubHostHandle
{
    fs::path                 hub_dir;
    std::unique_ptr<HubHost> host;
    std::string              endpoint;
    std::string              pubkey;

    HubHostHandle() = default;
    HubHostHandle(HubHostHandle &&) noexcept = default;
    HubHostHandle &operator=(HubHostHandle &&) noexcept = default;
    ~HubHostHandle()
    {
        if (host)
            host->shutdown();
        host.reset();
        if (!hub_dir.empty())
        {
            std::error_code ec;
            fs::remove_all(hub_dir, ec);
        }
    }
};

HubHostHandle start_local_broker(BrokerService::Config legacy_cfg = {})
{
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_proto_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    pylabhub::utils::HubDirectory::init_directory(dir, "ProtoTestHub");

    const fs::path hub_json = dir / "hub.json";
    json j;
    {
        std::ifstream f(hub_json);
        if (f.is_open())
            j = json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";

    // Translate the legacy BrokerService::Config fields callers
    // customise into hub.json overrides so the original suite's
    // pattern of "spin up a broker with these specific heartbeat /
    // policy values" continues to work.
    if (legacy_cfg.heartbeat_interval.count() > 0)
        j["broker"]["heartbeat_interval_ms"] =
            static_cast<int>(legacy_cfg.heartbeat_interval.count());
    if (legacy_cfg.ready_miss_heartbeats > 0)
        j["broker"]["ready_miss_heartbeats"] = legacy_cfg.ready_miss_heartbeats;
    if (legacy_cfg.pending_miss_heartbeats > 0)
        j["broker"]["pending_miss_heartbeats"] = legacy_cfg.pending_miss_heartbeats;
    if (legacy_cfg.ready_timeout_override.has_value())
        j["broker"]["ready_timeout_ms"] =
            static_cast<int>(legacy_cfg.ready_timeout_override->count());
    if (legacy_cfg.pending_timeout_override.has_value())
        j["broker"]["pending_timeout_ms"] =
            static_cast<int>(legacy_cfg.pending_timeout_override->count());
    if (legacy_cfg.checksum_repair_policy ==
        ChecksumRepairPolicy::NotifyOnly)
        j["broker"]["checksum_repair_policy"] = "notify_only";
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
    fs::create_directories(dir / "schemas");

    HubHostHandle h;
    h.hub_dir = std::move(dir);
    h.host    = std::make_unique<HubHost>(
        HubConfig::load_from_directory(h.hub_dir.string()));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = h.host->broker_pubkey();
    return h;
}

// ─── BrokerRequestComm handle + poll loop thread ─────────────────────────────

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
        thread = std::thread([this] {
            brc.run_poll_loop([this] { return running.load(); });
        });
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

// ─── Channel-name + REG opts builders ─────────────────────────────────────────

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    json opts;
    opts["channel_name"]  = channel;
    opts["role_uid"]  = consumer_uid;
    opts["role_name"] = "test_consumer";
    opts["consumer_pid"]  = ::getpid();
    return opts;
}

// ─── Thread-safe event collector ──────────────────────────────────────────────

struct EventCollector
{
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, json>> events;

    void push(const std::string &type, const json &body)
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            events.emplace_back(type, body);
        }
        cv.notify_all();
    }

    bool wait_for(size_t count, int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return events.size() >= count; });
    }

    size_t size()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return events.size();
    }
};

// ─── Worker harness ───────────────────────────────────────────────────────────
//
// `body(broker, log_cap)` receives:
//   - `broker`: a freshly-started HubHostHandle.  The body may .reset()
//     and .emplace(start_local_broker(custom_cfg)) to swap to a
//     custom-config broker — same idiom the original suite used via
//     `broker_.reset() / broker_.emplace(...)`.
//   - `log_cap`: LogCaptureFixture for tests that need `log_path()`
//     (Heartbeat wire-payload echo) or ad-hoc ExpectLogWarn calls.

template <typename Body>
int run_with_host(std::string_view worker_name, Body &&body,
                  std::vector<std::string> warns  = {},
                  std::vector<std::string> errors = {})
{
    return run_gtest_worker(
        [body = std::forward<Body>(body),
         warns = std::move(warns),
         errors = std::move(errors)]() mutable {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : warns)
                log_cap.ExpectLogWarn(w);
            for (auto &e : errors)
                log_cap.ExpectLogError(e);

            std::optional<HubHostHandle> broker;
            broker.emplace(start_local_broker());

            body(broker, log_cap);

            broker.reset();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

// ============================================================================
// 1. CHECKSUM_ERROR_REPORT — broker forwards as CHANNEL_EVENT_NOTIFY
// ============================================================================

int checksum_error_report_forwarded_to_producer()
{
    return run_with_host(
        "broker_protocol::checksum_error_report_forwarded_to_producer",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            broker.reset();
            BrokerService::Config cfg;
            cfg.endpoint               = "tcp://127.0.0.1:0";
            cfg.schema_search_dirs     = {};
            cfg.checksum_repair_policy = ChecksumRepairPolicy::NotifyOnly;
            broker.emplace(start_local_broker(std::move(cfg)));

            const std::string channel = pid_chan("proto.checksum.prod");
            const std::string uid     = "prod." + channel;

            auto prod_events = std::make_shared<EventCollector>();
            BrcHandle prod_bh;
            prod_bh.brc.on_notification(
                [prod_events](const std::string &type, const json &body) {
                    if (type == "CHANNEL_EVENT_NOTIFY")
                        prod_events->push(type, body);
                });
            prod_bh.start(broker->endpoint, broker->pubkey, uid);

            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle reporter;
            reporter.start(broker->endpoint, broker->pubkey,
                           "REPORT-" + channel);

            json report;
            report["channel_name"] = channel;
            report["slot_index"]   = 42;
            report["error"]        = "bad CRC in slot 42";
            report["reporter_pid"] = ::getpid();
            reporter.brc.send_checksum_error(report);

            ASSERT_TRUE(prod_events->wait_for(1, 3000))
                << "Producer did not receive checksum error notify";

            reporter.stop();
            prod_bh.stop();
        },
        {"Cat2 checksum error"});
}

int checksum_error_report_unknown_channel_silent()
{
    return run_with_host(
        "broker_protocol::checksum_error_report_unknown_channel_silent",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            BrcHandle reporter;
            reporter.start(broker->endpoint, broker->pubkey, "REPORT-bogus");

            json report;
            report["channel_name"] = pid_chan("proto.checksum.bogus");
            report["slot_index"]   = 0;
            report["error"]        = "test";
            report["reporter_pid"] = ::getpid();

            EXPECT_NO_THROW(reporter.brc.send_checksum_error(report));

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            EXPECT_NO_THROW({
                auto snap = broker->host->broker().query_channel_snapshot();
                (void)snap;
            }) << "broker must remain operational after "
                  "unknown-channel checksum report";

            reporter.stop();
        },
        {"Cat2 checksum error"});
}

// ============================================================================
// 2. CHANNEL_CLOSING_NOTIFY — delivery to ALL registered members
// ============================================================================

int closing_notify_delivered_to_producer_and_consumer()
{
    return run_with_host(
        "broker_protocol::closing_notify_delivered_to_producer_and_consumer",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            // Preserve the original suite's broker.reset()+emplace
            // pattern even though the legacy config is effectively a
            // no-op (default endpoint, empty schema search dirs).
            broker.reset();
            BrokerService::Config cfg;
            cfg.endpoint           = "tcp://127.0.0.1:0";
            cfg.schema_search_dirs = {};
            broker.emplace(start_local_broker(std::move(cfg)));

            const std::string channel  = pid_chan("proto.close.all");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            std::atomic<int> prod_closing{0}, cons_closing{0};

            BrcHandle prod_bh;
            prod_bh.brc.on_notification(
                [&](const std::string &type, const json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY")
                        prod_closing.fetch_add(1);
                });
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);

            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.brc.on_notification(
                [&](const std::string &type, const json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY")
                        cons_closing.fetch_add(1);
                });
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);

            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value());

            broker->host->broker().request_close_channel(channel);

            EXPECT_TRUE(poll_until(
                [&] {
                    return prod_closing.load() > 0 && cons_closing.load() > 0;
                },
                std::chrono::seconds(5)))
                << "CHANNEL_CLOSING_NOTIFY not delivered to both members "
                   "within 5s";

            EXPECT_GE(prod_closing.load(), 1)
                << "Producer did not receive CHANNEL_CLOSING_NOTIFY";
            EXPECT_GE(cons_closing.load(), 1)
                << "Consumer did not receive CHANNEL_CLOSING_NOTIFY";

            cons_bh.stop();
            prod_bh.stop();
        });
}

// ============================================================================
// 3. Duplicate REG_REQ — SHM cardinality + schema hash conflict
// ============================================================================

int duplicate_reg_shm_cardinality()
{
    return run_with_host(
        "broker_protocol::duplicate_reg_shm_cardinality",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.dup.same");
            const std::string hash_hex = std::string(64, 'a');
            const std::string uid1 = "prod.proto.dup.same.uid00000001";
            const std::string uid2 = "prod.proto.dup.same.uid00000002";

            BrcHandle bh1;
            bh1.start(broker->endpoint, broker->pubkey, uid1);
            auto opts1 = make_reg_opts(channel, uid1);
            opts1["schema_hash"] = hash_hex;
            auto h1 = bh1.brc.register_channel(opts1, 3000);
            ASSERT_TRUE(h1.has_value());

            BrcHandle bh2;
            bh2.start(broker->endpoint, broker->pubkey, uid2);
            auto opts2 = make_reg_opts(channel, uid2);
            opts2["schema_hash"] = hash_hex;
            auto h2 = bh2.brc.register_channel(opts2, 3000);
            ASSERT_TRUE(h2.has_value())
                << "Broker must return a structured error envelope, "
                   "not transport failure";
            EXPECT_EQ(h2->value("status", std::string{}), "error")
                << "Second SHM producer must reject; got: " << h2->dump();
            EXPECT_EQ(h2->value("error_code", std::string{}),
                      "MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM")
                << "SHM cardinality reject must surface as "
                   "MULTI_PRODUCER_NOT_SUPPORTED_FOR_SHM; got: "
                << h2->dump();

            bh2.stop();
            bh1.stop();
        },
        {"SHM channels are physically single-producer"});
}

int duplicate_reg_different_schema_hash()
{
    return run_with_host(
        "broker_protocol::duplicate_reg_different_schema_hash",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel = pid_chan("proto.dup.diff");
            const std::string hash_a  = std::string(64, 'a');
            const std::string hash_b  = std::string(64, 'b');
            const std::string uid1 = "prod.proto.dup.diff.uid00000001";
            const std::string uid2 = "prod.proto.dup.diff.uid00000002";

            BrcHandle bh1;
            bh1.start(broker->endpoint, broker->pubkey, uid1);
            auto opts1 = make_reg_opts(channel, uid1);
            opts1["schema_hash"] = hash_a;
            auto h1 = bh1.brc.register_channel(opts1, 3000);
            ASSERT_TRUE(h1.has_value());

            BrcHandle bh2;
            bh2.start(broker->endpoint, broker->pubkey, uid2);
            auto opts2 = make_reg_opts(channel, uid2);
            opts2["schema_hash"] = hash_b;
            auto h2 = bh2.brc.register_channel(opts2, 3000);
            ASSERT_TRUE(h2.has_value())
                << "Broker should respond with ERROR, not silent timeout";
            EXPECT_EQ(h2->value("status", std::string{}), "error");
            EXPECT_EQ(h2->value("error_code", std::string{}),
                      "SCHEMA_MISMATCH");

            bh2.stop();
            bh1.stop();
        },
        /*warns=*/{},
        /*errors=*/{"Cat1 schema mismatch", "CHANNEL_ERROR_NOTIFY"});
}

// ============================================================================
// 4. HEARTBEAT_REQ — PendingReady → Ready + wire payload + keying
// ============================================================================

int heartbeat_transitions_to_ready()
{
    return run_with_host(
        "broker_protocol::heartbeat_transitions_to_ready",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel = pid_chan("proto.heartbeat.ready");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            // Pre-heartbeat: registering (producer registered, no
            // heartbeat yet).
            ChannelSnapshot snap = broker->host->broker().query_channel_snapshot();
            for (const auto &ch : snap.channels)
            {
                if (ch.name == channel)
                {
                    EXPECT_EQ(ch.observable, "registering");
                }
            }

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            auto channel_is_live = [&] {
                auto s = broker->host->broker().query_channel_snapshot();
                for (const auto &ch : s.channels)
                {
                    if (ch.name == channel)
                        return ch.observable == "live";
                }
                return false;
            };
            EXPECT_TRUE(poll_until(channel_is_live, std::chrono::seconds(3)))
                << "channel did not transition to live within 3s "
                   "after heartbeat";

            bh.stop();
        });
}

int heartbeat_wire_payload_includes_uid_and_role_type()
{
    return run_with_host(
        "broker_protocol::heartbeat_wire_payload_includes_uid_and_role_type",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &log_cap) {
            const auto prev_level =
                pylabhub::utils::Logger::instance().level();
            pylabhub::utils::Logger::instance().set_level(
                pylabhub::utils::Logger::Level::L_DEBUG);
            auto restore_level = pylabhub::basics::make_scope_guard([&] {
                pylabhub::utils::Logger::instance().set_level(prev_level);
            });

            const std::string channel   =
                pid_chan("proto.heartbeat.wire.uid");
            const std::string uid       = "prod." + channel;
            const std::string role_type = "producer";

            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value()) << "REG_REQ should succeed";

            bh.brc.send_heartbeat(channel, uid, role_type, {});

            // broker_proto 4→5 (R3.5b): HEARTBEAT_REQ wire key renamed
            // `uid` → `role_uid`.  The broker's HEARTBEAT_REQ log line
            // mirrors the wire field name.
            const std::string expected =
                "Broker: HEARTBEAT_REQ channel='" + channel +
                "' role_uid='" + uid +
                "' role_type='" + role_type + "'";

            auto read_log = [&]() {
                std::ifstream f(log_cap.log_path());
                return std::string(std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>{});
            };

            EXPECT_TRUE(poll_until(
                [&] {
                    return read_log().find(expected) != std::string::npos;
                },
                std::chrono::seconds(2)))
                << "broker did not log the expected wire-payload fields "
                   "within 2s.\nExpected substring: " << expected
                << "\nActual log tail:\n"
                << ([&]() {
                       auto s = read_log();
                       return s.size() > 2000
                                  ? s.substr(s.size() - 2000)
                                  : s;
                   })();

            bh.stop();
        });
}

int heartbeat_keying_producer_vs_consumer_distinct_rows()
{
    return run_with_host(
        "broker_protocol::heartbeat_keying_producer_vs_consumer_distinct_rows",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.heartbeat.keying");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_h, cons_h;
            prod_h.start(broker->endpoint, broker->pubkey, prod_uid);
            cons_h.start(broker->endpoint, broker->pubkey, cons_uid);

            auto reg = prod_h.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "REG_REQ should succeed";

            prod_h.brc.send_heartbeat(channel, prod_uid, "producer", {});

            auto disc = cons_h.brc.discover_channel(
                channel, nlohmann::json::object(), 3000);
            ASSERT_TRUE(disc.has_value()) << "DISC_REQ should resolve";
            auto cons_reg = cons_h.brc.register_consumer(
                make_cons_opts(channel, cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value())
                << "CONSUMER_REG_REQ should succeed";

            nlohmann::json prod_metrics = {{"out_written", 100}, {"drops", 0}};
            nlohmann::json cons_metrics = {{"in_received", 100}, {"drops", 0}};

            prod_h.brc.send_heartbeat(channel, prod_uid, "producer",
                                       prod_metrics);
            cons_h.brc.send_heartbeat(channel, cons_uid, "consumer",
                                       cons_metrics);

            auto heartbeats_processed = [&] {
                auto prod_re = broker->host->state().role(prod_uid);
                auto cons_re = broker->host->state().role(cons_uid);
                if (!prod_re.has_value() || !cons_re.has_value())
                    return false;
                const auto *prod_p =
                    prod_re->find_presence(channel, "producer");
                const auto *cons_p =
                    cons_re->find_presence(channel, "consumer");
                return prod_p && cons_p
                    && prod_p->first_heartbeat_seen
                    && cons_p->first_heartbeat_seen;
            };
            EXPECT_TRUE(poll_until(heartbeats_processed,
                                    std::chrono::seconds(2)))
                << "presence rows did not record first_heartbeat_seen "
                   "within 2s";

            auto prod_re = broker->host->state().role(prod_uid);
            ASSERT_TRUE(prod_re.has_value());
            const RolePresence *prod_p =
                prod_re->find_presence(channel, "producer");
            ASSERT_NE(prod_p, nullptr) << "producer-presence row missing";
            EXPECT_EQ(prod_p->channel, channel);
            EXPECT_EQ(prod_p->role_type, "producer");
            EXPECT_TRUE(prod_p->first_heartbeat_seen);
            EXPECT_EQ(prod_p->state, RoleState::Connected);
            ASSERT_TRUE(prod_p->latest_metrics.is_object())
                << "producer-presence latest_metrics is not a JSON object: "
                << prod_p->latest_metrics.type_name();
            ASSERT_TRUE(prod_p->latest_metrics.contains("out_written"));
            EXPECT_EQ(prod_p->latest_metrics.value("out_written", 0), 100);

            auto cons_re = broker->host->state().role(cons_uid);
            ASSERT_TRUE(cons_re.has_value());
            const RolePresence *cons_p =
                cons_re->find_presence(channel, "consumer");
            ASSERT_NE(cons_p, nullptr) << "consumer-presence row missing";
            EXPECT_EQ(cons_p->channel, channel);
            EXPECT_EQ(cons_p->role_type, "consumer");
            EXPECT_TRUE(cons_p->first_heartbeat_seen);
            EXPECT_EQ(cons_p->state, RoleState::Connected);
            ASSERT_TRUE(cons_p->latest_metrics.is_object())
                << "consumer-presence latest_metrics is not a JSON object: "
                << cons_p->latest_metrics.type_name();
            ASSERT_TRUE(cons_p->latest_metrics.contains("in_received"));
            EXPECT_EQ(cons_p->latest_metrics.value("in_received", 0), 100);

            EXPECT_EQ(prod_re->find_presence(channel, "consumer"), nullptr)
                << "producer's RoleEntry incorrectly contains a "
                   "consumer-presence row — broker keyed off the channel's "
                   "first producer instead of the wire-decoded uid";
            EXPECT_EQ(cons_re->find_presence(channel, "producer"), nullptr)
                << "consumer's RoleEntry incorrectly contains a "
                   "producer-presence row";

            EXPECT_EQ(prod_re->presences.size(), 1u);
            EXPECT_EQ(cons_re->presences.size(), 1u);

            cons_h.stop();
            prod_h.stop();
        });
}

// ============================================================================
// 5. ROLE_PRESENCE_REQ + ROLE_INFO_REQ
// ============================================================================

int role_presence_req_unknown_uid()
{
    return run_with_host(
        "broker_protocol::role_presence_req_unknown_uid",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, "QUERIER-unknown");
            auto resp = bh.brc.query_role_presence(
                "prod.unknown.uiddeadbeef", 2000);
            ASSERT_TRUE(resp.has_value())
                << "Broker should respond, not time out";
            EXPECT_FALSE(resp->value("present", true))
                << "Unknown uid → present=false";
            bh.stop();
        });
}

int role_info_req_unknown_uid()
{
    return run_with_host(
        "broker_protocol::role_info_req_unknown_uid",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, "QUERIER-unknown2");
            auto info = bh.brc.query_role_info(
                "prod.unknown.uiddeadbeef", 2000);
            if (info.has_value())
            {
                EXPECT_FALSE(info->value("found", true))
                    << "Expected found=false for unknown UID";
            }
            bh.stop();
        });
}

int role_presence_req_producer_uid()
{
    return run_with_host(
        "broker_protocol::role_presence_req_producer_uid",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel = pid_chan("proto.presence.prod");
            const std::string uid     = "prod.prestest.uidaaaa0001";

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, uid);
            auto opts = make_reg_opts(channel, uid);
            opts["role_name"] = "PresTestProd";
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle querier;
            querier.start(broker->endpoint, broker->pubkey,
                          "QUERIER-pres-prod");
            EXPECT_TRUE(querier.brc.query_role_presence(uid, 2000));

            querier.stop();
            prod_bh.stop();
        });
}

int role_presence_req_consumer_uid()
{
    return run_with_host(
        "broker_protocol::role_presence_req_consumer_uid",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel      = pid_chan("proto.presence.cons");
            const std::string prod_uid     = "prod." + channel;
            const std::string consumer_uid = "cons.prestest.uidbbbb0002";

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.start(broker->endpoint, broker->pubkey, consumer_uid);
            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, consumer_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value());

            BrcHandle querier;
            querier.start(broker->endpoint, broker->pubkey,
                          "QUERIER-pres-cons");
            EXPECT_TRUE(querier.brc.query_role_presence(consumer_uid, 2000));

            querier.stop();
            cons_bh.stop();
            prod_bh.stop();
        });
}

int role_info_req_with_inbox()
{
    return run_with_host(
        "broker_protocol::role_info_req_with_inbox",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.roleinfo.withinbox");
            const std::string uid      = "prod.roleinfo.uiddddd0004";
            const std::string inbox_ep = "tcp://127.0.0.1:9987";
            const std::string schema_json =
                R"([{"type":"float64","count":1,"length":0}])";
            const std::string packing = "aligned";

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, uid);

            auto opts = make_reg_opts(channel, uid);
            opts["role_name"]         = "InboxProd";
            opts["inbox_endpoint"]    = inbox_ep;
            opts["inbox_schema_json"] = schema_json;
            opts["inbox_packing"]     = packing;
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle querier;
            querier.start(broker->endpoint, broker->pubkey,
                          "QUERIER-roleinfo");
            auto info = querier.brc.query_role_info(uid, 2000);
            ASSERT_TRUE(info.has_value()) << "Expected role info, got nullopt";
            EXPECT_EQ(info->value("inbox_endpoint", ""), inbox_ep);
            EXPECT_EQ(info->value("inbox_packing", ""), packing);

            querier.stop();
            prod_bh.stop();
        });
}

// ============================================================================
// 6. Transport arbitration
// ============================================================================

int transport_mismatch_shm_producer_zmq_consumer()
{
    return run_with_host(
        "broker_protocol::transport_mismatch_shm_producer_zmq_consumer",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  =
                pid_chan("proto.transport.shm_zmq");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);
            auto cons_opts = make_cons_opts(channel, cons_uid);
            cons_opts["consumer_queue_type"] = "zmq";
            auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
            ASSERT_TRUE(cons_reg.has_value())
                << "Broker should respond with ERROR, not silent timeout";
            EXPECT_EQ(cons_reg->value("status", std::string{}), "error");
            EXPECT_EQ(cons_reg->value("error_code", std::string{}),
                      "TRANSPORT_MISMATCH");

            cons_bh.stop();
            prod_bh.stop();
        },
        {"transport mismatch"});
}

int transport_match_shm_consumer_shm_producer()
{
    return run_with_host(
        "broker_protocol::transport_match_shm_consumer_shm_producer",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  =
                pid_chan("proto.transport.shm_shm");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);
            auto cons_opts = make_cons_opts(channel, cons_uid);
            cons_opts["consumer_queue_type"] = "shm";
            auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
            EXPECT_TRUE(cons_reg.has_value())
                << "Both sides use SHM — should succeed";

            cons_bh.stop();
            prod_bh.stop();
        });
}

int transport_match_no_driver_field()
{
    return run_with_host(
        "broker_protocol::transport_match_no_driver_field",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  =
                pid_chan("proto.transport.nofield");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            BrcHandle cons_bh;
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);
            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, cons_uid), 3000);
            EXPECT_TRUE(cons_reg.has_value())
                << "Should succeed when consumer_queue_type is omitted";

            cons_bh.stop();
            prod_bh.stop();
        });
}

// ============================================================================
// 7. REG_ACK / CONSUMER_REG_ACK heartbeat-negotiation block
// ============================================================================

int reg_ack_contains_heartbeat_block_defaults()
{
    return run_with_host(
        "broker_protocol::reg_ack_contains_heartbeat_block_defaults",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel = pid_chan("proto.regack.hb_default");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            ASSERT_TRUE(reg->contains("heartbeat"))
                << "REG_ACK missing heartbeat block";
            const auto &hb = (*reg)["heartbeat"];
            ASSERT_TRUE(hb.is_object());

            EXPECT_EQ(hb.value("heartbeat_interval_ms", -1),
                      ::pylabhub::kDefaultHeartbeatIntervalMs);
            EXPECT_EQ(hb.value("ready_miss_heartbeats", uint32_t{0}),
                      ::pylabhub::kDefaultReadyMissHeartbeats);
            EXPECT_EQ(hb.value("pending_miss_heartbeats", uint32_t{0}),
                      ::pylabhub::kDefaultPendingMissHeartbeats);

            bh.stop();
        });
}

int reg_ack_heartbeat_block_honors_custom_config()
{
    return run_with_host(
        "broker_protocol::reg_ack_heartbeat_block_honors_custom_config",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            broker.reset();
            BrokerService::Config cfg;
            cfg.endpoint                = "tcp://127.0.0.1:0";
            cfg.heartbeat_interval      = std::chrono::milliseconds(250);
            cfg.ready_miss_heartbeats   = 12;
            cfg.pending_miss_heartbeats = 8;
            broker.emplace(start_local_broker(std::move(cfg)));

            const std::string channel = pid_chan("proto.regack.hb_custom");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            ASSERT_TRUE(reg->contains("heartbeat"));
            const auto &hb = (*reg)["heartbeat"];
            EXPECT_EQ(hb.value("heartbeat_interval_ms", -1), 250);
            EXPECT_EQ(hb.value("ready_miss_heartbeats", uint32_t{0}), 12u);
            EXPECT_EQ(hb.value("pending_miss_heartbeats", uint32_t{0}), 8u);

            bh.stop();
        });
}

int consumer_reg_ack_contains_heartbeat_block()
{
    return run_with_host(
        "broker_protocol::consumer_reg_ack_contains_heartbeat_block",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.cons_regack.hb");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());
            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});

            BrcHandle cons_bh;
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);
            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value());

            ASSERT_TRUE(cons_reg->contains("heartbeat"))
                << "CONSUMER_REG_ACK missing heartbeat block";
            const auto &hb = (*cons_reg)["heartbeat"];
            EXPECT_TRUE(hb.contains("heartbeat_interval_ms"));
            EXPECT_TRUE(hb.contains("ready_miss_heartbeats"));
            EXPECT_TRUE(hb.contains("pending_miss_heartbeats"));

            cons_bh.stop();
            prod_bh.stop();
        });
}

// ============================================================================
// 8. CHANNEL_BROADCAST_REQ — fan-out to producer + ALL consumers
// ============================================================================

int broadcast_fan_out_delivered_to_producer_and_consumers()
{
    return run_with_host(
        "broker_protocol::broadcast_fan_out_delivered_to_producer_and_consumers",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel   = pid_chan("proto.bcast.fanout");
            const std::string prod_uid  = "prod." + channel;
            const std::string cons1_uid = "cons.first." + channel;
            const std::string cons2_uid = "cons.second." + channel;
            const std::string send_uid  =
                "prod.broadcast.sender.pid" +
                std::to_string(static_cast<unsigned long>(::getpid()));

            auto prod_evts  = std::make_shared<EventCollector>();
            auto cons1_evts = std::make_shared<EventCollector>();
            auto cons2_evts = std::make_shared<EventCollector>();

            auto only_bcast = [](std::shared_ptr<EventCollector> col) {
                return [col](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_NOTIFY") col->push(t, b);
                };
            };

            BrcHandle prod_bh;
            prod_bh.brc.on_notification(only_bcast(prod_evts));
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons1_bh;
            cons1_bh.brc.on_notification(only_bcast(cons1_evts));
            cons1_bh.start(broker->endpoint, broker->pubkey, cons1_uid);
            {
                auto opts = make_cons_opts(channel, cons1_uid);
                opts["consumer_pid"] =
                    static_cast<uint64_t>(::getpid()) * 100u + 1u;
                ASSERT_TRUE(cons1_bh.brc.register_consumer(opts, 3000)
                                .has_value());
            }

            BrcHandle cons2_bh;
            cons2_bh.brc.on_notification(only_bcast(cons2_evts));
            cons2_bh.start(broker->endpoint, broker->pubkey, cons2_uid);
            {
                auto opts = make_cons_opts(channel, cons2_uid);
                opts["consumer_pid"] =
                    static_cast<uint64_t>(::getpid()) * 100u + 2u;
                ASSERT_TRUE(cons2_bh.brc.register_consumer(opts, 3000)
                                .has_value());
            }

            BrcHandle sender_bh;
            auto sender_evts = std::make_shared<EventCollector>();
            sender_bh.brc.on_notification(only_bcast(sender_evts));
            sender_bh.start(broker->endpoint, broker->pubkey, send_uid);

            sender_bh.brc.send_broadcast(channel, send_uid,
                                          "hello-fan-out", "");

            ASSERT_TRUE(prod_evts->wait_for(1, 5000))
                << "producer did not receive CHANNEL_BROADCAST_NOTIFY";
            ASSERT_TRUE(cons1_evts->wait_for(1, 5000))
                << "cons1 did not receive CHANNEL_BROADCAST_NOTIFY";
            ASSERT_TRUE(cons2_evts->wait_for(1, 5000))
                << "cons2 did not receive CHANNEL_BROADCAST_NOTIFY";

            auto check_payload =
                [&](const json &b, const char *who) {
                    EXPECT_EQ(b.value("channel_name", ""), channel) << who;
                    EXPECT_EQ(b.value("event", ""), "broadcast") << who;
                    EXPECT_EQ(b.value("sender_uid", ""), send_uid) << who;
                    EXPECT_EQ(b.value("message", ""), "hello-fan-out") << who;
                };
            {
                std::lock_guard<std::mutex> lk(prod_evts->mtx);
                ASSERT_FALSE(prod_evts->events.empty());
                check_payload(prod_evts->events.front().second, "producer");
            }
            {
                std::lock_guard<std::mutex> lk(cons1_evts->mtx);
                ASSERT_FALSE(cons1_evts->events.empty());
                check_payload(cons1_evts->events.front().second, "cons1");
            }
            {
                std::lock_guard<std::mutex> lk(cons2_evts->mtx);
                ASSERT_FALSE(cons2_evts->events.empty());
                check_payload(cons2_evts->events.front().second, "cons2");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            EXPECT_EQ(sender_evts->size(), 0u)
                << "external (non-member) sender unexpectedly received "
                   "NOTIFY";

            sender_bh.stop();
            cons2_bh.stop();
            cons1_bh.stop();
            prod_bh.stop();
        });
}

int broadcast_fan_out_data_payload_round_trip()
{
    return run_with_host(
        "broker_protocol::broadcast_fan_out_data_payload_round_trip",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.bcast.payload");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;
            const std::string send_uid = "ext.bcast.payload.uid00000077";
            const std::string msg      = "payload-test";
            const std::string data     =
                R"({"k":"v","n":42,"arr":[1,2,3]})";

            auto cons_evts = std::make_shared<EventCollector>();

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            ASSERT_TRUE(prod_bh.brc.register_channel(
                            make_reg_opts(channel, prod_uid), 3000)
                            .has_value());

            BrcHandle cons_bh;
            cons_bh.brc.on_notification(
                [cons_evts](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_NOTIFY")
                        cons_evts->push(t, b);
                });
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);
            ASSERT_TRUE(cons_bh.brc.register_consumer(
                            make_cons_opts(channel, cons_uid), 3000)
                            .has_value());

            BrcHandle sender_bh;
            sender_bh.start(broker->endpoint, broker->pubkey, send_uid);
            sender_bh.brc.send_broadcast(channel, send_uid, msg, data);

            ASSERT_TRUE(cons_evts->wait_for(1, 5000))
                << "consumer did not receive broadcast NOTIFY with data "
                   "payload";

            std::lock_guard<std::mutex> lk(cons_evts->mtx);
            ASSERT_FALSE(cons_evts->events.empty());
            const auto &body = cons_evts->events.front().second;
            EXPECT_EQ(body.value("channel_name", ""), channel);
            EXPECT_EQ(body.value("event", ""), "broadcast");
            EXPECT_EQ(body.value("sender_uid", ""), send_uid);
            EXPECT_EQ(body.value("message", ""), msg);
            EXPECT_EQ(body.value("data", ""), data)
                << "data payload was modified in transit";

            sender_bh.stop();
            cons_bh.stop();
            prod_bh.stop();
        });
}

int broadcast_unknown_channel_no_notify_delivered()
{
    return run_with_host(
        "broker_protocol::broadcast_unknown_channel_no_notify_delivered",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.bcast.unknown");
            const std::string send_uid = "ext.bcast.unknown.uid00000088";

            const std::string other_ch  = pid_chan("proto.bcast.other");
            const std::string other_prd = "prod." + other_ch;
            const std::string spec_uid  = "cons." + other_ch;
            auto spec_evts = std::make_shared<EventCollector>();

            BrcHandle other_prod;
            other_prod.start(broker->endpoint, broker->pubkey, other_prd);
            ASSERT_TRUE(other_prod.brc.register_channel(
                            make_reg_opts(other_ch, other_prd), 3000)
                            .has_value());

            BrcHandle spec_bh;
            spec_bh.brc.on_notification(
                [spec_evts](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_NOTIFY")
                        spec_evts->push(t, b);
                });
            spec_bh.start(broker->endpoint, broker->pubkey, spec_uid);
            ASSERT_TRUE(spec_bh.brc.register_consumer(
                            make_cons_opts(other_ch, spec_uid), 3000)
                            .has_value());

            BrcHandle sender_bh;
            sender_bh.start(broker->endpoint, broker->pubkey, send_uid);
            sender_bh.brc.send_broadcast(channel, send_uid,
                                          "into-the-void", "");

            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            EXPECT_EQ(spec_evts->size(), 0u)
                << "broadcast for unknown channel leaked to other-channel "
                   "consumer";

            EXPECT_NO_THROW({
                auto snap =
                    broker->host->broker().query_channel_snapshot();
                (void)snap;
            }) << "broker stopped servicing requests after broadcast for "
                  "unknown channel";

            sender_bh.stop();
            spec_bh.stop();
            other_prod.stop();
        });
}

int broadcast_fan_out_hub_queue_path_fans_out_same()
{
    return run_with_host(
        "broker_protocol::broadcast_fan_out_hub_queue_path_fans_out_same",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.bcast.hubpath");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;
            const std::string msg      = "from-hub-script";
            const std::string data     = "extra";

            auto cons_evts = std::make_shared<EventCollector>();
            auto prod_evts = std::make_shared<EventCollector>();

            BrcHandle prod_bh;
            prod_bh.brc.on_notification(
                [prod_evts](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_NOTIFY")
                        prod_evts->push(t, b);
                });
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            ASSERT_TRUE(prod_bh.brc.register_channel(
                            make_reg_opts(channel, prod_uid), 3000)
                            .has_value());

            BrcHandle cons_bh;
            cons_bh.brc.on_notification(
                [cons_evts](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_NOTIFY")
                        cons_evts->push(t, b);
                });
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);
            ASSERT_TRUE(cons_bh.brc.register_consumer(
                            make_cons_opts(channel, cons_uid), 3000)
                            .has_value());

            broker->host->broker().request_broadcast_channel(
                channel, msg, data);

            ASSERT_TRUE(prod_evts->wait_for(1, 5000))
                << "producer did not receive in-process broadcast";
            ASSERT_TRUE(cons_evts->wait_for(1, 5000))
                << "consumer did not receive in-process broadcast";

            const std::string self_uid =
                broker->host->config().identity().uid;
            ASSERT_FALSE(self_uid.empty())
                << "real HubHost must populate self_hub_uid";
            auto check_hub_payload = [&](const json &b, const char *who) {
                EXPECT_EQ(b.value("channel_name", ""), channel) << who;
                EXPECT_EQ(b.value("event", ""), "broadcast") << who;
                EXPECT_EQ(b.value("sender_uid", ""), self_uid) << who;
                EXPECT_EQ(b.value("message", ""), msg) << who;
                EXPECT_EQ(b.value("data", ""), data) << who;
            };
            {
                std::lock_guard<std::mutex> lk(prod_evts->mtx);
                ASSERT_FALSE(prod_evts->events.empty());
                check_hub_payload(prod_evts->events.front().second,
                                   "producer");
            }
            {
                std::lock_guard<std::mutex> lk(cons_evts->mtx);
                ASSERT_FALSE(cons_evts->events.empty());
                check_hub_payload(cons_evts->events.front().second,
                                   "consumer");
            }

            cons_bh.stop();
            prod_bh.stop();
        });
}

// ============================================================================
// Audit TR1 — wire-conformance regression tests (2026-05-17)
// ============================================================================
//
// Pre-2026-05-17, no test in the suite pinned a wire payload key set
// directly against a HEP §.  The B1 audit found that the BRC + broker
// agreed on the wrong band wire key (`channel` instead of `band` per
// HEP-CORE-0030 §5.1) for over a year because round-trip tests
// passed.  These tests use the `pylabhub::tests::wire` helpers
// (`tests/test_framework/wire_conformance.h`) to lock down the
// observable shape of major ACK families against their authoritative
// HEP §:
//
//   REG_ACK / CONSUMER_REG_ACK         — HEP-CORE-0023 §2.5.1
//   DISC_REQ_ACK (CHANNEL_NOT_FOUND +
//                 DISC_ACK variants)   — HEP-CORE-0023 §2.2
//   ROLE_INFO_ACK                       — HEP-CORE-0027 §4.2
//   BAND_JOIN_ACK / BAND_LEAVE_ACK /
//   BAND_MEMBERS_ACK                    — HEP-CORE-0030 §5.1
//
// Each test asserts both required keys (HAS) and forbidden legacy
// keys (LACKS), so a stale rename leaves the test failing with a
// pinpoint diagnostic.

int wire_conformance_reg_ack_shape()
{
    return run_with_host(
        "broker_protocol::wire_conformance_reg_ack_shape",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            using namespace pylabhub::tests::wire;
            const std::string channel = pid_chan("tr1.regack");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "REG_REQ timed out";

            // HEP-CORE-0023 §2.5.1 + §2.5 — REG_ACK carries `status`,
            // `channel_id` (the broker-side identifier), and the
            // `heartbeat` block.  We don't pin every optional field
            // here; we pin the spec-named ones and reject legacy /
            // typo-style names.
            expect_object_has_keys(*reg,
                {"status", "heartbeat"},
                "REG_ACK", "HEP-CORE-0023 §2.5.1");
            expect_string_field(*reg, "status", "success",
                                 "REG_ACK", "HEP-CORE-0023 §2.5.1");

            // Stale / wrong keys that would indicate a rename leak:
            // - `channel` (used by BAND_* — wrong family here)
            // - `band` (band-family — wrong family here)
            // - `name` (over-generic, suggests refactor in flight)
            expect_object_lacks_keys(*reg,
                {"band"},
                "REG_ACK", "HEP-CORE-0023 §2.5.1 (band family is "
                "separate per HEP-CORE-0030 §5.1)");

            // Heartbeat sub-block keys per §2.5.1.
            const auto &hb = (*reg)["heartbeat"];
            expect_object_has_keys(hb,
                {"heartbeat_interval_ms",
                 "ready_miss_heartbeats",
                 "pending_miss_heartbeats"},
                "REG_ACK.heartbeat", "HEP-CORE-0023 §2.5.1");
            expect_int_field(hb, "heartbeat_interval_ms",
                              "REG_ACK.heartbeat",
                              "HEP-CORE-0023 §2.5.1");
            expect_int_field(hb, "ready_miss_heartbeats",
                              "REG_ACK.heartbeat",
                              "HEP-CORE-0023 §2.5.1");
            expect_int_field(hb, "pending_miss_heartbeats",
                              "REG_ACK.heartbeat",
                              "HEP-CORE-0023 §2.5.1");

            bh.stop();
        });
}

int wire_conformance_consumer_reg_ack_shape()
{
    return run_with_host(
        "broker_protocol::wire_conformance_consumer_reg_ack_shape",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            using namespace pylabhub::tests::wire;
            const std::string channel = pid_chan("tr1.creg");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            // Producer first so the channel exists for the consumer.
            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid);
            ASSERT_TRUE(prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000).has_value());

            BrcHandle cons_bh;
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid);
            auto reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, cons_uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "CONSUMER_REG_REQ timed out";

            expect_object_has_keys(*reg,
                {"status", "heartbeat"},
                "CONSUMER_REG_ACK", "HEP-CORE-0023 §2.5.1");
            expect_string_field(*reg, "status", "success",
                                 "CONSUMER_REG_ACK",
                                 "HEP-CORE-0023 §2.5.1");
            expect_object_lacks_keys(*reg,
                {"band"},
                "CONSUMER_REG_ACK", "HEP-CORE-0023 §2.5.1");

            const auto &hb = (*reg)["heartbeat"];
            expect_object_has_keys(hb,
                {"heartbeat_interval_ms",
                 "ready_miss_heartbeats",
                 "pending_miss_heartbeats"},
                "CONSUMER_REG_ACK.heartbeat",
                "HEP-CORE-0023 §2.5.1");

            cons_bh.stop();
            prod_bh.stop();
        });
}

int wire_conformance_role_info_ack_shape()
{
    return run_with_host(
        "broker_protocol::wire_conformance_role_info_ack_shape",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            using namespace pylabhub::tests::wire;

            // ── Case 1: target uid found (no inbox configured) ─────
            const std::string channel = pid_chan("tr1.roleinfo");
            const std::string uid     = "prod." + channel;
            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            ASSERT_TRUE(bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000).has_value());

            // Side BRC to query.
            BrcHandle querier;
            querier.start(broker->endpoint, broker->pubkey,
                          "tr1.querier.uid0000001");
            auto info = querier.brc.query_role_info(uid, 3000);
            ASSERT_TRUE(info.has_value())
                << "ROLE_INFO_REQ timed out for a registered uid";

            // HEP-CORE-0027 §4.2 + HEP-CORE-0023 §4: ROLE_INFO_ACK
            // body for a found role carries `found`, `channel`, and
            // inbox metadata (empty when no inbox advertised).
            expect_object_has_keys(*info,
                {"found", "channel", "inbox_endpoint",
                 "inbox_packing", "inbox_checksum", "inbox_schema"},
                "ROLE_INFO_ACK", "HEP-CORE-0027 §4.2");

            // Audit I1 (Round-2): code returns parsed `inbox_schema`
            // object, NOT the stringified `inbox_schema_json` that
            // some HEPs still mention.  Pin the code's contract.
            expect_object_lacks_keys(*info,
                {"inbox_schema_json"},
                "ROLE_INFO_ACK",
                "HEP-CORE-0027 §4.2 (code returns parsed object; "
                "any HEP reference to `inbox_schema_json` is stale)");

            // ── Case 2: unknown uid → found=false ────────────────────
            // broker_proto 5 (R3.5b): the uid must be well-formed
            // (HEP-CORE-0033 §G2.2.0b: tag + name + unique) — using
            // a `prod.`-prefixed unique placeholder so the grammar
            // gate passes; absence is verified by the broker's
            // not-found scan.
            auto not_found = querier.brc.query_role_info(
                "prod.no.such.role.uid00000000", 3000);
            ASSERT_TRUE(not_found.has_value())
                << "ROLE_INFO_REQ for unknown uid should still get an "
                   "ACK (with found=false), not time out";
            expect_object_has_keys(*not_found,
                {"found"},
                "ROLE_INFO_ACK (unknown uid)",
                "HEP-CORE-0027 §4.2");
            ASSERT_TRUE(not_found->at("found").is_boolean());
            EXPECT_FALSE(not_found->at("found").get<bool>())
                << "ROLE_INFO_ACK.found must be false for an unknown uid";

            querier.stop();
            bh.stop();
        });
}
// Audit R3.6 (2026-05-17): `wire_conformance_channel_notify_req_federation_relay`
// retired.  The broker-side `handle_channel_notify_req` handler was
// deleted because federation peer-relay actually uses `HUB_RELAY_MSG`
// (broker↔broker) rather than `CHANNEL_NOTIFY_REQ`.  See
// `docs/code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` (R3.6)
// for the investigation.  Old clients sending CHANNEL_NOTIFY_REQ now
// receive UNKNOWN_MSG_TYPE via the standard dispatch fall-through.


int wire_conformance_band_ack_shapes()
{
    return run_with_host(
        "broker_protocol::wire_conformance_band_ack_shapes",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &) {
            using namespace pylabhub::tests::wire;

            // Need a registered channel so BRC has a role_uid + the
            // role can join a band.  Band names are `!`-prefixed per
            // HEP-CORE-0030 §3.
            const std::string channel = pid_chan("tr1.bandshape");
            const std::string uid     = "prod." + channel;
            const std::string band    = "!" + pid_chan("tr1.band");

            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            ASSERT_TRUE(bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000).has_value());

            // ── BAND_JOIN_ACK ──────────────────────────────────────
            auto join_ack = bh.brc.band_join(band, 3000);
            ASSERT_TRUE(join_ack.has_value()) << "BAND_JOIN_REQ timed out";
            expect_object_has_keys(*join_ack,
                {"status", "band", "members"},
                "BAND_JOIN_ACK", "HEP-CORE-0030 §5.1");
            expect_string_field(*join_ack, "status", "success",
                                 "BAND_JOIN_ACK",
                                 "HEP-CORE-0030 §5.1");
            // Stale key from before the audit-B1 rename
            // (2026-05-17): the wire field used to be `channel`.
            // If the rename leaks back in, this test catches it.
            expect_object_lacks_keys(*join_ack,
                {"channel"},
                "BAND_JOIN_ACK",
                "HEP-CORE-0030 §5.1 (audit B1 — wire key is `band`)");
            ASSERT_TRUE(join_ack->at("band").is_string());
            EXPECT_EQ(join_ack->at("band").get<std::string>(), band);
            ASSERT_TRUE(join_ack->at("members").is_array());

            // ── BAND_MEMBERS_ACK ───────────────────────────────────
            auto members_ack = bh.brc.band_members(band, 3000);
            ASSERT_TRUE(members_ack.has_value())
                << "BAND_MEMBERS_REQ timed out";
            expect_object_has_keys(*members_ack,
                {"band", "members"},
                "BAND_MEMBERS_ACK", "HEP-CORE-0030 §5.1");
            expect_object_lacks_keys(*members_ack,
                {"channel"},
                "BAND_MEMBERS_ACK",
                "HEP-CORE-0030 §5.1 (audit B1 — wire key is `band`)");

            // ── BAND_LEAVE_ACK ─────────────────────────────────────
            auto leave_ack = bh.brc.band_leave(band, 3000);
            ASSERT_TRUE(leave_ack.has_value())
                << "BAND_LEAVE_REQ timed out";
            expect_object_has_keys(*leave_ack,
                {"status"},
                "BAND_LEAVE_ACK", "HEP-CORE-0030 §5.1");
            expect_string_field(*leave_ack, "status", "success",
                                 "BAND_LEAVE_ACK",
                                 "HEP-CORE-0030 §5.1");

            bh.stop();
        });
}

// ──────────────────────────────────────────────────────────────────────────────
// Audit M1 (2026-05-20): BAND_JOIN_ACK / BAND_LEAVE_ACK + ERROR replies on
// the BAND paths must echo the request's `correlation_id` (broker_proto 5
// contract; broker-side fix in commit `d759424`).  BRC's `do_request`
// doesn't currently inject `correlation_id` into outgoing requests
// (matches by message-type), so this test bypasses BRC and uses raw
// ZMQ to drive the explicit-corr_id path that other clients (admin
// tools, federation peers, future BRC versions) rely on.
//
// Pins three echo cases:
//   1. BAND_JOIN_ACK success — corr_id in body matches request.
//   2. BAND_LEAVE_ACK success — corr_id in body matches request.
//   3. BAND_LEAVE error (NOT_A_MEMBER) — corr_id in body matches request.
//
// Plus a backward-compat case: when the request omits corr_id, the
// reply must NOT carry an empty corr_id field (matches `make_error`'s
// `!correlation_id.empty()` gate at `broker_service.cpp:3662`).
// ──────────────────────────────────────────────────────────────────────────────
int wire_conformance_band_corr_id_echo()
{
    return run_with_host(
        "broker_protocol::wire_conformance_band_corr_id_echo",
        [](std::optional<HubHostHandle> &broker, LogCaptureFixture &lcf) {
            using namespace pylabhub::tests::wire;
            // Case 3 below deliberately drives the broker's
            // `NOT_A_MEMBER` rejection path, which emits a WARN.
            // Whitelist it via the LogCaptureFixture allowlist so the
            // intentional log doesn't fail the test.
            lcf.ExpectLogWarn("not a member of band");

            const std::string channel = pid_chan("tr1.bandcorr");
            const std::string uid     = "prod." + channel;
            const std::string role_nm = channel;
            const std::string band    = "!" + pid_chan("tr1.bcorr");

            // Register via BRC so the role_uid + role_name are valid
            // wire-grammar tokens and the broker has admitted the
            // producer.  BAND_JOIN handler only validates grammar /
            // side-aware tag, not registration, but registering keeps
            // the test path symmetric with realistic usage.
            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid);
            ASSERT_TRUE(bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000).has_value())
                << "REG_REQ setup failed";

            // ── Case 1: BAND_JOIN success echoes corr_id ─────────────
            const std::string join_corr = "test.band.join.corr.001";
            nlohmann::json join_req;
            join_req["band"]           = band;
            join_req["role_uid"]       = uid;
            join_req["role_name"]      = role_nm;
            join_req["correlation_id"] = join_corr;
            auto join_resp = ::pylabhub::tests::worker::broker::raw_req(
                broker->endpoint, "BAND_JOIN_REQ", join_req, 3000,
                broker->pubkey);
            ASSERT_FALSE(join_resp.is_null())
                << "BAND_JOIN_REQ timed out";
            ASSERT_EQ(join_resp.value("status", std::string{}), "success")
                << "BAND_JOIN_REQ failed; body=" << join_resp.dump();
            ASSERT_TRUE(join_resp.contains("correlation_id"))
                << "BAND_JOIN_ACK missing correlation_id field "
                   "(broker_proto 5 contract; broker_service.cpp B1 fix); "
                   "body=" << join_resp.dump();
            EXPECT_EQ(join_resp.at("correlation_id").get<std::string>(),
                      join_corr)
                << "BAND_JOIN_ACK echoed wrong correlation_id";

            // ── Case 2: BAND_LEAVE success echoes corr_id ────────────
            const std::string leave_corr = "test.band.leave.corr.002";
            nlohmann::json leave_req;
            leave_req["band"]           = band;
            leave_req["role_uid"]       = uid;
            leave_req["correlation_id"] = leave_corr;
            auto leave_resp = ::pylabhub::tests::worker::broker::raw_req(
                broker->endpoint, "BAND_LEAVE_REQ", leave_req, 3000,
                broker->pubkey);
            ASSERT_FALSE(leave_resp.is_null())
                << "BAND_LEAVE_REQ timed out";
            ASSERT_EQ(leave_resp.value("status", std::string{}), "success")
                << "BAND_LEAVE_REQ failed; body=" << leave_resp.dump();
            ASSERT_TRUE(leave_resp.contains("correlation_id"))
                << "BAND_LEAVE_ACK missing correlation_id; body="
                << leave_resp.dump();
            EXPECT_EQ(leave_resp.at("correlation_id").get<std::string>(),
                      leave_corr)
                << "BAND_LEAVE_ACK echoed wrong correlation_id";

            // ── Case 3: BAND_LEAVE NOT_A_MEMBER error echoes corr_id ─
            // The role left successfully in Case 2; another LEAVE
            // must produce typed `NOT_A_MEMBER` (HEP-CORE-0030 S4
            // amendment).  The error path must also carry corr_id.
            const std::string err_corr = "test.band.leave.err.corr.003";
            nlohmann::json leave_req_err;
            leave_req_err["band"]           = band;
            leave_req_err["role_uid"]       = uid;
            leave_req_err["correlation_id"] = err_corr;
            auto err_resp = ::pylabhub::tests::worker::broker::raw_req(
                broker->endpoint, "BAND_LEAVE_REQ", leave_req_err, 3000,
                broker->pubkey);
            ASSERT_FALSE(err_resp.is_null())
                << "BAND_LEAVE_REQ (error path) timed out";
            ASSERT_EQ(err_resp.value("status", std::string{}), "error");
            EXPECT_EQ(err_resp.value("error_code", std::string{}),
                      "NOT_A_MEMBER");
            ASSERT_TRUE(err_resp.contains("correlation_id"))
                << "BAND_LEAVE error reply missing correlation_id; body="
                << err_resp.dump();
            EXPECT_EQ(err_resp.at("correlation_id").get<std::string>(),
                      err_corr)
                << "BAND_LEAVE error reply echoed wrong correlation_id";

            // ── Case 4: backward compat — no corr_id in req → none in ACK ──
            // Re-join (no corr_id), verify ACK lacks the field rather
            // than carrying an empty one.  `make_error` + the success
            // echo are both gated on `!corr_id.empty()`.
            nlohmann::json rejoin_req;
            rejoin_req["band"]      = band;
            rejoin_req["role_uid"]  = uid;
            rejoin_req["role_name"] = role_nm;
            auto rejoin = ::pylabhub::tests::worker::broker::raw_req(
                broker->endpoint, "BAND_JOIN_REQ", rejoin_req, 3000,
                broker->pubkey);
            ASSERT_FALSE(rejoin.is_null())
                << "BAND_JOIN_REQ (no-corr-id) timed out";
            ASSERT_EQ(rejoin.value("status", std::string{}), "success");
            EXPECT_FALSE(rejoin.contains("correlation_id"))
                << "BAND_JOIN_ACK must not carry correlation_id when "
                   "the request omitted it; body=" << rejoin.dump();

            bh.stop();
        });
}

} // namespace broker_protocol
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct BrokerProtocolRegistrar
{
    BrokerProtocolRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_protocol")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_protocol;

                if (sc == "checksum_error_report_forwarded_to_producer")
                    return checksum_error_report_forwarded_to_producer();
                if (sc == "checksum_error_report_unknown_channel_silent")
                    return checksum_error_report_unknown_channel_silent();
                if (sc == "closing_notify_delivered_to_producer_and_consumer")
                    return closing_notify_delivered_to_producer_and_consumer();
                if (sc == "duplicate_reg_shm_cardinality")
                    return duplicate_reg_shm_cardinality();
                if (sc == "duplicate_reg_different_schema_hash")
                    return duplicate_reg_different_schema_hash();
                if (sc == "heartbeat_transitions_to_ready")
                    return heartbeat_transitions_to_ready();
                if (sc == "heartbeat_wire_payload_includes_uid_and_role_type")
                    return heartbeat_wire_payload_includes_uid_and_role_type();
                if (sc == "heartbeat_keying_producer_vs_consumer_distinct_rows")
                    return heartbeat_keying_producer_vs_consumer_distinct_rows();
                if (sc == "role_presence_req_unknown_uid")
                    return role_presence_req_unknown_uid();
                if (sc == "role_info_req_unknown_uid")
                    return role_info_req_unknown_uid();
                if (sc == "role_presence_req_producer_uid")
                    return role_presence_req_producer_uid();
                if (sc == "role_presence_req_consumer_uid")
                    return role_presence_req_consumer_uid();
                if (sc == "role_info_req_with_inbox")
                    return role_info_req_with_inbox();
                if (sc == "transport_mismatch_shm_producer_zmq_consumer")
                    return transport_mismatch_shm_producer_zmq_consumer();
                if (sc == "transport_match_shm_consumer_shm_producer")
                    return transport_match_shm_consumer_shm_producer();
                if (sc == "transport_match_no_driver_field")
                    return transport_match_no_driver_field();
                if (sc == "reg_ack_contains_heartbeat_block_defaults")
                    return reg_ack_contains_heartbeat_block_defaults();
                if (sc == "reg_ack_heartbeat_block_honors_custom_config")
                    return reg_ack_heartbeat_block_honors_custom_config();
                if (sc == "consumer_reg_ack_contains_heartbeat_block")
                    return consumer_reg_ack_contains_heartbeat_block();
                if (sc == "broadcast_fan_out_delivered_to_producer_and_consumers")
                    return broadcast_fan_out_delivered_to_producer_and_consumers();
                if (sc == "broadcast_fan_out_data_payload_round_trip")
                    return broadcast_fan_out_data_payload_round_trip();
                if (sc == "broadcast_unknown_channel_no_notify_delivered")
                    return broadcast_unknown_channel_no_notify_delivered();
                if (sc == "broadcast_fan_out_hub_queue_path_fans_out_same")
                    return broadcast_fan_out_hub_queue_path_fans_out_same();
                // Audit TR1 — wire-conformance regressions
                if (sc == "wire_conformance_reg_ack_shape")
                    return wire_conformance_reg_ack_shape();
                if (sc == "wire_conformance_consumer_reg_ack_shape")
                    return wire_conformance_consumer_reg_ack_shape();
                if (sc == "wire_conformance_role_info_ack_shape")
                    return wire_conformance_role_info_ack_shape();
                if (sc == "wire_conformance_band_ack_shapes")
                    return wire_conformance_band_ack_shapes();
                if (sc == "wire_conformance_band_corr_id_echo")
                    return wire_conformance_band_corr_id_echo();
                // R3.6 retired — handler deleted, no test path
                return -1;
            });
    }
} g_registrar;

} // namespace
