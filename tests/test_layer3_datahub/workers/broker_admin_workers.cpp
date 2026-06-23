/**
 * @file broker_admin_workers.cpp
 * @brief Worker bodies for BrokerService admin API tests
 *        (Pattern 3).  Migrated 2026-05-13 from in-process
 *        `SetUpTestSuite`-owned `LifecycleGuard`.
 */

#include "broker_admin_workers.h"

#include "broker_test_harness.h"
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
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/security/shm_capability_channel.hpp" // #281 default_shm_capability_endpoint

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::poll_until;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace broker_admin
{

namespace
{

json hubhost_overrides()
{
    return json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

// Thin wrappers around the canonical production payload builders
// (`pylabhub::hub::build_*_reg_payload` in `utils/role_reg_payload.hpp`).
// All wire-shape logic — required fields, defaults, ordering — lives
// in the production builders.  Tests only supply the inputs.
json make_reg_opts(const std::string &channel, const std::string &role_uid,
                   const std::string &zmq_pubkey)
{
    // #281 (2026-06-23): post-broker-hardening, `data_transport` is a
    // REQUIRED REG_REQ field.  This helper is used by tests that exercise
    // the broker's admin surface (list_channels, snapshot, close_channel)
    // — they don't care which transport per se, but the wire MUST declare
    // one.  Mirror production producer_role_host: SHM transport with the
    // canonical endpoint via `default_shm_capability_endpoint(channel)`.
    return pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "test_producer",
            .role_tag   = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = zmq_pubkey,
            .shm_capability_endpoint =
                pylabhub::utils::security::default_shm_capability_endpoint(channel),
        });
}

json make_cons_opts(const std::string &channel, const std::string &consumer_uid,
                    const std::string &zmq_pubkey)
{
    return pylabhub::hub::build_consumer_reg_payload(
        pylabhub::hub::ConsumerRegInputs{
            .channel    = channel,
            .role_uid   = consumer_uid,
            .role_name  = "test_consumer",
            .zmq_pubkey = zmq_pubkey,
        });
}

/// Run a worker body with a freshly spun-up HubHostBrokerHandle +
/// LogCaptureFixture under real CURVE + admission (HEP-CORE-0035
/// §2 + §4.6.5).  Body receives (broker, curve).
template <typename Body>
int run_with_host(std::string_view worker_name,
                  std::vector<std::string> role_uids,
                  Body &&body,
                  std::vector<std::string> expect_log_warns = {})
{
    return run_gtest_worker(
        [role_uids = std::move(role_uids),
         body = std::forward<Body>(body),
         expect_log_warns = std::move(expect_log_warns)]() mutable {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : expect_log_warns)
                log_cap.ExpectLogWarn(w);

            auto curve = pylabhub::tests::make_curve_setup(role_uids);
            // HEP-CORE-0040 §172: fixture owns SMS + KeyStore + identity
            // seeding (the production-shaped path); start_hubhost_broker
            // only reads from key_store().
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test.l3", "test.broker_admin.harness", curve);
            auto broker = pylabhub::tests::start_hubhost_broker(
                hubhost_overrides(), curve);
            ASSERT_TRUE(broker.host && broker.host->is_running());

            body(broker, curve);

            broker.stop_and_join();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetDataBlockModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

int list_channels_empty()
{
    return run_with_host(
        "broker_admin::list_channels_empty", {},
        [](pylabhub::tests::HubHostBrokerHandle &broker,
           pylabhub::tests::CurveSetup &) {
            std::string result = broker.service().list_channels_json_str();
            auto j = json::parse(result);
            ASSERT_TRUE(j.is_array());
            EXPECT_TRUE(j.empty())
                << "Expected empty channel list, got: " << result;
        });
}

int list_channels_one_channel()
{
    const std::string channel = pid_chan("admin.list.one");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::list_channels_one_channel", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid, curve.role(uid).public_z85), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            std::string result = broker.service().list_channels_json_str();
            auto j = json::parse(result);
            ASSERT_TRUE(j.is_array());
            ASSERT_GE(j.size(), 1u);

            bool found = false;
            for (const auto &entry : j)
            {
                if (entry.value("name", "") == channel)
                {
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found)
                << "Channel '" << channel << "' not found in: " << result;

            bh.stop();
        });
}

int list_channels_field_presence()
{
    const std::string channel = pid_chan("admin.list.fields");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::list_channels_field_presence", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid, curve.role(uid).public_z85), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            std::string result = broker.service().list_channels_json_str();
            auto j = json::parse(result);

            const json *entry = nullptr;
            for (const auto &e : j)
            {
                if (e.value("name", "") == channel)
                {
                    entry = &e;
                    break;
                }
            }
            ASSERT_NE(entry, nullptr) << "Channel not found in JSON";

            EXPECT_TRUE(entry->contains("name"));
            EXPECT_TRUE(entry->contains("observable"));
            EXPECT_TRUE(entry->contains("consumer_count"));
            EXPECT_TRUE(entry->contains("producer_pid"));

            bh.stop();
        });
}

int snapshot_empty()
{
    return run_with_host(
        "broker_admin::snapshot_empty", {},
        [](pylabhub::tests::HubHostBrokerHandle &broker,
           pylabhub::tests::CurveSetup &) {
            ChannelSnapshot snap = broker.service().query_channel_snapshot();
            EXPECT_TRUE(snap.channels.empty());
        });
}

int snapshot_one_channel()
{
    const std::string channel = pid_chan("admin.snap.one");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::snapshot_one_channel", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid, curve.role(uid).public_z85), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            ChannelSnapshot snap = broker.service().query_channel_snapshot();
            ASSERT_GE(snap.channels.size(), 1u);

            const ChannelSnapshotEntry *found = nullptr;
            for (const auto &ch : snap.channels)
            {
                if (ch.name == channel)
                {
                    found = &ch;
                    break;
                }
            }
            ASSERT_NE(found, nullptr) << "Channel not in snapshot";
            EXPECT_FALSE(found->observable.empty());
            EXPECT_EQ(found->consumer_count, 0);

            bh.stop();
        });
}

int snapshot_after_consumer()
{
    const std::string channel  = pid_chan("admin.snap.consumer");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_admin::snapshot_after_consumer", {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid,
                          pylabhub::tests::role_keystore_name(prod_uid));
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          pylabhub::tests::role_keystore_name(cons_uid));
            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, cons_uid, curve.role(cons_uid).public_z85), 3000);
            ASSERT_TRUE(cons_reg.has_value()) << "register_consumer failed";

            ChannelSnapshot snap = broker.service().query_channel_snapshot();
            const ChannelSnapshotEntry *found = nullptr;
            for (const auto &ch : snap.channels)
            {
                if (ch.name == channel)
                {
                    found = &ch;
                    break;
                }
            }
            ASSERT_NE(found, nullptr) << "Channel not in snapshot";
            EXPECT_EQ(found->consumer_count, 1);

            cons_bh.stop();
            prod_bh.stop();
        });
}

int close_channel_existing()
{
    const std::string channel = pid_chan("admin.close.existing");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::close_channel_existing", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid, curve.role(uid).public_z85), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            broker.service().request_close_channel(channel);

            auto channel_gone = [&] {
                auto s = broker.service().query_channel_snapshot();
                for (const auto &ch : s.channels)
                    if (ch.name == channel) return false;
                return true;
            };
            EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
                << "Channel still present 3s after request_close_channel";

            bh.stop();
        });
}

int close_channel_non_existent()
{
    return run_with_host(
        "broker_admin::close_channel_non_existent", {},
        [](pylabhub::tests::HubHostBrokerHandle &broker,
           pylabhub::tests::CurveSetup &) {
            EXPECT_NO_THROW(
                broker.service().request_close_channel(
                    pid_chan("admin.close.bogus")));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            EXPECT_NO_THROW({
                ChannelSnapshot snap = broker.service().query_channel_snapshot();
                (void)snap.channels.size();
            });
        });
}

// ============================================================================
// #281 (2026-06-23) — REG_REQ wire-contract pins for `data_transport`.
// ============================================================================
//
// Six wire-level pins that exercise the broker REG_REQ handler directly via
// `BrokerRequestComm::register_channel` (real CURVE + admission via the
// `run_with_host` harness):
//
//   * Four NEGATIVE pins: REG_REQ payloads that should be REJECTED with
//     INVALID_REQUEST per HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1.  The
//     broker emits a LOGGER_WARN naming the violation; the test harness
//     declares each via `ExpectLogWarn` so the warn is allowlisted (it is
//     EXPECTED, not collateral noise).  Without `ExpectLogWarn`, the
//     harness's end-of-test `AssertNoUnexpectedLogWarnError` would fail
//     even though the response shape is correct.
//
//   * Two POSITIVE pins: the canonical SHM and ZMQ wire shapes that
//     production producers emit (status="success").
//
// Why this lives in `broker_admin_workers.cpp`: the file already wires
// `run_with_host` (CURVE harness + KeyStore fixture) and `make_reg_opts`
// helper, and BrokerAdminTest is the natural fixture home for "broker
// rejects malformed wire shape" coverage.  The L3 broker test ladder
// (rungs 2/3 under Pattern4*) targets handshake / heartbeat / lifecycle
// flows, not REG_REQ field-level wire validation — so the pins live here
// rather than expanding the rung set.

namespace {

/// Build a baseline SHM REG_REQ payload (data_transport="shm" + valid
/// shm_capability_endpoint) that satisfies the broker's strict checks.
/// Tests then erase / mutate specific fields to exercise each rejection
/// branch.  Mirrors `make_reg_opts` shape but exposed locally so the
/// tests don't accidentally depend on a future change to that helper.
json make_baseline_shm_reg(const std::string &channel,
                           const std::string &role_uid,
                           const std::string &zmq_pubkey)
{
    return pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "reg_validation_producer",
            .role_tag   = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = zmq_pubkey,
            .shm_capability_endpoint =
                pylabhub::utils::security::default_shm_capability_endpoint(channel),
        });
}

/// Build a baseline ZMQ REG_REQ payload.
json make_baseline_zmq_reg(const std::string &channel,
                           const std::string &role_uid,
                           const std::string &zmq_pubkey)
{
    return pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "reg_validation_producer",
            .role_tag   = "producer",
            .has_shm    = false,
            .is_zmq_transport  = true,
            .zmq_node_endpoint = "tcp://127.0.0.1:0",
            .zmq_pubkey = zmq_pubkey,
            .shm_capability_endpoint = {},
        });
}

} // anonymous namespace

int reg_validation_missing_data_transport()
{
    const std::string channel = pid_chan("reg.val.missing_dt");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::reg_validation_missing_data_transport", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid,
                     pylabhub::tests::role_keystore_name(uid));
            auto reg_opts = make_baseline_shm_reg(
                channel, uid, curve.role(uid).public_z85);
            reg_opts.erase("data_transport");

            auto resp = bh.brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");
            EXPECT_NE(resp->value("message", std::string{})
                          .find("data_transport"),
                      std::string::npos)
                << "Error message should name the missing field; got: "
                << resp->value("message", std::string{});

            bh.stop();
        },
        // Broker emits a LOGGER_WARN naming the rejection — allowlist it.
        {"missing required `data_transport` field"});
}

int reg_validation_empty_data_transport()
{
    const std::string channel = pid_chan("reg.val.empty_dt");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::reg_validation_empty_data_transport", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid,
                     pylabhub::tests::role_keystore_name(uid));
            auto reg_opts = make_baseline_shm_reg(
                channel, uid, curve.role(uid).public_z85);
            reg_opts["data_transport"] = "";

            auto resp = bh.brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");

            bh.stop();
        },
        // Broker emits the "not one of {shm,zmq}" WARN for explicit empty.
        {"is not one of"});
}

int reg_validation_bogus_data_transport()
{
    const std::string channel = pid_chan("reg.val.bogus_dt");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::reg_validation_bogus_data_transport", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid,
                     pylabhub::tests::role_keystore_name(uid));
            auto reg_opts = make_baseline_shm_reg(
                channel, uid, curve.role(uid).public_z85);
            reg_opts["data_transport"] = "tcp";

            auto resp = bh.brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");
            EXPECT_NE(resp->value("message", std::string{}).find("tcp"),
                      std::string::npos)
                << "Error message should echo the bad value; got: "
                << resp->value("message", std::string{});

            bh.stop();
        },
        {"is not one of"});
}

int reg_validation_shm_missing_endpoint()
{
    const std::string channel = pid_chan("reg.val.shm_no_ep");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::reg_validation_shm_missing_endpoint", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid,
                     pylabhub::tests::role_keystore_name(uid));
            auto reg_opts = make_baseline_shm_reg(
                channel, uid, curve.role(uid).public_z85);
            // data_transport stays "shm"; explicitly drop the endpoint.
            // This closes the pre-#281 coverage gap from #268
            // (1i-prod-hardening shipped the §5.1 endpoint-required
            // check without a wire-level regression pin).
            reg_opts.erase("shm_capability_endpoint");

            auto resp = bh.brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}), "INVALID_REQUEST");
            EXPECT_NE(resp->value("message", std::string{})
                          .find("shm_capability_endpoint"),
                      std::string::npos)
                << "Error message should name the missing endpoint field; got: "
                << resp->value("message", std::string{});

            bh.stop();
        },
        {"data_transport='shm' but `shm_capability_endpoint` is empty"});
}

int reg_validation_shm_success()
{
    const std::string channel = pid_chan("reg.val.shm_ok");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::reg_validation_shm_success", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid,
                     pylabhub::tests::role_keystore_name(uid));
            auto reg_opts = make_baseline_shm_reg(
                channel, uid, curve.role(uid).public_z85);

            auto resp = bh.brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(resp->value("status", std::string{}), "success")
                << "Canonical SHM REG_REQ shape must succeed; got error_code='"
                << resp->value("error_code", std::string{}) << "' message='"
                << resp->value("message", std::string{}) << "'";

            // Sanity: the broker recorded the channel.  We don't pin
            // the per-channel `data_transport` field here — the
            // existing list/snapshot admin surfaces (`list_channels_json_str`
            // and `ChannelSnapshotEntry`) don't expose `data_transport`,
            // and threading a custom view through just to assert it
            // would over-specify the test.  The negative pins above
            // confirm the broker classifies the wire correctly; the
            // status="success" here is the positive end.
            auto snap = broker.service().query_channel_snapshot();
            bool seen = false;
            for (const auto &ch : snap.channels)
                if (ch.name == channel) { seen = true; break; }
            EXPECT_TRUE(seen) << "Channel '" << channel
                              << "' missing from snapshot after success";

            bh.stop();
        });
}

int reg_validation_zmq_success()
{
    const std::string channel = pid_chan("reg.val.zmq_ok");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::reg_validation_zmq_success", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid,
                     pylabhub::tests::role_keystore_name(uid));
            auto reg_opts = make_baseline_zmq_reg(
                channel, uid, curve.role(uid).public_z85);

            auto resp = bh.brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(resp.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(resp->value("status", std::string{}), "success")
                << "Canonical ZMQ REG_REQ shape must succeed; got error_code='"
                << resp->value("error_code", std::string{}) << "' message='"
                << resp->value("message", std::string{}) << "'";

            auto snap = broker.service().query_channel_snapshot();
            bool seen = false;
            for (const auto &ch : snap.channels)
                if (ch.name == channel) { seen = true; break; }
            EXPECT_TRUE(seen) << "Channel '" << channel
                              << "' missing from snapshot after success";

            bh.stop();
        });
}

} // namespace broker_admin
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct BrokerAdminRegistrar
{
    BrokerAdminRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_admin")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_admin;

                if (sc == "list_channels_empty")
                    return list_channels_empty();
                if (sc == "list_channels_one_channel")
                    return list_channels_one_channel();
                if (sc == "list_channels_field_presence")
                    return list_channels_field_presence();
                if (sc == "snapshot_empty")
                    return snapshot_empty();
                if (sc == "snapshot_one_channel")
                    return snapshot_one_channel();
                if (sc == "snapshot_after_consumer")
                    return snapshot_after_consumer();
                if (sc == "close_channel_existing")
                    return close_channel_existing();
                if (sc == "close_channel_non_existent")
                    return close_channel_non_existent();
                // #281 (2026-06-23) REG_REQ wire-contract pins
                if (sc == "reg_validation_missing_data_transport")
                    return reg_validation_missing_data_transport();
                if (sc == "reg_validation_empty_data_transport")
                    return reg_validation_empty_data_transport();
                if (sc == "reg_validation_bogus_data_transport")
                    return reg_validation_bogus_data_transport();
                if (sc == "reg_validation_shm_missing_endpoint")
                    return reg_validation_shm_missing_endpoint();
                if (sc == "reg_validation_shm_success")
                    return reg_validation_shm_success();
                if (sc == "reg_validation_zmq_success")
                    return reg_validation_zmq_success();
                return -1;
            });
    }
} g_registrar;

} // namespace
