/**
 * @file datahub_metrics_workers.cpp
 * @brief Worker bodies for broker metrics-plane tests
 *        (HEP-CORE-0019 + HEP-CORE-0033 §10.3 unified query engine;
 *        Pattern 3).
 *
 * Bodies preserve:
 *   - `wait_for_metric(channel, pred, timeout)` `poll_until` helper —
 *     pins ordering deterministically (replaces the pre-2026-05-01
 *     `sleep_for + query + assert` Class B antipattern).
 *   - `raw_request(...)` raw-wire DEALER helper for the
 *     METRICS_REPORT_REQ retirement check.
 *   - Wave M1.4 + M2.5 G1 + M3 H34 contract pins (heartbeat-piggyback
 *     unified path; per-uid producer/consumer tree; multi-presence
 *     no-cross-attribution; retired-wire-path UNKNOWN_MSG_TYPE).
 *
 * Each TEST_F runs as a Pattern-3 subprocess against the real
 * `HubHost` composite (real BrokerService + real HubState + real
 * AdminService + ThreadManager-backed run-loop), wired through
 * `pylabhub::tests::start_hubhost_broker(overrides, curve)` from
 * `broker_test_harness.h`.  CURVE identities are seeded via
 * `seed_curve_identities()` per HEP-CORE-0040 §172 use-not-export.
 * Per the test-design principle in
 * `feedback_test_layering_and_no_mocks.md`, regressions in HubHost's
 * threading / lifecycle / state-ownership wiring are actually caught.
 *
 * Module surface: Logger + FileLock + JsonConfig + SecureSubsystem +
 * ZMQContext.  FileLock + JsonConfig are needed because the
 * canonical harness reads hub.json via the JsonConfig module (which
 * uses FileLock) when emitting the temp hub directory.  Matches the
 * broker_schema / broker_admin / hub_lua_integration profile.
 *
 * @see HEP-CORE-0019 §2.3 (heartbeat-piggyback metrics, Phase 6 post-M1.4)
 * @see HEP-CORE-0033 §10.3 (unified query engine)
 * @see HEP-CORE-0033 §G2.2.0b (role_uid format invariant)
 * @see HEP-CORE-0036 §5.2 R6 (producer-kLive gate before consumer reg)
 *
 * ── RATIONALE — HubHostBrokerHandle sweep disposition (task #52 Round 3) ─────
 * Every worker in this file KEEPS the in-process broker.  The storage/wire
 * metrics tests already migrated to Pattern 4
 * (`test_pattern4_metrics.cpp` — HeartbeatMetricsStored trace + the
 * METRICS_REPORT_REQ→UNKNOWN_MSG_TYPE retirement check).  What remains are
 * the QUERY-ENGINE tests, which call `svc.query_metrics(...)` /
 * `svc.query_metrics_json_str(...)` directly.
 *   PURPOSE: pin the unified query engine's output/filter semantics
 *     (HEP-CORE-0033 §10.3) — category filtering, per-uid producer/consumer
 *     trees, `_collected_at` stamping, filter echo, multi-presence
 *     no-cross-attribution, and the all-channels-iterates-HubState-snapshot
 *     invariant.
 *   WHY IN-PROCESS BROKER: there is NO metrics wire query — the legacy
 *     `METRICS_REPORT_REQ` path is retired (UNKNOWN_MSG_TYPE) and this fixture
 *     disables the admin plane (`{"admin",{"enabled",false}}`).  The only
 *     readout of the query engine is the in-process `BrokerService` accessor;
 *     the aggregate query-response JSON maps to no wire ACK field and no log
 *     line.
 *   WHY NOT THE SINGLE-PUMPER ANTIPATTERN: one `HubHost` broker = one ZAP
 *     pump.  The metrics-generating clients are bare `BrcHandle`s (DEALER, no
 *     ZAP pump), so HEP-CORE-0036 §7.4 does not apply.
 *   FUTURE: if an admin/metrics wire query is added (or the `METRICS_REQ`
 *     surface materializes), these re-home to Pattern 4 like the storage
 *     tests did.  Tracked in TESTING_TODO Round 3.
 */

#include "datahub_metrics_workers.h"

#include "broker_test_harness.h"
#include "curve_test_setup.h"
#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_sync_utils.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_metrics_filter.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/wire_adapter.hpp"
#include "utils/wire_envelope.hpp"

#include <gtest/gtest.h>
#include <cppzmq/zmq_addon.hpp>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using pylabhub::tests::CurveSetup;
using pylabhub::tests::HubHostBrokerHandle;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::poll_until;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;
using namespace pylabhub::broker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace datahub_metrics
{

namespace
{

// ── Canonical HubHost broker startup (matches plh_hub binary path) ─────────
//
// Strict-CURVE migration (#154 AUTH-6 C5, 2026-06-30): switched from the
// raw `HubConfig::load_from_directory(make_test_hub_dir())` shape to
// `pylabhub::tests::start_hubhost_broker(overrides, curve)`.  The shared
// harness wires the temp hub_dir + KnownRolesStore + KeyStore seeding
// per HEP-CORE-0040 §172 so the broker admits each role uid declared
// in the per-test `role_uids` list.

json hub_overrides_baseline()
{
    return json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin", {{"enabled", false}}},
        {"script", {{"path", ""}}},
    };
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

/// Module list for every worker in this TU.  HubConfig load +
/// HubHost startup transitively require FileLock + JsonConfig.
#define PLH_METRICS_MODS                                                                           \
    Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),                                  \
        JsonConfig::GetLifecycleModule(),                                                          \
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),                          \
        pylabhub::hub::GetZMQContextModule()

/// Per-worker fixture: install LogCaptureFixture, seed a CurveKeyStore
/// for `role_uids`, spin up a HubHost via the canonical harness, run
/// the body with refs to the broker.  Uninstall + log assertion at end.
///
/// Body signature: `(const std::string& ep, const std::string& pk,
///                   BrokerService& svc)` — same shape as the pre-
/// migration template; only the wrapper's setup path changed (now
/// goes through the canonical CurveSetup + KeyStore fixture + harness).
///
/// `role_uids` is the set of role uids the body will register.  Each
/// is seeded into the per-process KeyStore as `role.<uid>` (canonical
/// name) and pushed onto the broker's KnownRolesStore via
/// `start_hubhost_broker`.  Body sites call BRC via
/// `bh.start(ep, pk, uid, pylabhub::tests::role_keystore_name(uid))`.
template <typename Body>
int run_with_broker(std::string_view worker_name, std::vector<std::string> role_uids, Body &&body,
                    std::vector<std::string> expect_log_warns = {})
{
    return run_gtest_worker(
        [role_uids = std::move(role_uids), body = std::forward<Body>(body),
         expect_log_warns = std::move(expect_log_warns)]() mutable
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            // Baseline noise allow-list (post-strict-CURVE harness).
            // Patterns are pinned to the FULL message body
            // (`data_block_recovery.cpp:39`, `broker_service.cpp:3301`)
            // so a future regression that emits a DIFFERENT error
            // string at the same call site, or the same string from a
            // new site, surfaces as a test failure instead of being
            // silently swallowed.  See #308 for the source-side fix
            // (gate the diagnostic at the emitter so this allow-list
            // can retire).
            //   - SHM recovery probes the test's fresh channel name
            //     before producer reg; channel doesn't exist yet so
            //     `open()` fails.  Benign.
            //   - HEARTBEAT_NOTIFY from a CONSUMER carries no
            //     producer_pid; broker logs at ERROR for diagnostics
            //     but accepts the heartbeat.  Benign.
            log_cap.ExpectLogError("recovery: Failed to open '");
            log_cap.ExpectLogError("' missing or zero producer_pid");
            for (auto &w : expect_log_warns)
                log_cap.ExpectLogWarn(w);

            auto curve = pylabhub::tests::make_curve_setup(role_uids);
            pylabhub::tests::seed_role_identities(curve);
            auto broker = pylabhub::tests::start_hubhost_broker(hub_overrides_baseline(), curve);
            ASSERT_TRUE(broker.host && broker.host->is_running());

            body(broker.endpoint, broker.pubkey, broker.service());

            broker.stop_and_join();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        std::string(worker_name).c_str(), PLH_METRICS_MODS);
}

/// Adapter: query metrics for a single channel (empty = all).
json query_metrics_single(BrokerService &svc, const std::string &channel = {})
{
    return json::parse(svc.query_metrics_json_str(channel));
}

/// `poll_until`-based wait that re-queries the broker until @p pred
/// returns true or the deadline elapses.  Replaces the pre-2026-05-01
/// Class B ordering antipattern of `sleep_for(N ms); query; assert`.
/// Polls every 5 ms (cheap in-process call).

} // namespace

// ─── Test #1: HeartbeatMetrics_StoredByBroker ──────────────────────────────

// ─── Test #2: ConsumerHeartbeatMetrics_StoredByBroker ──────────────────────

// ─── Test #3: QueryMetrics_UnknownChannel_ReturnsEmpty ─────────────────────

int query_metrics_unknown_channel_returns_empty()
{
    return run_with_broker("datahub_metrics::query_metrics_unknown_channel_returns_empty", {},
                           [](const std::string &, const std::string &, BrokerService &svc)
                           {
                               auto result = query_metrics_single(svc, pid_chan("metrics.unknown"));
                               EXPECT_EQ(result.value("status", ""), "success");
                               ASSERT_TRUE(result.contains("metrics"));
                               EXPECT_TRUE(result["metrics"].empty());
                           });
}

// ─── Test #4: QueryMetrics_AllChannels ─────────────────────────────────────

int query_metrics_all_channels()
{
    const std::string channel = pid_chan("metrics.all");
    const std::string uid = "prod." + channel;
    return run_with_broker(
        "datahub_metrics::query_metrics_all_channels", {uid},
        [channel, uid](const std::string &ep, const std::string &pk, BrokerService &svc)
        {
            pylabhub::tests::BrcHandle bh;
            bh.start(ep, pk, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(pylabhub::tests::make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["test_field"] = 99;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            ASSERT_TRUE(poll_until(
                [&]
                {
                    auto r = query_metrics_single(svc);
                    return r.contains("channels") && r["channels"].contains(channel);
                },
                std::chrono::seconds(2)))
                << "channel '" << channel
                << "' did not appear in all-channels query within 2s "
                   "after heartbeat";

            auto result = query_metrics_single(svc);
            ASSERT_TRUE(result.contains("channels"));
            ASSERT_TRUE(result["channels"].contains(channel));

            bh.stop();
        });
}

// ─── Test #5: HeartbeatNoMetrics_BackwardCompat ────────────────────────────

// ─── Test #6: MetricsUpdate_OverwriteOnHeartbeat ───────────────────────────

// ─── Test #7: ProducerPID_InQueryResult ────────────────────────────────────

// ─── Unified query engine — HEP-CORE-0033 §10.3 ────────────────────────────

int query_engine_empty_filter_all_categories_present()
{
    const std::string channel = pid_chan("query.empty.all");
    const std::string uid = "prod." + channel;
    return run_with_broker(
        "datahub_metrics::query_engine_empty_filter_all_categories_present", {uid},
        [channel, uid](const std::string &ep, const std::string &pk, BrokerService &svc)
        {
            pylabhub::tests::BrcHandle bh;
            bh.start(ep, pk, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(pylabhub::tests::make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            pylabhub::hub::MetricsFilter f;
            json result = svc.query_metrics(f);

            EXPECT_EQ(result.value("status", ""), "success");
            EXPECT_TRUE(result.contains("queried_at"));
            EXPECT_FALSE(result.value("queried_at", "").empty());
            EXPECT_TRUE(result.contains("filter"));
            EXPECT_TRUE(result.contains("channels"));
            EXPECT_TRUE(result.contains("roles"));
            EXPECT_TRUE(result.contains("bands"));
            EXPECT_TRUE(result.contains("peers"));
            EXPECT_TRUE(result.contains("broker"));
            EXPECT_TRUE(result.contains("shm"));
            EXPECT_TRUE(result.contains("schemas"));

            bh.stop();
        });
}

int query_engine_category_filter_only_broker()
{
    return run_with_broker("datahub_metrics::query_engine_category_filter_only_broker", {},
                           [](const std::string &, const std::string &, BrokerService &svc)
                           {
                               pylabhub::hub::MetricsFilter f;
                               f.categories.insert(pylabhub::hub::metrics_category::kBroker);
                               json result = svc.query_metrics(f);

                               EXPECT_EQ(result.value("status", ""), "success");
                               EXPECT_TRUE(result.contains("broker"));
                               EXPECT_FALSE(result.contains("channels"));
                               EXPECT_FALSE(result.contains("roles"));
                               EXPECT_FALSE(result.contains("bands"));
                               EXPECT_FALSE(result.contains("peers"));
                               EXPECT_FALSE(result.contains("shm"));
                               EXPECT_FALSE(result.contains("schemas"));

                               EXPECT_TRUE(result["broker"].contains("_collected_at"));
                           });
}

int query_engine_channel_identity_filter()
{
    const std::string ch1 = pid_chan("query.identity.A");
    const std::string ch2 = pid_chan("query.identity.B");
    const std::string uid1 = "prod." + ch1;
    const std::string uid2 = "prod." + ch2;
    return run_with_broker(
        "datahub_metrics::query_engine_channel_identity_filter", {uid1, uid2},
        [ch1, ch2, uid1, uid2](const std::string &ep, const std::string &pk, BrokerService &svc)
        {
            pylabhub::tests::BrcHandle b1;
            b1.start(ep, pk, uid1, pylabhub::tests::role_keystore_name(uid1));
            auto r1 = b1.brc.register_channel(pylabhub::tests::make_reg_opts(ch1, uid1), 3000);
            ASSERT_TRUE(r1.has_value());

            pylabhub::tests::BrcHandle b2;
            b2.start(ep, pk, uid2, pylabhub::tests::role_keystore_name(uid2));
            auto r2 = b2.brc.register_channel(pylabhub::tests::make_reg_opts(ch2, uid2), 3000);
            ASSERT_TRUE(r2.has_value());

            pylabhub::hub::MetricsFilter f;
            f.categories.insert(pylabhub::hub::metrics_category::kChannel);
            f.channels = {ch1};
            json result = svc.query_metrics(f);

            ASSERT_TRUE(result.contains("channels"));
            EXPECT_TRUE(result["channels"].contains(ch1));
            EXPECT_FALSE(result["channels"].contains(ch2));

            b1.stop();
            b2.stop();
        });
}

int query_engine_roles_carry_collected_at()
{
    const std::string channel = pid_chan("query.role.collected");
    const std::string uid = "prod." + channel;
    return run_with_broker(
        "datahub_metrics::query_engine_roles_carry_collected_at", {uid},
        [channel, uid](const std::string &ep, const std::string &pk, BrokerService &svc)
        {
            pylabhub::tests::BrcHandle bh;
            bh.start(ep, pk, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(pylabhub::tests::make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["iteration_count"] = 7;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            pylabhub::hub::MetricsFilter f;
            f.categories.insert(pylabhub::hub::metrics_category::kRole);
            f.roles = {uid};
            ASSERT_TRUE(poll_until(
                [&]
                {
                    json r = svc.query_metrics(f);
                    return r.contains("roles") && r["roles"].contains(uid) &&
                           !r["roles"][uid].value("_collected_at", "").empty();
                },
                std::chrono::seconds(2)))
                << "role record _collected_at did not populate within 2s";

            json result = svc.query_metrics(f);
            ASSERT_TRUE(result.contains("roles"));
            ASSERT_TRUE(result["roles"].contains(uid));
            const auto &r = result["roles"][uid];
            EXPECT_EQ(r.value("uid", ""), uid);
            EXPECT_EQ(r.value("short_tag", ""), "prod");
            EXPECT_TRUE(r.contains("_collected_at"));
            EXPECT_FALSE(r.value("_collected_at", "").empty());

            bh.stop();
        });
}

int query_engine_channels_have_producer_and_consumer_metrics()
{
    const std::string channel = pid_chan("query.channel.metrics");
    const std::string uid = "prod." + channel;
    return run_with_broker(
        "datahub_metrics::query_engine_channels_have_producer_and_consumer_metrics", {uid},
        [channel, uid](const std::string &ep, const std::string &pk, BrokerService &svc)
        {
            pylabhub::tests::BrcHandle bh;
            bh.start(ep, pk, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(pylabhub::tests::make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["iteration_count"] = 99;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            pylabhub::hub::MetricsFilter f;
            f.categories.insert(pylabhub::hub::metrics_category::kChannel);
            // Wave M2.5 G1: producer_metrics is a per-uid tree.
            ASSERT_TRUE(poll_until(
                [&]
                {
                    json r = svc.query_metrics(f);
                    return r.contains("channels") && r["channels"].contains(channel) &&
                           r["channels"][channel].contains("producer_metrics") &&
                           r["channels"][channel]["producer_metrics"].contains(uid) &&
                           r["channels"][channel]["producer_metrics"][uid].value("iteration_count",
                                                                                 0) == 99;
                },
                std::chrono::seconds(2)))
                << "channel producer_metrics iteration_count=99 not visible "
                   "within 2s";

            json result = svc.query_metrics(f);
            ASSERT_TRUE(result.contains("channels"));
            ASSERT_TRUE(result["channels"].contains(channel));
            const auto &c = result["channels"][channel];
            EXPECT_TRUE(c.contains("producer_metrics"));
            ASSERT_TRUE(c["producer_metrics"].contains(uid));
            EXPECT_EQ(c["producer_metrics"][uid].value("iteration_count", 0), 99);
            // Per-group freshness (HEP-CORE-0019 §4.2): each uid-keyed group is
            // one reporter, so it carries its OWN `_collected_at` — the time the
            // broker last received that role's heartbeat-borne metrics — distinct
            // from the channel-level `_collected_at` below.
            EXPECT_TRUE(c["producer_metrics"][uid].contains("_collected_at"))
                << "per-reporter metrics group must carry _collected_at; body="
                << c["producer_metrics"][uid].dump();
            EXPECT_FALSE(
                c["producer_metrics"][uid].value("_collected_at", std::string{}).empty())
                << "per-group _collected_at must be a non-empty timestamp";
            EXPECT_TRUE(c.contains("_collected_at"));

            bh.stop();
        });
}

// ─── Wave M2.5 G1 — multi-producer metrics isolation ───────────────────────

int query_engine_filter_echo()
{
    return run_with_broker("datahub_metrics::query_engine_filter_echo", {},
                           [](const std::string &, const std::string &, BrokerService &svc)
                           {
                               pylabhub::hub::MetricsFilter f;
                               f.categories.insert("role");
                               f.roles = {"prod.specific.uid12345678"};
                               json result = svc.query_metrics(f);

                               ASSERT_TRUE(result.contains("filter"));
                               const auto &echo = result["filter"];
                               EXPECT_TRUE(echo.contains("categories"));
                               ASSERT_TRUE(echo.contains("roles"));
                               ASSERT_EQ(echo["roles"].size(), 1u);
                               EXPECT_EQ(echo["roles"][0].get<std::string>(),
                                         "prod.specific.uid12345678");
                           });
}

// ─── Wave M1.4 (2026-05-11): wire-protocol retirement ──────────────────────

// ─── M1.4 + M3 H34: end-to-end multi-presence isolation ────────────────────

int multi_presence_end_to_end_no_cross_attribution()
{
    const std::string ch_a = pid_chan("metrics.multi.A");
    const std::string ch_b = pid_chan("metrics.multi.B");
    const std::string p_a = "prod.A." + ch_a;
    const std::string p_b = "prod.B." + ch_b;
    const std::string c_a = "cons.A." + ch_a;
    const std::string c_b = "cons.B." + ch_b;
    return run_with_broker(
        "datahub_metrics::multi_presence_end_to_end_no_cross_attribution", {p_a, p_b, c_a, c_b},
        [ch_a, ch_b, p_a, p_b, c_a, c_b](const std::string &ep, const std::string &pk,
                                         BrokerService &svc)
        {
            pylabhub::tests::BrcHandle prod_a_bh, prod_b_bh;
            prod_a_bh.start(ep, pk, p_a, pylabhub::tests::role_keystore_name(p_a));
            prod_b_bh.start(ep, pk, p_b, pylabhub::tests::role_keystore_name(p_b));
            ASSERT_TRUE(
                prod_a_bh.brc.register_channel(pylabhub::tests::make_reg_opts(ch_a, p_a), 3000)
                    .has_value());
            ASSERT_TRUE(
                prod_b_bh.brc.register_channel(pylabhub::tests::make_reg_opts(ch_b, p_b), 3000)
                    .has_value());

            // HEP-CORE-0036 §5.2 R6 producer-kLive gate (see
            // consumer_heartbeat_metrics_stored_by_broker for the
            // rationale).  Drive both producers to kLive via a probing
            // heartbeat with a marker metric, then poll until the
            // broker has observed both before the consumers register.
            prod_a_bh.brc.send_heartbeat(ch_a, p_a, "producer", json{{"_ready", 1}});
            prod_b_bh.brc.send_heartbeat(ch_b, p_b, "producer", json{{"_ready", 1}});
            ASSERT_TRUE(poll_until(
                [&]
                {
                    auto r = query_metrics_single(svc);
                    if (!r.contains("channels"))
                        return false;
                    const auto &chans = r["channels"];
                    return chans.contains(ch_a) && chans.contains(ch_b) &&
                           chans[ch_a].contains("producers") && chans[ch_b].contains("producers") &&
                           chans[ch_a]["producers"].contains(p_a) &&
                           chans[ch_b]["producers"].contains(p_b) &&
                           chans[ch_a]["producers"][p_a].value("_ready", 0) == 1 &&
                           chans[ch_b]["producers"][p_b].value("_ready", 0) == 1;
                },
                std::chrono::seconds(2)))
                << "producers did not reach kLive within 2s";

            pylabhub::tests::BrcHandle cons_a_bh, cons_b_bh;
            cons_a_bh.start(ep, pk, c_a, pylabhub::tests::role_keystore_name(c_a));
            cons_b_bh.start(ep, pk, c_b, pylabhub::tests::role_keystore_name(c_b));
            ASSERT_TRUE(
                cons_a_bh.brc.register_consumer(pylabhub::tests::make_cons_opts(ch_a, c_a), 3000)
                    .has_value());
            ASSERT_TRUE(
                cons_b_bh.brc.register_consumer(pylabhub::tests::make_cons_opts(ch_b, c_b), 3000)
                    .has_value());

            // Distinct metrics — literal values make cross-attribution
            // impossible to miss in the assertions.
            prod_a_bh.brc.send_heartbeat(ch_a, p_a, "producer", json{{"tag", "P-A"}, {"v", 11}});
            prod_b_bh.brc.send_heartbeat(ch_b, p_b, "producer", json{{"tag", "P-B"}, {"v", 22}});
            cons_a_bh.brc.send_heartbeat(ch_a, c_a, "consumer", json{{"tag", "C-A"}, {"v", 33}});
            cons_b_bh.brc.send_heartbeat(ch_b, c_b, "consumer", json{{"tag", "C-B"}, {"v", 44}});

            ASSERT_TRUE(poll_until(
                [&]
                {
                    auto r = query_metrics_single(svc);
                    if (!r.contains("channels"))
                        return false;
                    const auto &chans = r["channels"];
                    return chans.contains(ch_a) && chans.contains(ch_b) &&
                           chans[ch_a].contains("producers") && chans[ch_a].contains("consumers") &&
                           chans[ch_b].contains("producers") && chans[ch_b].contains("consumers") &&
                           chans[ch_a]["producers"].contains(p_a) &&
                           chans[ch_a]["consumers"].contains(c_a) &&
                           chans[ch_b]["producers"].contains(p_b) &&
                           chans[ch_b]["consumers"].contains(c_b);
                },
                std::chrono::seconds(2)))
                << "Not all four metrics rows appeared within 2 s";

            auto r = query_metrics_single(svc);
            const auto &cha = r["channels"][ch_a];
            const auto &chb = r["channels"][ch_b];

            // Channel A: P-A on producer row, C-A on consumer row.
            EXPECT_EQ(cha["producers"][p_a]["tag"], "P-A");
            EXPECT_EQ(cha["producers"][p_a]["v"], 11);
            EXPECT_EQ(cha["consumers"][c_a]["tag"], "C-A");
            EXPECT_EQ(cha["consumers"][c_a]["v"], 33);
            EXPECT_FALSE(cha["producers"][p_a].contains("tag") &&
                         cha["producers"][p_a]["tag"] == "C-A")
                << "Sanity: producer P-A row must NOT hold C-A's tag "
                   "(H34-root)";

            // Channel B: same isolation property.
            EXPECT_EQ(chb["producers"][p_b]["tag"], "P-B");
            EXPECT_EQ(chb["producers"][p_b]["v"], 22);
            EXPECT_EQ(chb["consumers"][c_b]["tag"], "C-B");
            EXPECT_EQ(chb["consumers"][c_b]["v"], 44);

            // Cross-channel isolation.
            EXPECT_FALSE(cha["producers"].contains(p_b))
                << "Channel A must NOT show channel B's producer";
            EXPECT_FALSE(cha["consumers"].contains(c_b))
                << "Channel A must NOT show channel B's consumer";
            EXPECT_FALSE(chb["producers"].contains(p_a));
            EXPECT_FALSE(chb["consumers"].contains(c_a));

            prod_a_bh.stop();
            prod_b_bh.stop();
            cons_a_bh.stop();
            cons_b_bh.stop();
        });
}

int all_channels_includes_channels_without_metrics()
{
    // M1.4 behavioral change: pre-fix `query_metrics()` (all-
    // channels) iterated `metrics_store_` (only channels with
    // reports); post-fix iterates HubState's
    // `snapshot().channels` (ALL registered channels).
    const std::string channel_with = pid_chan("metrics.has.metrics");
    const std::string channel_without = pid_chan("metrics.no.metrics");
    const std::string uid_with = "prod." + channel_with;
    const std::string uid_without = "prod." + channel_without;
    return run_with_broker(
        "datahub_metrics::all_channels_includes_channels_without_metrics", {uid_with, uid_without},
        [channel_with, channel_without, uid_with,
         uid_without](const std::string &ep, const std::string &pk, BrokerService &svc)
        {
            pylabhub::tests::BrcHandle bh1, bh2;
            bh1.start(ep, pk, uid_with, pylabhub::tests::role_keystore_name(uid_with));
            bh2.start(ep, pk, uid_without, pylabhub::tests::role_keystore_name(uid_without));
            ASSERT_TRUE(
                bh1.brc
                    .register_channel(pylabhub::tests::make_reg_opts(channel_with, uid_with), 3000)
                    .has_value());
            ASSERT_TRUE(bh2.brc
                            .register_channel(
                                pylabhub::tests::make_reg_opts(channel_without, uid_without), 3000)
                            .has_value());

            bh1.brc.send_heartbeat(channel_with, uid_with, "producer", json{{"data_point", 7}});

            ASSERT_TRUE(poll_until(
                [&]
                {
                    auto r = query_metrics_single(svc);
                    return r.contains("channels") && r["channels"].contains(channel_with) &&
                           r["channels"][channel_with].contains("producers");
                },
                std::chrono::seconds(2)))
                << "channel_with did not record its metrics within 2s";

            auto r = query_metrics_single(svc);
            ASSERT_TRUE(r.contains("channels"));

            ASSERT_TRUE(r["channels"].contains(channel_with))
                << "Channel with metrics appears (was true pre-M1.4 too)";
            ASSERT_TRUE(r["channels"].contains(channel_without))
                << "M1.4 contract: channel WITHOUT metrics also appears in "
                   "all-channels query (with empty metrics object).";

            EXPECT_EQ(r["channels"][channel_with]["producers"][uid_with]["data_point"], 7);

            auto &cw = r["channels"][channel_without];
            EXPECT_FALSE(cw.contains("producers")) << "Empty channel has no producers metrics";
            EXPECT_FALSE(cw.contains("consumers"));

            bh1.stop();
            bh2.stop();
        });
}

} // namespace datahub_metrics
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct DatahubMetricsRegistrar
{
    DatahubMetricsRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "datahub_metrics")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::datahub_metrics;

                // heartbeat_metrics_stored_by_broker + consumer variant
                // MIGRATED to Pattern 4 (task #52 Round 3 — broker
                // HeartbeatMetricsStored trace).
                if (sc == "query_metrics_unknown_channel_returns_empty")
                    return query_metrics_unknown_channel_returns_empty();
                if (sc == "query_metrics_all_channels")
                    return query_metrics_all_channels();
                // heartbeat_no_metrics_backward_compat +
                // metrics_update_overwrite_on_heartbeat +
                // producer_pid_in_query_result MIGRATED to Pattern 4
                // (task #52 Round 3).
                if (sc == "query_engine_empty_filter_all_categories_present")
                    return query_engine_empty_filter_all_categories_present();
                if (sc == "query_engine_category_filter_only_broker")
                    return query_engine_category_filter_only_broker();
                if (sc == "query_engine_channel_identity_filter")
                    return query_engine_channel_identity_filter();
                if (sc == "query_engine_roles_carry_collected_at")
                    return query_engine_roles_carry_collected_at();
                if (sc == "query_engine_channels_have_producer_and_consumer_"
                          "metrics")
                    return query_engine_channels_have_producer_and_consumer_metrics();
                // fan_in_two_producers_metrics_do_not_overwrite +
                // old_metrics_report_req_gets_unknown_msg_type MIGRATED to
                // Pattern 4 (task #52 Round 3).
                if (sc == "query_engine_filter_echo")
                    return query_engine_filter_echo();
                if (sc == "multi_presence_end_to_end_no_cross_attribution")
                    return multi_presence_end_to_end_no_cross_attribution();
                if (sc == "all_channels_includes_channels_without_metrics")
                    return all_channels_includes_channels_without_metrics();
                return -1;
            });
    }
} g_registrar;

} // namespace
