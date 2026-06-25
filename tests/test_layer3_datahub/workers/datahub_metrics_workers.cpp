/**
 * @file datahub_metrics_workers.cpp
 * @brief Worker bodies for broker metrics-plane tests
 *        (HEP-CORE-0019 + HEP-CORE-0033 §10.3 unified query engine;
 *        Pattern 3).
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Bodies transplanted verbatim,
 * preserving:
 *   - `wait_for_metric(channel, pred, timeout)` poll_until helper
 *     (replaces the pre-2026-05-01 `sleep_for + query + assert`
 *     Class B ordering — preserves the audit-§1.3 rationale inline).
 *   - `raw_request(...)` raw-wire DEALER helper for test #15's
 *     METRICS_REPORT_REQ retirement check.
 *   - Wave M1.4 + M2.5 G1 + M3 H34 contract pins (heartbeat-piggyback
 *     unified path; per-uid producer/consumer tree; multi-presence
 *     no-cross-attribution; retired-wire-path UNKNOWN_MSG_TYPE).
 *
 * **Real-production-wiring refactor (2026-05-14)**: switched from
 * the legacy `LocalBrokerHandle` (raw HubState + bare BrokerService
 * + raw `std::thread`) to real `HubHost` via
 * `HubConfig::load_from_directory(...)`.  Per the test-design
 * principle in `feedback_test_layering_and_no_mocks.md`: L3 broker
 * tests MUST run against the real HubHost composite (real
 * BrokerService + real HubState + real AdminService +
 * ThreadManager-backed broker run-loop) so regressions in HubHost's
 * threading / lifecycle / state-ownership wiring are actually
 * caught.  Same refactor shape as zmq_endpoint_registry commit
 * `07382d0`.
 *
 * Module surface: Logger + FileLock + JsonConfig + CryptoUtils +
 * ZMQContext (5 modules).  FileLock + JsonConfig added because
 * `HubConfig::load_from_directory` reads hub.json via the
 * JsonConfig module (which uses FileLock).  Matches the
 * broker_schema / broker_admin / hub_lua_integration profile.
 *
 * @see HEP-CORE-0019 §2.3 (heartbeat-piggyback metrics, Phase 6 post-M1.4)
 * @see HEP-CORE-0033 §10.3 (unified query engine)
 * @see HEP-CORE-0033 §G2.2.0b (role_uid format invariant)
 */

#include "datahub_metrics_workers.h"

#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_sync_utils.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_metrics_filter.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <cppzmq/zmq_addon.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::poll_until;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::HubDirectory;
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

// ── Real-HubHost broker startup (matches plh_hub binary path) ──────────────

fs::path make_test_hub_dir()
{
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_metrics_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    HubDirectory::init_directory(dir, "MetricsTestHub");

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
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
    return dir;
}

struct BrcHandle
{
    pylabhub::hub::BrokerRequestComm brc;
    std::atomic<bool>                running{true};
    std::thread                      thread;

    void start(const std::string &ep, const std::string &pk,
               const std::string &uid)
    {
        pylabhub::hub::BrokerRequestComm::Config cfg;
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

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

/// Raw-wire DEALER helper for test #15 — sends a literal msg_type
/// (METRICS_REPORT_REQ) to verify the retired wire path returns
/// UNKNOWN_MSG_TYPE.  Mirrors the pattern in
/// `datahub_broker_workers.cpp::raw_req`.  Single-shot, two-frame
/// [type, payload].  Returns the parsed reply body, or null JSON on
/// timeout.
json raw_request(const std::string &endpoint,
                 const std::string &server_pubkey,
                 const std::string &msg_type,
                 const json        &payload,
                 int                timeout_ms = 2000)
{
    zmq::context_t ctx(1);
    zmq::socket_t  dealer(ctx, zmq::socket_type::dealer);

    if (server_pubkey.size() == 40)
    {
        std::array<char, 41> client_pub{};
        std::array<char, 41> client_sec{};
        if (zmq_curve_keypair(client_pub.data(), client_sec.data()) != 0)
            return {};
        dealer.set(zmq::sockopt::curve_serverkey, server_pubkey);
        dealer.set(zmq::sockopt::curve_publickey,
                   std::string(client_pub.data(), 40));
        dealer.set(zmq::sockopt::curve_secretkey,
                   std::string(client_sec.data(), 40));
    }

    dealer.connect(endpoint);

    static constexpr char kCtrl = 'C';
    const std::string     body  = payload.dump();
    std::vector<zmq::const_buffer> frames = {zmq::buffer(&kCtrl, 1),
                                              zmq::buffer(msg_type),
                                              zmq::buffer(body)};
    if (!zmq::send_multipart(dealer, frames)) return {};

    std::vector<zmq::pollitem_t> items = {{dealer.handle(), 0, ZMQ_POLLIN, 0}};
    zmq::poll(items, std::chrono::milliseconds(timeout_ms));
    if ((items[0].revents & ZMQ_POLLIN) == 0) return {};  // timeout

    std::vector<zmq::message_t> recv_frames;
    if (!zmq::recv_multipart(dealer, std::back_inserter(recv_frames)))
        return {};
    if (recv_frames.size() < 2) return {};
    return json::parse(std::string(recv_frames.back().data<char>(),
                                    recv_frames.back().size()));
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

json make_cons_opts(const std::string &channel,
                    const std::string &consumer_uid)
{
    json opts;
    opts["channel_name"]  = channel;
    opts["role_uid"]  = consumer_uid;
    opts["role_name"] = "test_consumer";
    opts["consumer_pid"]  = ::getpid();
    return opts;
}

/// Module list for every worker in this TU.  Grown from the pre-
/// refactor 3-module list (Logger + Crypto + ZMQContext) because
/// HubConfig::load_from_directory + HubHost startup transitively
/// require FileLock + JsonConfig.
#define PLH_METRICS_MODS                                                       \
    Logger::GetLifecycleModule(),                                              \
    FileLock::GetLifecycleModule(),                                            \
    JsonConfig::GetLifecycleModule(),                                          \
    pylabhub::crypto::GetLifecycleModule(),                                    \
    pylabhub::hub::GetZMQContextModule()

/// Per-worker fixture: install LogCaptureFixture, spin up a real
/// HubHost via `HubConfig::load_from_directory(...)`, run the body
/// with refs to the broker.  Uninstall + log assertion at end.  Body
/// receives:
///   - `ep` / `pk`: broker endpoint + pubkey
///   - `svc`: BrokerService reference (for query_metrics / query_metrics_json_str)
template <typename Body>
int run_with_broker(std::string_view worker_name, Body &&body,
                    std::vector<std::string> expect_log_warns = {})
{
    return run_gtest_worker(
        [body = std::forward<Body>(body),
         expect_log_warns = std::move(expect_log_warns)]() mutable {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : expect_log_warns)
                log_cap.ExpectLogWarn(w);

            const fs::path dir = make_test_hub_dir();
            auto host = std::make_unique<HubHost>(
                HubConfig::load_from_directory(dir.string()));
            host->startup();
            ASSERT_TRUE(host->is_running());

            body(host->broker_endpoint(), host->broker_pubkey(),
                 host->broker());

            host.reset();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            std::error_code ec;
            fs::remove_all(dir, ec);
        },
        std::string(worker_name).c_str(),
        PLH_METRICS_MODS);
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
bool wait_for_metric(BrokerService &svc, const std::string &channel,
                     std::function<bool(const json &)> pred,
                     std::chrono::milliseconds timeout =
                         std::chrono::seconds(2))
{
    return poll_until([&] { return pred(query_metrics_single(svc, channel)); },
                       timeout, std::chrono::milliseconds(5));
}

} // namespace

// ─── Test #1: HeartbeatMetrics_StoredByBroker ──────────────────────────────

int heartbeat_metrics_stored_by_broker()
{
    return run_with_broker(
        "datahub_metrics::heartbeat_metrics_stored_by_broker",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("metrics.heartbeat.stored");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["iteration_count"] = 42;
            metrics["avg_period_us"]   = 1000;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            // Wave M2.5 G1 — per-uid producer tree.
            ASSERT_TRUE(wait_for_metric(svc, channel,
                [&uid](const json &r) {
                    return r.value("status", "") == "success"
                        && r.contains("metrics")
                        && r["metrics"].contains("producers")
                        && r["metrics"]["producers"].contains(uid)
                        && r["metrics"]["producers"][uid]
                               .value("iteration_count", 0) == 42;
                }))
                << "broker did not record producer iteration_count=42 "
                   "within 2s";

            auto result = query_metrics_single(svc, channel);
            ASSERT_EQ(result.value("status", ""), "success");
            ASSERT_TRUE(result.contains("metrics"));
            auto &m = result["metrics"];
            ASSERT_TRUE(m.contains("producers"));
            ASSERT_TRUE(m["producers"].contains(uid));
            EXPECT_EQ(m["producers"][uid].value("iteration_count", 0), 42);

            bh.stop();
        });
}

// ─── Test #2: ConsumerHeartbeatMetrics_StoredByBroker ──────────────────────

int consumer_heartbeat_metrics_stored_by_broker()
{
    return run_with_broker(
        "datahub_metrics::consumer_heartbeat_metrics_stored_by_broker",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel  = pid_chan("metrics.consumer.stored");
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_bh;
            prod_bh.start(ep, pk, prod_uid);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.start(ep, pk, cons_uid);
            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value());

            // M1.4: metrics piggyback on HEARTBEAT_REQ.
            json cons_metrics;
            cons_metrics["read_count"] = 100;
            cons_bh.brc.send_heartbeat(channel, cons_uid, "consumer",
                                        cons_metrics);

            ASSERT_TRUE(wait_for_metric(svc, channel,
                [&](const json &r) {
                    return r.contains("metrics")
                        && r["metrics"].contains("consumers")
                        && r["metrics"]["consumers"].contains(cons_uid)
                        && r["metrics"]["consumers"][cons_uid]
                               .value("read_count", 0) == 100;
                }))
                << "broker did not record consumer read_count=100 within 2s";

            auto result = query_metrics_single(svc, channel);
            ASSERT_TRUE(result.contains("metrics"));
            auto &m = result["metrics"];
            ASSERT_TRUE(m.contains("consumers"));
            ASSERT_TRUE(m["consumers"].contains(cons_uid));
            EXPECT_EQ(m["consumers"][cons_uid].value("read_count", 0), 100);

            cons_bh.stop();
            prod_bh.stop();
        });
}

// ─── Test #3: QueryMetrics_UnknownChannel_ReturnsEmpty ─────────────────────

int query_metrics_unknown_channel_returns_empty()
{
    return run_with_broker(
        "datahub_metrics::query_metrics_unknown_channel_returns_empty",
        [](const std::string &, const std::string &, BrokerService &svc) {
            auto result = query_metrics_single(svc, pid_chan("metrics.unknown"));
            EXPECT_EQ(result.value("status", ""), "success");
            ASSERT_TRUE(result.contains("metrics"));
            EXPECT_TRUE(result["metrics"].empty());
        });
}

// ─── Test #4: QueryMetrics_AllChannels ─────────────────────────────────────

int query_metrics_all_channels()
{
    return run_with_broker(
        "datahub_metrics::query_metrics_all_channels",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("metrics.all");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["test_field"] = 99;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            ASSERT_TRUE(poll_until([&] {
                auto r = query_metrics_single(svc);
                return r.contains("channels") && r["channels"].contains(channel);
            }, std::chrono::seconds(2)))
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

int heartbeat_no_metrics_backward_compat()
{
    return run_with_broker(
        "datahub_metrics::heartbeat_no_metrics_backward_compat",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("metrics.no.payload");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            auto result = query_metrics_single(svc, channel);
            EXPECT_EQ(result.value("status", ""), "success");

            bh.stop();
        });
}

// ─── Test #6: MetricsUpdate_OverwriteOnHeartbeat ───────────────────────────

int metrics_update_overwrite_on_heartbeat()
{
    return run_with_broker(
        "datahub_metrics::metrics_update_overwrite_on_heartbeat",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("metrics.overwrite");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            json m1;
            m1["iteration_count"] = 10;
            bh.brc.send_heartbeat(channel, uid, "producer", m1);
            ASSERT_TRUE(wait_for_metric(svc, channel,
                [&uid](const json &r) {
                    return r.contains("metrics")
                        && r["metrics"].contains("producers")
                        && r["metrics"]["producers"].contains(uid)
                        && r["metrics"]["producers"][uid]
                               .value("iteration_count", 0) == 10;
                }))
                << "first heartbeat (iteration_count=10) did not propagate "
                   "within 2s";

            json m2;
            m2["iteration_count"] = 20;
            bh.brc.send_heartbeat(channel, uid, "producer", m2);
            ASSERT_TRUE(wait_for_metric(svc, channel,
                [&uid](const json &r) {
                    return r.contains("metrics")
                        && r["metrics"].contains("producers")
                        && r["metrics"]["producers"].contains(uid)
                        && r["metrics"]["producers"][uid]
                               .value("iteration_count", 0) == 20;
                }))
                << "second heartbeat (iteration_count=20) did not overwrite "
                   "first within 2s";

            auto result = query_metrics_single(svc, channel);
            auto &m = result["metrics"];
            ASSERT_TRUE(m.contains("producers"));
            ASSERT_TRUE(m["producers"].contains(uid));
            EXPECT_EQ(m["producers"][uid].value("iteration_count", 0), 20);

            bh.stop();
        });
}

// ─── Test #7: ProducerPID_InQueryResult ────────────────────────────────────

int producer_pid_in_query_result()
{
    return run_with_broker(
        "datahub_metrics::producer_pid_in_query_result",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("metrics.pid");
            const std::string uid     = "prod." + channel;

            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["test"] = 1;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            ASSERT_TRUE(wait_for_metric(svc, channel,
                [&uid](const json &r) {
                    return r.contains("metrics")
                        && r["metrics"].contains("producers")
                        && r["metrics"]["producers"].contains(uid)
                        && r["metrics"]["producers"][uid]
                               .value("pid", 0) == ::getpid();
                }))
                << "producer PID did not appear in metrics within 2s";

            auto result = query_metrics_single(svc, channel);
            auto &m = result["metrics"];
            ASSERT_TRUE(m.contains("producers"));
            ASSERT_TRUE(m["producers"].contains(uid));
            EXPECT_EQ(m["producers"][uid].value("pid", 0), ::getpid());

            bh.stop();
        });
}

// ─── Unified query engine — HEP-CORE-0033 §10.3 ────────────────────────────

int query_engine_empty_filter_all_categories_present()
{
    return run_with_broker(
        "datahub_metrics::query_engine_empty_filter_all_categories_present",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("query.empty.all");
            const std::string uid     = "prod." + channel;
            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
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
    return run_with_broker(
        "datahub_metrics::query_engine_category_filter_only_broker",
        [](const std::string &, const std::string &, BrokerService &svc) {
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
    return run_with_broker(
        "datahub_metrics::query_engine_channel_identity_filter",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string ch1 = pid_chan("query.identity.A");
            const std::string ch2 = pid_chan("query.identity.B");

            BrcHandle b1; b1.start(ep, pk, "prod." + ch1);
            auto r1 = b1.brc.register_channel(make_reg_opts(ch1, "prod." + ch1),
                                               3000);
            ASSERT_TRUE(r1.has_value());

            BrcHandle b2; b2.start(ep, pk, "prod." + ch2);
            auto r2 = b2.brc.register_channel(make_reg_opts(ch2, "prod." + ch2),
                                               3000);
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
    return run_with_broker(
        "datahub_metrics::query_engine_roles_carry_collected_at",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("query.role.collected");
            const std::string uid     = "prod." + channel;
            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["iteration_count"] = 7;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            pylabhub::hub::MetricsFilter f;
            f.categories.insert(pylabhub::hub::metrics_category::kRole);
            f.roles = {uid};
            ASSERT_TRUE(poll_until([&] {
                json r = svc.query_metrics(f);
                return r.contains("roles") && r["roles"].contains(uid)
                    && !r["roles"][uid].value("_collected_at", "").empty();
            }, std::chrono::seconds(2)))
                << "role record _collected_at did not populate within 2s";

            json result = svc.query_metrics(f);
            ASSERT_TRUE(result.contains("roles"));
            ASSERT_TRUE(result["roles"].contains(uid));
            const auto &r = result["roles"][uid];
            EXPECT_EQ(r.value("uid", ""), uid);
            EXPECT_EQ(r.value("role_tag", ""), "prod");
            EXPECT_TRUE(r.contains("_collected_at"));
            EXPECT_FALSE(r.value("_collected_at", "").empty());

            bh.stop();
        });
}

int query_engine_channels_have_producer_and_consumer_metrics()
{
    return run_with_broker(
        "datahub_metrics::query_engine_channels_have_producer_and_consumer_metrics",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("query.channel.metrics");
            const std::string uid     = "prod." + channel;
            BrcHandle bh;
            bh.start(ep, pk, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                3000);
            ASSERT_TRUE(reg.has_value());

            json metrics;
            metrics["iteration_count"] = 99;
            bh.brc.send_heartbeat(channel, uid, "producer", metrics);

            pylabhub::hub::MetricsFilter f;
            f.categories.insert(pylabhub::hub::metrics_category::kChannel);
            // Wave M2.5 G1: producer_metrics is a per-uid tree.
            ASSERT_TRUE(poll_until([&] {
                json r = svc.query_metrics(f);
                return r.contains("channels") && r["channels"].contains(channel)
                    && r["channels"][channel].contains("producer_metrics")
                    && r["channels"][channel]["producer_metrics"]
                          .contains(uid)
                    && r["channels"][channel]["producer_metrics"][uid]
                          .value("iteration_count", 0) == 99;
            }, std::chrono::seconds(2)))
                << "channel producer_metrics iteration_count=99 not visible "
                   "within 2s";

            json result = svc.query_metrics(f);
            ASSERT_TRUE(result.contains("channels"));
            ASSERT_TRUE(result["channels"].contains(channel));
            const auto &c = result["channels"][channel];
            EXPECT_TRUE(c.contains("producer_metrics"));
            ASSERT_TRUE(c["producer_metrics"].contains(uid));
            EXPECT_EQ(c["producer_metrics"][uid].value("iteration_count", 0),
                      99);
            EXPECT_TRUE(c.contains("_collected_at"));

            bh.stop();
        });
}

// ─── Wave M2.5 G1 — multi-producer metrics isolation ───────────────────────

int fan_in_two_producers_metrics_do_not_overwrite()
{
    return run_with_broker(
        "datahub_metrics::fan_in_two_producers_metrics_do_not_overwrite",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string channel = pid_chan("metrics.fanin");
            // HEP-CORE-0033 §G2.2.0b: each tag.name.unique component
            // starts with a letter; pidNNN prefix keeps the last
            // component letter-led.
            const std::string uid_a   =
                "prod.fanin.a.pid" + std::to_string(::getpid());
            const std::string uid_b   =
                "prod.fanin.b.pid" + std::to_string(::getpid());

            BrcHandle bh_a, bh_b;
            bh_a.start(ep, pk, uid_a);
            bh_b.start(ep, pk, uid_b);

            // ZMQ transport — SHM forbids multi-producer
            // (HEP-CORE-0023 §2.1.1).
            auto opts_a = make_reg_opts(channel, uid_a);
            opts_a["data_transport"] = "zmq";
            auto reg_a = bh_a.brc.register_channel(opts_a, 3000);
            ASSERT_TRUE(reg_a.has_value()) << reg_a.value_or(json{}).dump();
            ASSERT_EQ(reg_a->value("status", ""), "success") << reg_a->dump();

            auto opts_b = make_reg_opts(channel, uid_b);
            opts_b["data_transport"] = "zmq";
            auto reg_b = bh_b.brc.register_channel(opts_b, 3000);
            ASSERT_TRUE(reg_b.has_value()) << reg_b.value_or(json{}).dump();
            ASSERT_EQ(reg_b->value("status", ""), "success") << reg_b->dump();

            json ma, mb;
            ma["iteration_count"] = 100;
            mb["iteration_count"] = 200;
            bh_a.brc.send_heartbeat(channel, uid_a, "producer", ma);
            bh_b.brc.send_heartbeat(channel, uid_b, "producer", mb);

            // Wait until BOTH producers' slots exist with expected values.
            ASSERT_TRUE(wait_for_metric(svc, channel,
                [&](const json &r) {
                    if (!r.contains("metrics")) return false;
                    const auto &m = r["metrics"];
                    if (!m.contains("producers")) return false;
                    const auto &p = m["producers"];
                    return p.contains(uid_a)
                        && p[uid_a].value("iteration_count", 0) == 100
                        && p.contains(uid_b)
                        && p[uid_b].value("iteration_count", 0) == 200;
                }))
                << "Both producers' metrics must coexist in the per-uid "
                   "tree (pre-G1 the second report overwrote the first); "
                   "within 2s";

            auto result = query_metrics_single(svc, channel);
            const auto &p = result["metrics"]["producers"];
            EXPECT_EQ(p[uid_a].value("iteration_count", 0), 100)
                << "Producer A's slot must NOT be overwritten by B's report";
            EXPECT_EQ(p[uid_b].value("iteration_count", 0), 200)
                << "Producer B's slot must NOT be overwritten by A's report";
            EXPECT_EQ(p[uid_a].value("pid", 0), ::getpid());
            EXPECT_EQ(p[uid_b].value("pid", 0), ::getpid());

            bh_b.stop();
            bh_a.stop();
        });
}

int query_engine_filter_echo()
{
    return run_with_broker(
        "datahub_metrics::query_engine_filter_echo",
        [](const std::string &, const std::string &, BrokerService &svc) {
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

int old_metrics_report_req_gets_unknown_msg_type()
{
    return run_with_broker(
        "datahub_metrics::old_metrics_report_req_gets_unknown_msg_type",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            (void)svc;  // svc unused; we send via raw_request below
            // M1.4 contract: METRICS_REPORT_REQ is RETIRED — broker
            // returns UNKNOWN_MSG_TYPE.  Sensitivity: a regression that
            // re-adds METRICS_REPORT_REQ to the dispatch table OR to
            // kFireAndForgetTypes would fail this test.

            const std::string channel = pid_chan("metrics.retired");
            const std::string uid     = "prod." + channel;

            // Pre-register the channel so the rejection is about
            // msg_type, not channel-not-found.
            BrcHandle bh;
            bh.start(ep, pk, uid);
            ASSERT_TRUE(bh.brc.register_channel(make_reg_opts(channel, uid),
                                                  3000)
                            .has_value());

            json payload;
            payload["channel_name"] = channel;
            payload["uid"]          = uid;
            payload["metrics"]      = json{{"legacy_field", 1}};

            json reply = raw_request(ep, pk, "METRICS_REPORT_REQ", payload);
            ASSERT_FALSE(reply.is_null())
                << "Expected an UNKNOWN_MSG_TYPE reply within 2 s; got "
                   "nothing (the broker may be silently dropping the "
                   "request rather than responding with an error)";
            EXPECT_EQ(reply.value("status", std::string{}), "error");
            EXPECT_EQ(reply.value("error_code", std::string{}),
                      "UNKNOWN_MSG_TYPE")
                << "Wire-protocol break must surface as UNKNOWN_MSG_TYPE per "
                   "broker_service.cpp dispatch fallback";

            bh.stop();
        },
        {"unknown msg_type 'METRICS_REPORT_REQ'"});
}

// ─── M1.4 + M3 H34: end-to-end multi-presence isolation ────────────────────

int multi_presence_end_to_end_no_cross_attribution()
{
    return run_with_broker(
        "datahub_metrics::multi_presence_end_to_end_no_cross_attribution",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            const std::string ch_a = pid_chan("metrics.multi.A");
            const std::string ch_b = pid_chan("metrics.multi.B");
            const std::string p_a  = "prod.A." + ch_a;
            const std::string p_b  = "prod.B." + ch_b;
            const std::string c_a  = "cons.A." + ch_a;
            const std::string c_b  = "cons.B." + ch_b;

            BrcHandle prod_a_bh, prod_b_bh;
            prod_a_bh.start(ep, pk, p_a);
            prod_b_bh.start(ep, pk, p_b);
            ASSERT_TRUE(prod_a_bh.brc.register_channel(
                            make_reg_opts(ch_a, p_a), 3000).has_value());
            ASSERT_TRUE(prod_b_bh.brc.register_channel(
                            make_reg_opts(ch_b, p_b), 3000).has_value());

            BrcHandle cons_a_bh, cons_b_bh;
            cons_a_bh.start(ep, pk, c_a);
            cons_b_bh.start(ep, pk, c_b);
            ASSERT_TRUE(cons_a_bh.brc.register_consumer(
                            make_cons_opts(ch_a, c_a), 3000).has_value());
            ASSERT_TRUE(cons_b_bh.brc.register_consumer(
                            make_cons_opts(ch_b, c_b), 3000).has_value());

            // Distinct metrics — literal values make cross-attribution
            // impossible to miss in the assertions.
            prod_a_bh.brc.send_heartbeat(ch_a, p_a, "producer",
                json{{"tag", "P-A"}, {"v", 11}});
            prod_b_bh.brc.send_heartbeat(ch_b, p_b, "producer",
                json{{"tag", "P-B"}, {"v", 22}});
            cons_a_bh.brc.send_heartbeat(ch_a, c_a, "consumer",
                json{{"tag", "C-A"}, {"v", 33}});
            cons_b_bh.brc.send_heartbeat(ch_b, c_b, "consumer",
                json{{"tag", "C-B"}, {"v", 44}});

            ASSERT_TRUE(poll_until([&] {
                auto r = query_metrics_single(svc);
                if (!r.contains("channels")) return false;
                const auto &chans = r["channels"];
                return chans.contains(ch_a) && chans.contains(ch_b)
                    && chans[ch_a].contains("producers")
                    && chans[ch_a].contains("consumers")
                    && chans[ch_b].contains("producers")
                    && chans[ch_b].contains("consumers")
                    && chans[ch_a]["producers"].contains(p_a)
                    && chans[ch_a]["consumers"].contains(c_a)
                    && chans[ch_b]["producers"].contains(p_b)
                    && chans[ch_b]["consumers"].contains(c_b);
            }, std::chrono::seconds(2)))
                << "Not all four metrics rows appeared within 2 s";

            auto r = query_metrics_single(svc);
            const auto &cha = r["channels"][ch_a];
            const auto &chb = r["channels"][ch_b];

            // Channel A: P-A on producer row, C-A on consumer row.
            EXPECT_EQ(cha["producers"][p_a]["tag"], "P-A");
            EXPECT_EQ(cha["producers"][p_a]["v"],   11);
            EXPECT_EQ(cha["consumers"][c_a]["tag"], "C-A");
            EXPECT_EQ(cha["consumers"][c_a]["v"],   33);
            EXPECT_FALSE(cha["producers"][p_a].contains("tag")
                          && cha["producers"][p_a]["tag"] == "C-A")
                << "Sanity: producer P-A row must NOT hold C-A's tag "
                   "(H34-root)";

            // Channel B: same isolation property.
            EXPECT_EQ(chb["producers"][p_b]["tag"], "P-B");
            EXPECT_EQ(chb["producers"][p_b]["v"],   22);
            EXPECT_EQ(chb["consumers"][c_b]["tag"], "C-B");
            EXPECT_EQ(chb["consumers"][c_b]["v"],   44);

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
    return run_with_broker(
        "datahub_metrics::all_channels_includes_channels_without_metrics",
        [](const std::string &ep, const std::string &pk, BrokerService &svc) {
            // M1.4 behavioral change: pre-fix `query_metrics()` (all-
            // channels) iterated `metrics_store_` (only channels with
            // reports); post-fix iterates HubState's
            // `snapshot().channels` (ALL registered channels).
            const std::string channel_with    =
                pid_chan("metrics.has.metrics");
            const std::string channel_without =
                pid_chan("metrics.no.metrics");
            const std::string uid_with        = "prod." + channel_with;
            const std::string uid_without     = "prod." + channel_without;

            BrcHandle bh1, bh2;
            bh1.start(ep, pk, uid_with);
            bh2.start(ep, pk, uid_without);
            ASSERT_TRUE(bh1.brc.register_channel(
                            make_reg_opts(channel_with, uid_with), 3000)
                            .has_value());
            ASSERT_TRUE(bh2.brc.register_channel(
                            make_reg_opts(channel_without, uid_without), 3000)
                            .has_value());

            bh1.brc.send_heartbeat(channel_with, uid_with, "producer",
                                    json{{"data_point", 7}});

            ASSERT_TRUE(poll_until([&] {
                auto r = query_metrics_single(svc);
                return r.contains("channels")
                    && r["channels"].contains(channel_with)
                    && r["channels"][channel_with].contains("producers");
            }, std::chrono::seconds(2)))
                << "channel_with did not record its metrics within 2s";

            auto r = query_metrics_single(svc);
            ASSERT_TRUE(r.contains("channels"));

            ASSERT_TRUE(r["channels"].contains(channel_with))
                << "Channel with metrics appears (was true pre-M1.4 too)";
            ASSERT_TRUE(r["channels"].contains(channel_without))
                << "M1.4 contract: channel WITHOUT metrics also appears in "
                   "all-channels query (with empty metrics object).";

            EXPECT_EQ(
                r["channels"][channel_with]["producers"][uid_with]
                    ["data_point"],
                7);

            auto &cw = r["channels"][channel_without];
            EXPECT_FALSE(cw.contains("producers"))
                << "Empty channel has no producers metrics";
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
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "datahub_metrics")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::datahub_metrics;

                if (sc == "heartbeat_metrics_stored_by_broker")
                    return heartbeat_metrics_stored_by_broker();
                if (sc == "consumer_heartbeat_metrics_stored_by_broker")
                    return consumer_heartbeat_metrics_stored_by_broker();
                if (sc == "query_metrics_unknown_channel_returns_empty")
                    return query_metrics_unknown_channel_returns_empty();
                if (sc == "query_metrics_all_channels")
                    return query_metrics_all_channels();
                if (sc == "heartbeat_no_metrics_backward_compat")
                    return heartbeat_no_metrics_backward_compat();
                if (sc == "metrics_update_overwrite_on_heartbeat")
                    return metrics_update_overwrite_on_heartbeat();
                if (sc == "producer_pid_in_query_result")
                    return producer_pid_in_query_result();
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
                if (sc == "fan_in_two_producers_metrics_do_not_overwrite")
                    return fan_in_two_producers_metrics_do_not_overwrite();
                if (sc == "query_engine_filter_echo")
                    return query_engine_filter_echo();
                if (sc == "old_metrics_report_req_gets_unknown_msg_type")
                    return old_metrics_report_req_gets_unknown_msg_type();
                if (sc == "multi_presence_end_to_end_no_cross_attribution")
                    return multi_presence_end_to_end_no_cross_attribution();
                if (sc == "all_channels_includes_channels_without_metrics")
                    return all_channels_includes_channels_without_metrics();
                return -1;
            });
    }
} g_registrar;

} // namespace
