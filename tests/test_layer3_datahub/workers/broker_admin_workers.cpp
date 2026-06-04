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

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
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
                          curve.role(prod_uid));
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          curve.role(cons_uid));
            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, cons_uid), 3000);
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
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
                return -1;
            });
    }
} g_registrar;

} // namespace
