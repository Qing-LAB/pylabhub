/**
 * @file hub_host_integration_workers.cpp
 * @brief Worker bodies for HubHost ↔ BrokerService L3 integration tests
 *        (Pattern 3).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Each scenario constructs `HubHost`
 * (spawning a `BrokerService` thread) and a `BrokerRequestComm` client
 * — transitively touches Logger / FileLock / JsonConfig / CryptoUtils /
 * ZMQContext; Pattern 3 isolation required per README_testing.md.
 */

#include "hub_host_integration_workers.h"

#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"

#include "log_capture_fixture.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace pylabhub::utils;
using namespace pylabhub::broker;
using pylabhub::config::HubConfig;
using pylabhub::hub::BrokerRequestComm;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace hub_host_integration
{

namespace
{

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l3_hubhost_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

fs::path init_test_hub_dir(const char *tag)
{
    const fs::path dir = unique_temp_dir(tag);
    HubDirectory::init_directory(dir, "L3HubHost");

    json j;
    {
        std::ifstream f(dir / "hub.json");
        j = json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";
    {
        std::ofstream f(dir / "hub.json");
        f << j.dump(2);
    }
    return dir;
}

json make_reg_opts(const std::string &channel, const std::string &uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

struct BrcHandle
{
    BrokerRequestComm  brc;
    std::atomic<bool>  running{true};
    std::thread        thread;

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

std::unique_ptr<HubHost> spawn_host(const char *tag,
                                     std::vector<fs::path> &cleanup)
{
    const fs::path dir = init_test_hub_dir(tag);
    cleanup.push_back(dir);
    auto cfg  = HubConfig::load_from_directory(dir.string());
    auto host = std::make_unique<HubHost>(std::move(cfg));
    host->startup();
    return host;
}

void cleanup_paths(std::vector<fs::path> &cleanup)
{
    for (const auto &p : cleanup)
    {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
    cleanup.clear();
}

} // namespace

int hubhost_brokerreachable_afterstartup()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();
            std::vector<fs::path> cleanup;

            auto host = spawn_host("reachable", cleanup);
            ASSERT_FALSE(host->broker_endpoint().empty())
                << "HubHost::broker_endpoint() empty after startup — "
                   "bind didn't happen";

            BrcHandle client;
            client.start(host->broker_endpoint(), host->broker_pubkey(),
                         "prod.test.uid_reachable");

            client.stop();
            host->shutdown();

            cleanup_paths(cleanup);
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_host_integration::hubhost_brokerreachable_afterstartup",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int hubhost_regreq_roundtripsviaspawnedbroker()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();
            std::vector<fs::path> cleanup;

            auto host = spawn_host("regreq", cleanup);

            BrcHandle client;
            const std::string uid     = "prod.cam.uid_regreq";
            const std::string channel = pid_chan("hubhost.regreq");
            client.start(host->broker_endpoint(), host->broker_pubkey(), uid);

            auto reg = client.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(reg->value("status", ""), "success");

            ASSERT_TRUE(reg->contains("heartbeat"));
            EXPECT_EQ((*reg)["heartbeat"].value("heartbeat_interval_ms", -1),
                      host->config().broker().heartbeat_interval_ms);

            const auto snap = host->state().snapshot();
            EXPECT_TRUE(snap.channels.count(channel) > 0)
                << "channel '" << channel
                << "' missing from HubHost::state() snapshot after REG_ACK";

            client.stop();
            host->shutdown();

            cleanup_paths(cleanup);
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_host_integration::hubhost_regreq_roundtripsviaspawnedbroker",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int hubhost_shutdown_breaksclientconnection()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();
            std::vector<fs::path> cleanup;

            log_cap.ExpectLogWarn("REG_REQ timed out");
            // After A2 (Wave-B M8 prep): BRC's socket monitor is now
            // polled as a first-class poll item (was a side-effect of
            // DEALER traffic), so ZMQ_EVENT_DISCONNECTED is detected
            // promptly when host->shutdown() closes the broker socket.
            // The hub-dead WARN was previously suppressed by the
            // detection delay; now it fires within this test's window.
            log_cap.ExpectLogWarn(
                "BrokerRequestComm: hub-dead (ZMQ_EVENT_DISCONNECTED)");

            auto host = spawn_host("shutdown", cleanup);

            BrcHandle client;
            client.start(host->broker_endpoint(), host->broker_pubkey(),
                         "prod.test.uid_shutdown");

            auto reg = client.brc.register_channel(
                make_reg_opts(pid_chan("hubhost.shutdown.precheck"),
                               "prod.test.uid_shutdown"),
                3000);
            ASSERT_TRUE(reg.has_value());

            host->shutdown();
            EXPECT_FALSE(host->is_running());

            auto reg2 = client.brc.register_channel(
                make_reg_opts(pid_chan("hubhost.shutdown.postcheck"),
                               "prod.test.uid_shutdown"),
                3000);
            EXPECT_FALSE(reg2.has_value())
                << "REG_REQ unexpectedly succeeded after host->shutdown()";

            client.stop();

            cleanup_paths(cleanup);
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_host_integration::hubhost_shutdown_breaksclientconnection",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace hub_host_integration
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct HubHostIntegrationRegistrar
{
    HubHostIntegrationRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "hub_host_integration")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_host_integration;

                if (sc == "hubhost_brokerreachable_afterstartup")
                    return hubhost_brokerreachable_afterstartup();
                if (sc == "hubhost_regreq_roundtripsviaspawnedbroker")
                    return hubhost_regreq_roundtripsviaspawnedbroker();
                if (sc == "hubhost_shutdown_breaksclientconnection")
                    return hubhost_shutdown_breaksclientconnection();
                return -1;
            });
    }
} g_registrar;

} // namespace
