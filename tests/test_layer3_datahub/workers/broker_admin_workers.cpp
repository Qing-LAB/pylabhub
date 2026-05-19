/**
 * @file broker_admin_workers.cpp
 * @brief Worker bodies for BrokerService admin API tests
 *        (Pattern 3).  Migrated 2026-05-13 from in-process
 *        `SetUpTestSuite`-owned `LifecycleGuard`.
 */

#include "broker_admin_workers.h"

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
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::poll_until;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pylabhub::tests::worker
{
namespace broker_admin
{

namespace
{

fs::path make_test_hub_dir()
{
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_admin_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    pylabhub::utils::HubDirectory::init_directory(dir, "AdminTestHub");

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
    BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread       thread;

    void start(const std::string &ep, const std::string &pk,
               const std::string &uid)
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

/// Run a worker body with a freshly spun-up HubHost + LogCaptureFixture.
template <typename Body>
int run_with_host(std::string_view worker_name, Body &&body,
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

            body(*host);

            host.reset();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            std::error_code ec;
            fs::remove_all(dir, ec);
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
        "broker_admin::list_channels_empty",
        [](HubHost &host) {
            std::string result = host.broker().list_channels_json_str();
            auto j = json::parse(result);
            ASSERT_TRUE(j.is_array());
            EXPECT_TRUE(j.empty())
                << "Expected empty channel list, got: " << result;
        });
}

int list_channels_one_channel()
{
    return run_with_host(
        "broker_admin::list_channels_one_channel",
        [](HubHost &host) {
            const std::string channel = pid_chan("admin.list.one");

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(),
                     "prod." + channel);
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, "prod." + channel), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            std::string result = host.broker().list_channels_json_str();
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
    return run_with_host(
        "broker_admin::list_channels_field_presence",
        [](HubHost &host) {
            const std::string channel = pid_chan("admin.list.fields");

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(),
                     "prod." + channel);
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, "prod." + channel), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            std::string result = host.broker().list_channels_json_str();
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
        "broker_admin::snapshot_empty",
        [](HubHost &host) {
            ChannelSnapshot snap = host.broker().query_channel_snapshot();
            EXPECT_TRUE(snap.channels.empty());
        });
}

int snapshot_one_channel()
{
    return run_with_host(
        "broker_admin::snapshot_one_channel",
        [](HubHost &host) {
            const std::string channel = pid_chan("admin.snap.one");

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(),
                     "prod." + channel);
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, "prod." + channel), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            ChannelSnapshot snap = host.broker().query_channel_snapshot();
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
    return run_with_host(
        "broker_admin::snapshot_after_consumer",
        [](HubHost &host) {
            const std::string channel = pid_chan("admin.snap.consumer");

            BrcHandle prod_bh;
            prod_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          "prod." + channel);
            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, "prod." + channel), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            prod_bh.brc.send_heartbeat(channel, "prod." + channel,
                                        "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            BrcHandle cons_bh;
            cons_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          "cons." + channel);
            auto cons_reg = cons_bh.brc.register_consumer(
                make_cons_opts(channel, "cons." + channel), 3000);
            ASSERT_TRUE(cons_reg.has_value()) << "register_consumer failed";

            ChannelSnapshot snap = host.broker().query_channel_snapshot();
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
    return run_with_host(
        "broker_admin::close_channel_existing",
        [](HubHost &host) {
            const std::string channel = pid_chan("admin.close.existing");

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(),
                     "prod." + channel);
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, "prod." + channel), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            host.broker().request_close_channel(channel);

            auto channel_gone = [&] {
                auto s = host.broker().query_channel_snapshot();
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
        "broker_admin::close_channel_non_existent",
        [](HubHost &host) {
            EXPECT_NO_THROW(
                host.broker().request_close_channel(pid_chan("admin.close.bogus")));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            EXPECT_NO_THROW({
                ChannelSnapshot snap = host.broker().query_channel_snapshot();
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
