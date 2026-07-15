/**
 * @file datahub_broker_protocol_workers.cpp
 * @brief Worker bodies for broker control-plane protocol tests
 *        (CHECKSUM_ERROR, CHANNEL_CLOSING_NOTIFY, REG_REQ duplicate /
 *         schema mismatch, HEARTBEAT_NOTIFY, ROLE_PRESENCE_REQ,
 *         ROLE_INFO_REQ, transport arbitration, REG_ACK heartbeat
 *         negotiation, CHANNEL_BROADCAST_SEND_NOTIFY fan-out).
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

#include "broker_test_harness.h"
#include "curve_test_setup.h"
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
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/scope_guard.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/security/shm_capability_channel.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/wire_adapter.hpp"
#include "utils/wire_envelope.hpp"

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

// ─── Broker handle alias + legacy-config → hub.json translation ───────────
//
// The previous local `HubHostHandle` + `start_local_broker()` rolled their
// own ephemeral hub-directory + HubConfig load + HubHost startup.  Post
// HEP-CORE-0035 §4.6.5 + HEP-CORE-0040 §172 the shared
// `pylabhub::tests::start_hubhost_broker(j_overrides, curve)` is the
// canonical assembly — it ALSO writes `known_roles.json` from
// `curve.role_keys` so the broker's Layer-1 ZAP admits every role in
// the bundle.  AUTH-6 batch-2a migration pulls this file onto that
// harness so every L3 broker test funnels through the same code path.

using HubHostHandle = pylabhub::tests::HubHostBrokerHandle;
using pylabhub::tests::BrcHandle;

/// Translate the legacy `BrokerService::Config` overrides callers
/// customise (heartbeat cadence, ready/pending timeouts, checksum
/// repair policy) into hub.json overrides for
/// `start_hubhost_broker(j_overrides, curve)`.  The non-customisable
/// fields (`network.broker_endpoint=tcp://127.0.0.1:0`, `admin.enabled
/// =false`, `script.path=""`) are merged in unconditionally so
/// callers do not have to repeat them.
json hubhost_overrides_from_cfg(const BrokerService::Config &cfg = {})
{
    json j{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
    if (cfg.heartbeat_interval.count() > 0)
        j["broker"]["heartbeat_interval_ms"] =
            static_cast<int>(cfg.heartbeat_interval.count());
    if (cfg.ready_miss_heartbeats > 0)
        j["broker"]["ready_miss_heartbeats"] = cfg.ready_miss_heartbeats;
    if (cfg.pending_miss_heartbeats > 0)
        j["broker"]["pending_miss_heartbeats"] = cfg.pending_miss_heartbeats;
    if (cfg.ready_timeout_override.has_value())
        j["broker"]["ready_timeout_ms"] =
            static_cast<int>(cfg.ready_timeout_override->count());
    if (cfg.pending_timeout_override.has_value())
        j["broker"]["pending_timeout_ms"] =
            static_cast<int>(cfg.pending_timeout_override->count());
    if (cfg.checksum_repair_policy == ChecksumRepairPolicy::NotifyOnly)
        j["broker"]["checksum_repair_policy"] = "notify_only";
    return j;
}

/// Start a broker via the shared harness with the caller's
/// `BrokerService::Config` overrides translated to hub.json shape.
/// `curve` MUST already have been seeded into the KeyStore via
/// `seed_curve_identities()` (typically by `run_with_host` below).
HubHostHandle start_local_broker(BrokerService::Config legacy_cfg,
                                 const pylabhub::tests::CurveSetup &curve)
{
    return pylabhub::tests::start_hubhost_broker(
        hubhost_overrides_from_cfg(std::move(legacy_cfg)),
        curve);
}

// ─── Channel-name + REG opts builders ─────────────────────────────────────────

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

// `make_reg_opts` / `make_cons_opts` consolidated into
// `tests/test_framework/broker_test_harness.h` (REVIEW_C2 F10
// 2026-06-29).  These were the original 2-param shape that won the
// consolidation — pubkey is functionally derivable from `role_uid`
// via the keystore (HEP-CORE-0036 §I10: one pubkey per role_uid).

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
// `body(broker, curve, log_cap)` receives:
//   - `broker`: `std::optional<HubHostHandle>` freshly emplaced with
//     `start_hubhost_broker(hubhost_overrides_from_cfg(), curve)`.
//     The body may `.reset()` and `.emplace(start_local_broker(cfg, curve))`
//     to swap to a custom-config broker — same idiom the original
//     suite used via `broker_.reset() / broker_.emplace(...)`.
//   - `curve`: the `CurveSetup` whose `role_keys` are seeded into the
//     process `KeyStore` AND written to `vault/known_roles.json` so the
//     broker's Layer-1 ZAP gate admits every role uid the body uses.
//     `role_uids` (the caller-supplied list passed to `run_with_host`)
//     drives both halves.
//   - `log_cap`: LogCaptureFixture for tests that need `log_path()`
//     (Heartbeat wire-payload echo) or ad-hoc ExpectLogWarn calls.

template <typename Body>
int run_with_host(std::string_view worker_name,
                  std::vector<std::string> role_uids,
                  Body &&body,
                  std::vector<std::string> warns  = {},
                  std::vector<std::string> errors = {})
{
    return run_gtest_worker(
        [role_uids = std::move(role_uids),
         body = std::forward<Body>(body),
         warns = std::move(warns),
         errors = std::move(errors)]() mutable {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : warns)
                log_cap.ExpectLogWarn(w);
            for (auto &e : errors)
                log_cap.ExpectLogError(e);

            auto curve = pylabhub::tests::make_curve_setup(role_uids);
            // HEP-CORE-0040 §172: fixture owns SMS + KeyStore + identity
            // seeding under `role.<uid>` names; start_hubhost_broker
            // only reads the KeyStore + writes known_roles.json.
            pylabhub::tests::seed_curve_identities(curve);

            std::optional<HubHostHandle> broker;
            broker.emplace(pylabhub::tests::start_hubhost_broker(
                hubhost_overrides_from_cfg(), curve));

            body(broker, curve, log_cap);

            broker.reset();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
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
        {"prod." + pid_chan("proto.checksum.prod"), "REPORT-" + pid_chan("proto.checksum.prod")},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
            broker.reset();
            BrokerService::Config cfg;
            cfg.endpoint               = "tcp://127.0.0.1:0";
            cfg.schema_search_dirs     = {};
            cfg.checksum_repair_policy = ChecksumRepairPolicy::NotifyOnly;
            broker.emplace(start_local_broker(std::move(cfg), curve));

            const std::string channel = pid_chan("proto.checksum.prod");
            const std::string uid     = "prod." + channel;

            auto prod_events = std::make_shared<EventCollector>();
            BrcHandle prod_bh;
            prod_bh.brc.on_notification(
                [prod_events](const std::string &type, const json &body) {
                    if (type == "CHANNEL_EVENT_NOTIFY")
                        prod_events->push(type, body);
                });
            prod_bh.start(broker->endpoint, broker->pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle reporter;
            reporter.start(broker->endpoint, broker->pubkey,
                           "REPORT-" + channel, pylabhub::tests::role_keystore_name("REPORT-" + channel));

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
        {"REPORT-bogus"},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
            BrcHandle reporter;
            reporter.start(broker->endpoint, broker->pubkey, "REPORT-bogus", pylabhub::tests::role_keystore_name("REPORT-bogus"));

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

// RETIRED 2026-06-28 — body for `ClosingNotify_DeliveredToProducerAndConsumer`
// removed.  Contract (CHANNEL_CLOSING_NOTIFY fan-out to ALL channel
// members, triggered via in-process `broker.request_close_channel`)
// absorbed into task #225 `Pattern4ChannelNotifiesTest` (Pattern 4
// rung 8) per Rule 6 retirement.  Coverage continuity: this is the
// only test pinning the dual-receipt invariant; #225 description
// updated 2026-06-28 to explicitly carry forward.  See driver file's
// retirement doc-block for full reasoning.


// ============================================================================
// 3. Duplicate REG_REQ — SHM cardinality + schema hash conflict
// ============================================================================
// duplicate_reg_shm_cardinality + duplicate_reg_different_schema_hash
// MIGRATED to tests/test_layer3_pattern4/ (task #54 Round 1).

// ============================================================================
// 4. HEARTBEAT_NOTIFY — PendingReady → Ready + wire payload + keying
// ============================================================================
// heartbeat_transitions_to_ready + heartbeat_keying_producer_vs_consumer_distinct_rows
// stay here — they inspect in-process broker HubState (channel-snapshot
// observable state; RolePresence rows) that has no wire equivalent.
// Round 3 of the sweep adds their RATIONALE blocks per Path 1.
// heartbeat_wire_payload_includes_uid_and_role_type is a Round-1
// Batch-D candidate (broker-log observation via expect_log).

int heartbeat_transitions_to_ready()
{
    return run_with_host(
        "broker_protocol::heartbeat_transitions_to_ready",
        {"prod." + pid_chan("proto.heartbeat.ready")},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
            const std::string channel = pid_chan("proto.heartbeat.ready");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(broker->endpoint, broker->pubkey, uid, pylabhub::tests::role_keystore_name(uid));
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
        {"prod." + pid_chan("proto.heartbeat.wire.uid")},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &log_cap) {
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
            bh.start(broker->endpoint, broker->pubkey, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value()) << "REG_REQ should succeed";

            bh.brc.send_heartbeat(channel, uid, role_type, {});

            // broker_proto 4→5 (R3.5b): HEARTBEAT_NOTIFY wire key renamed
            // `uid` → `role_uid`.  The broker's HEARTBEAT_NOTIFY log line
            // mirrors the wire field name.
            const std::string expected =
                "Broker: HEARTBEAT_NOTIFY channel='" + channel +
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
        {"prod." + pid_chan("proto.heartbeat.keying"), "cons." + pid_chan("proto.heartbeat.keying")},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.heartbeat.keying");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_h, cons_h;
            prod_h.start(broker->endpoint, broker->pubkey, prod_uid, pylabhub::tests::role_keystore_name(prod_uid));
            cons_h.start(broker->endpoint, broker->pubkey, cons_uid, pylabhub::tests::role_keystore_name(cons_uid));

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
// role_presence_req_unknown_uid, role_info_req_unknown_uid,
// role_presence_req_producer_uid, role_presence_req_consumer_uid, and
// role_info_req_with_inbox MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (task #54 Round 1 — HubHostBrokerHandle antipattern sweep).

// ============================================================================
// 6. Transport arbitration
// ============================================================================
// transport_mismatch_shm_producer_zmq_consumer,
// transport_match_shm_consumer_shm_producer, transport_match_no_driver_field
// MIGRATED to tests/test_layer3_pattern4/ (task #54 Round 1).

// ============================================================================
// 7. REG_ACK / CONSUMER_REG_ACK heartbeat-negotiation block
// ============================================================================
// reg_ack_contains_heartbeat_block_defaults,
// reg_ack_heartbeat_block_honors_custom_config (broker "hb_custom"
// profile), consumer_reg_ack_contains_heartbeat_block MIGRATED to
// tests/test_layer3_pattern4/ (task #54 Round 1).

// ============================================================================
// 8. CHANNEL_BROADCAST_SEND_NOTIFY — fan-out to producer + ALL consumers
// ============================================================================

int broadcast_fan_out_delivered_to_producer_and_consumers()
{
    return run_with_host(
        "broker_protocol::broadcast_fan_out_delivered_to_producer_and_consumers",
        {"prod." + pid_chan("proto.bcast.fanout"), "cons.first." + pid_chan("proto.bcast.fanout"), "cons.second." + pid_chan("proto.bcast.fanout"), "prod.broadcast.sender.pid" + std::to_string(static_cast<unsigned long>(::getpid()))},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
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
                    if (t == "CHANNEL_BROADCAST_DELIVER_NOTIFY") col->push(t, b);
                };
            };

            BrcHandle prod_bh;
            prod_bh.brc.on_notification(only_bcast(prod_evts));
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid, pylabhub::tests::role_keystore_name(prod_uid));
            // Fan-out — 1 producer, 2 consumers.  Explicit topology
            // required under the 2026-07-08 topology migration
            // (default is one-to-one; second consumer would trip
            // ONE_TO_ONE_CARDINALITY_VIOLATED).
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid, std::nullopt,
                              /*channel_topology=*/"fan-out"), 3000);
            ASSERT_TRUE(reg.has_value());
            // R6 producer-kLive gate — see HEP-CORE-0036 §5.2.
            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});

            BrcHandle cons1_bh;
            cons1_bh.brc.on_notification(only_bcast(cons1_evts));
            cons1_bh.start(broker->endpoint, broker->pubkey, cons1_uid, pylabhub::tests::role_keystore_name(cons1_uid));
            {
                auto opts = make_cons_opts(channel, cons1_uid, std::nullopt,
                                            /*channel_topology=*/"fan-out");
                opts["consumer_pid"] =
                    static_cast<uint64_t>(::getpid()) * 100u + 1u;
                ASSERT_TRUE(cons1_bh.brc.register_consumer(opts, 3000)
                                .has_value());
            }

            BrcHandle cons2_bh;
            cons2_bh.brc.on_notification(only_bcast(cons2_evts));
            cons2_bh.start(broker->endpoint, broker->pubkey, cons2_uid, pylabhub::tests::role_keystore_name(cons2_uid));
            {
                auto opts = make_cons_opts(channel, cons2_uid, std::nullopt,
                                            /*channel_topology=*/"fan-out");
                opts["consumer_pid"] =
                    static_cast<uint64_t>(::getpid()) * 100u + 2u;
                ASSERT_TRUE(cons2_bh.brc.register_consumer(opts, 3000)
                                .has_value());
            }

            BrcHandle sender_bh;
            auto sender_evts = std::make_shared<EventCollector>();
            sender_bh.brc.on_notification(only_bcast(sender_evts));
            sender_bh.start(broker->endpoint, broker->pubkey, send_uid, pylabhub::tests::role_keystore_name(send_uid));

            sender_bh.brc.send_broadcast(channel, send_uid,
                                          "hello-fan-out", "");

            ASSERT_TRUE(prod_evts->wait_for(1, 5000))
                << "producer did not receive CHANNEL_BROADCAST_DELIVER_NOTIFY";
            ASSERT_TRUE(cons1_evts->wait_for(1, 5000))
                << "cons1 did not receive CHANNEL_BROADCAST_DELIVER_NOTIFY";
            ASSERT_TRUE(cons2_evts->wait_for(1, 5000))
                << "cons2 did not receive CHANNEL_BROADCAST_DELIVER_NOTIFY";

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
        {"prod." + pid_chan("proto.bcast.payload"), "cons." + pid_chan("proto.bcast.payload"), "ext.bcast.payload.uid00000077"},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.bcast.payload");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;
            const std::string send_uid = "ext.bcast.payload.uid00000077";
            const std::string msg      = "payload-test";
            const std::string data     =
                R"({"k":"v","n":42,"arr":[1,2,3]})";

            auto cons_evts = std::make_shared<EventCollector>();

            BrcHandle prod_bh;
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid, pylabhub::tests::role_keystore_name(prod_uid));
            ASSERT_TRUE(prod_bh.brc.register_channel(
                            make_reg_opts(channel, prod_uid), 3000)
                            .has_value());
            // R6 producer-kLive gate — see HEP-CORE-0036 §5.2.
            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});

            BrcHandle cons_bh;
            cons_bh.brc.on_notification(
                [cons_evts](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_DELIVER_NOTIFY")
                        cons_evts->push(t, b);
                });
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid, pylabhub::tests::role_keystore_name(cons_uid));
            ASSERT_TRUE(cons_bh.brc.register_consumer(
                            make_cons_opts(channel, cons_uid), 3000)
                            .has_value());

            BrcHandle sender_bh;
            sender_bh.start(broker->endpoint, broker->pubkey, send_uid, pylabhub::tests::role_keystore_name(send_uid));
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
        {"prod." + pid_chan("proto.bcast.other"), "cons." + pid_chan("proto.bcast.other"), "ext.bcast.unknown.uid00000088"},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
            const std::string channel  = pid_chan("proto.bcast.unknown");
            const std::string send_uid = "ext.bcast.unknown.uid00000088";

            const std::string other_ch  = pid_chan("proto.bcast.other");
            const std::string other_prd = "prod." + other_ch;
            const std::string spec_uid  = "cons." + other_ch;
            auto spec_evts = std::make_shared<EventCollector>();

            BrcHandle other_prod;
            other_prod.start(broker->endpoint, broker->pubkey, other_prd, pylabhub::tests::role_keystore_name(other_prd));
            ASSERT_TRUE(other_prod.brc.register_channel(
                            make_reg_opts(other_ch, other_prd), 3000)
                            .has_value());

            BrcHandle spec_bh;
            spec_bh.brc.on_notification(
                [spec_evts](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_DELIVER_NOTIFY")
                        spec_evts->push(t, b);
                });
            spec_bh.start(broker->endpoint, broker->pubkey, spec_uid, pylabhub::tests::role_keystore_name(spec_uid));
            ASSERT_TRUE(spec_bh.brc.register_consumer(
                            make_cons_opts(other_ch, spec_uid), 3000)
                            .has_value());

            BrcHandle sender_bh;
            sender_bh.start(broker->endpoint, broker->pubkey, send_uid, pylabhub::tests::role_keystore_name(send_uid));
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
        {"prod." + pid_chan("proto.bcast.hubpath"), "cons." + pid_chan("proto.bcast.hubpath")},
        [](std::optional<HubHostHandle> &broker, pylabhub::tests::CurveSetup &curve, LogCaptureFixture &) {
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
                    if (t == "CHANNEL_BROADCAST_DELIVER_NOTIFY")
                        prod_evts->push(t, b);
                });
            prod_bh.start(broker->endpoint, broker->pubkey, prod_uid, pylabhub::tests::role_keystore_name(prod_uid));
            ASSERT_TRUE(prod_bh.brc.register_channel(
                            make_reg_opts(channel, prod_uid), 3000)
                            .has_value());
            // R6 producer-kLive gate — see HEP-CORE-0036 §5.2.
            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});

            BrcHandle cons_bh;
            cons_bh.brc.on_notification(
                [cons_evts](const std::string &t, const json &b) {
                    if (t == "CHANNEL_BROADCAST_DELIVER_NOTIFY")
                        cons_evts->push(t, b);
                });
            cons_bh.start(broker->endpoint, broker->pubkey, cons_uid, pylabhub::tests::role_keystore_name(cons_uid));
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
//
// wire_conformance_reg_ack_shape, wire_conformance_consumer_reg_ack_shape,
// wire_conformance_role_info_ack_shape, and wire_conformance_band_ack_shapes
// MIGRATED to tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (task #54 Round 1 — HubHostBrokerHandle antipattern sweep).  The
// B1/audit-key discipline above is preserved verbatim in the Pattern 4
// successors via the same `pylabhub::tests::wire` helpers.

// Audit R3.6 (2026-05-17): `wire_conformance_channel_notify_req_federation_relay`
// retired.  The broker-side `handle_channel_notify_req` handler was
// deleted because federation peer-relay actually uses `HUB_RELAY_MSG`
// (broker↔broker) rather than `CHANNEL_NOTIFY_REQ`.  See
// `docs/code_review/REVIEW_Connection_Inbox_Band_2026-05-17.md` (R3.6)
// for the investigation.  Old clients sending CHANNEL_NOTIFY_REQ now
// receive UNKNOWN_MSG_TYPE via the standard dispatch fall-through.



// wire_conformance_band_corr_id_echo migrated to
// tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (task #54 Round 1 — HubHostBrokerHandle antipattern sweep).

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
                // closing_notify_delivered_to_producer_and_consumer
                // RETIRED 2026-06-28 → task #225 (Pattern 4 rung 8).
                // duplicate_reg_* MIGRATED to Pattern 4 (task #54 Round 1).
                if (sc == "heartbeat_transitions_to_ready")
                    return heartbeat_transitions_to_ready();
                if (sc == "heartbeat_wire_payload_includes_uid_and_role_type")
                    return heartbeat_wire_payload_includes_uid_and_role_type();
                if (sc == "heartbeat_keying_producer_vs_consumer_distinct_rows")
                    return heartbeat_keying_producer_vs_consumer_distinct_rows();
                // role_presence_req_* / role_info_req_* / transport_* /
                // reg_ack_* / consumer_reg_ack_contains_heartbeat_block
                // MIGRATED to Pattern 4 (task #54 Round 1).
                if (sc == "broadcast_fan_out_delivered_to_producer_and_consumers")
                    return broadcast_fan_out_delivered_to_producer_and_consumers();
                if (sc == "broadcast_fan_out_data_payload_round_trip")
                    return broadcast_fan_out_data_payload_round_trip();
                if (sc == "broadcast_unknown_channel_no_notify_delivered")
                    return broadcast_unknown_channel_no_notify_delivered();
                if (sc == "broadcast_fan_out_hub_queue_path_fans_out_same")
                    return broadcast_fan_out_hub_queue_path_fans_out_same();
                // Audit TR1 wire-conformance shape tests
                // (reg_ack / consumer_reg_ack / role_info_ack / band_ack)
                // + wire_conformance_band_corr_id_echo MIGRATED to Pattern 4
                // (task #54 Round 1).
                // R3.6 retired — handler deleted, no test path
                return -1;
            });
    }
} g_registrar;

} // namespace
