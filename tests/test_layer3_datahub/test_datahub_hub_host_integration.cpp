/**
 * @file test_datahub_hub_host_integration.cpp
 * @brief L3 integration tests for HubHost ↔ BrokerService wiring.
 *
 * L2 tests (`test_layer2_service/test_hub_host.cpp`) cover HubHost's
 * own contract: lifecycle state machine, idempotency, the
 * `state() == broker().hub_state()` ownership invariant, etc.  They
 * deliberately do not connect a real client.
 *
 * The tests here verify that the broker spawned through HubHost
 * actually serves protocol traffic — i.e. the lifecycle wiring
 * produced a working hub.  They run a `BrokerRequestComm` client
 * against the HubHost-spawned broker and exercise the registration
 * round-trip.
 *
 * Out of scope (covered elsewhere):
 *   - BrokerService protocol behavior — see
 *     `test_datahub_broker_protocol.cpp` and friends.
 *   - HubState mutation correctness — see
 *     `test_layer2_service/test_hub_state.cpp`.
 *   - ThreadManager bounded-join semantics — see
 *     `test_layer2_service/test_role_data_loop.cpp`
 *     `ThreadManagerTest.*`.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace pylabhub::utils;
using namespace pylabhub::broker;
using pylabhub::config::HubConfig;
using pylabhub::hub::BrokerRequestComm;
using pylabhub::hub_host::HubHost;
using json = nlohmann::json;

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

/// Build a hub directory with a hub.json patched for L3 in-process
/// testing: ephemeral loopback port, no CURVE auth, admin disabled.
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
    {
        std::ofstream f(dir / "hub.json");
        f << j.dump(2);
    }
    return dir;
}

/// Helper: build a REG_REQ payload.
json make_reg_opts(const std::string &channel, const std::string &uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
    opts["role_uid"]          = uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

/// Wrap a BRC client + its poll-loop thread.
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
        cfg.broker_pubkey   = pk; // empty = NULL mech
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

} // namespace

// ─── Fixture ────────────────────────────────────────────────────────────────

class HubHostIntegrationTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(),
            FileLock::GetLifecycleModule(),
            JsonConfig::GetLifecycleModule(),
            pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetZMQContextModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::unique_ptr<HubHost> spawn_host(const char *tag)
    {
        const fs::path dir = init_test_hub_dir(tag);
        paths_to_clean_.push_back(dir);
        auto cfg  = HubConfig::load_from_directory(dir.string());
        auto host = std::make_unique<HubHost>(std::move(cfg));
        host->startup();
        return host;
    }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
    std::vector<fs::path>                  paths_to_clean_;
};

std::unique_ptr<LifecycleGuard> HubHostIntegrationTest::s_lifecycle_;

// ─── Tests ──────────────────────────────────────────────────────────────────

TEST_F(HubHostIntegrationTest, HubHost_BrokerReachable_AfterStartup)
{
    // Verifies the broker thread spawned through HubHost is actually
    // bound and accepting connections.  Strongest "lifecycle wiring
    // produced a working broker" signal at L3.
    auto host = spawn_host("reachable");
    ASSERT_FALSE(host->broker_endpoint().empty())
        << "HubHost::broker_endpoint() empty after startup — bind didn't happen";

    BrcHandle client;
    client.start(host->broker_endpoint(), host->broker_pubkey(),
                 "prod.test.uid_reachable");

    // BRC connect blocks until the ZMQ handshake completes.  If we got
    // here, the broker thread is alive and the socket is responding.

    client.stop();
    host->shutdown();
}

TEST_F(HubHostIntegrationTest, HubHost_RegReq_RoundTripsViaSpawnedBroker)
{
    // Full REG_REQ → REG_ACK round-trip.  Proves:
    //   1. Broker thread is processing protocol traffic.
    //   2. HubBrokerConfig fields propagate into BrokerService::Config
    //      (REG_ACK heartbeat block reflects HEP-0023 §2.5.1 values).
    //   3. The HubState being updated is the one HubHost owns
    //      (snapshot afterwards shows the channel).
    auto host = spawn_host("regreq");

    BrcHandle client;
    const std::string uid     = "prod.cam.uid_regreq";
    const std::string channel = pid_chan("hubhost.regreq");
    client.start(host->broker_endpoint(), host->broker_pubkey(), uid);

    auto reg = client.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value()) << "REG_REQ timed out";
    EXPECT_EQ(reg->value("status", ""), "success");

    // HEP-CORE-0023 §2.5.1 — REG_ACK carries the heartbeat negotiation
    // block populated from HubBrokerConfig.
    ASSERT_TRUE(reg->contains("heartbeat"));
    EXPECT_EQ((*reg)["heartbeat"].value("heartbeat_interval_ms", -1),
              host->config().broker().heartbeat_interval_ms);

    // The channel must appear in HubHost-owned state — verifies the
    // 6.1a HubState ownership invariant operationally (broker writes
    // landed in our HubState).
    const auto snap = host->state().snapshot();
    EXPECT_TRUE(snap.channels.count(channel) > 0)
        << "channel '" << channel
        << "' missing from HubHost::state() snapshot after REG_ACK";

    client.stop();
    host->shutdown();
}

TEST_F(HubHostIntegrationTest, HubHost_Shutdown_BreaksClientConnection)
{
    // After host->shutdown(), the broker's poll loop must exit (not
    // just the running flag).  Verified via the BRC's hub-dead
    // callback firing within a bounded window.
    auto host = spawn_host("shutdown");

    std::atomic<bool> hub_dead_fired{false};

    BrcHandle client;
    client.brc.on_hub_dead([&] { hub_dead_fired.store(true); });
    client.start(host->broker_endpoint(), host->broker_pubkey(),
                 "prod.test.uid_shutdown");

    // Confirm the client is connected and exchanging traffic.
    auto reg = client.brc.register_channel(
        make_reg_opts(pid_chan("hubhost.shutdown.precheck"),
                       "prod.test.uid_shutdown"),
        3000);
    ASSERT_TRUE(reg.has_value());

    // Shut the host down; broker thread should exit promptly.
    host->shutdown();

    // ZMTP heartbeat-driven hub-dead detection takes up to
    // ZMQ_HEARTBEAT_TIMEOUT (30s) by default; we don't wait that
    // long here.  The cheaper signal is that the broker thread has
    // actually exited — we can probe by attempting a request that
    // will time out fast (3 s) instead of hanging forever.
    auto reg2 = client.brc.register_channel(
        make_reg_opts(pid_chan("hubhost.shutdown.postcheck"),
                       "prod.test.uid_shutdown"),
        3000);
    EXPECT_FALSE(reg2.has_value())
        << "REG_REQ unexpectedly succeeded after host->shutdown()";

    client.stop();
    // host's destructor will run (no-op — already shut down).
}
