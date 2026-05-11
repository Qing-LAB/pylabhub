/**
 * @file test_datahub_broker_admin.cpp
 * @brief BrokerService admin API tests: list_channels_json_str,
 *        query_channel_snapshot, request_close_channel.
 *
 * Suite: BrokerAdminTest
 *
 * Uses the LocalBrokerHandle in-process pattern (same as BrokerSchemaTest):
 * no subprocess workers needed. BrokerRequestComm registers/discovers channels,
 * admin methods are called from the test thread.
 *
 * Cross-platform notes:
 *   - All endpoints use tcp://127.0.0.1:0 (ephemeral port)
 *   - Timeouts are generous (1000ms+) for Windows scheduler latency
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "test_sync_utils.h"
#include "log_capture_fixture.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{

// ── HubHostHandle — RAII wrapper around the real production HubHost ──────────
//
// Replaces the legacy `LocalBrokerHandle` mock-host pattern (raw HubState
// + raw BrokerService + raw std::thread) per the test-design principle in
// `docs/todo/TESTING_TODO.md` §"Test Design Principles": broker tests
// must run against the real production assembly.
//
// The wire-protocol entry points (broker endpoint + pubkey) are the same;
// the assembly behind them is now the same code production runs (HubConfig
// → HubHost → ThreadManager-launched broker thread).

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

HubHostHandle start_local_broker(BrokerService::Config /*legacy_cfg*/)
{
    HubHostHandle h;
    h.hub_dir = make_test_hub_dir();
    h.host    = std::make_unique<HubHost>(
        HubConfig::load_from_directory(h.hub_dir.string()));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = h.host->broker_pubkey();
    return h;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(getpid());
}

/// Build minimal JSON opts for BrokerRequestComm::register_channel().
nlohmann::json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

/// Build minimal JSON opts for BrokerRequestComm::register_consumer().
nlohmann::json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    nlohmann::json opts;
    opts["channel_name"]   = channel;
    opts["consumer_uid"]   = consumer_uid;
    opts["consumer_name"]  = "test_consumer";
    opts["consumer_pid"]   = ::getpid();
    return opts;
}

/// Helper: connect a BrokerRequestComm, start its poll thread.
struct BrcHandle
{
    BrokerRequestComm       brc;
    std::atomic<bool>       running{true};
    std::thread             thread;

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

} // anonymous namespace

// ============================================================================
// BrokerAdminTest fixture
// ============================================================================

class BrokerAdminTest : public ::testing::Test,
                         public pylabhub::tests::LogCaptureFixture
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(),
            FileLock::GetLifecycleModule(),
            JsonConfig::GetLifecycleModule(),
            pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetDataBlockModule(),
            pylabhub::hub::GetZMQContextModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        LogCaptureFixture::Install();
        broker_.emplace(start_local_broker({})); // legacy cfg ignored
    }

    void TearDown() override
    {
        broker_.reset();
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return broker_->host->broker(); }

    std::optional<HubHostHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> BrokerAdminTest::s_lifecycle_;

// ============================================================================
// list_channels_json_str tests
// ============================================================================

TEST_F(BrokerAdminTest, ListChannels_Empty)
{
    // No channels registered — expect empty array
    std::string result = svc().list_channels_json_str();
    auto j = json::parse(result);
    ASSERT_TRUE(j.is_array());
    EXPECT_TRUE(j.empty()) << "Expected empty channel list, got: " << result;
}

TEST_F(BrokerAdminTest, ListChannels_OneChannel)
{
    const std::string channel = pid_chan("admin.list.one");

    BrcHandle bh;
    bh.start(ep(), pk(), "prod." + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "prod." + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    std::string result = svc().list_channels_json_str();
    auto j = json::parse(result);
    ASSERT_TRUE(j.is_array());
    ASSERT_GE(j.size(), 1u);

    // Find our channel in the array
    bool found = false;
    for (const auto &entry : j)
    {
        if (entry.value("name", "") == channel)
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Channel '" << channel << "' not found in: " << result;

    bh.stop();
}

TEST_F(BrokerAdminTest, ListChannels_FieldPresence)
{
    const std::string channel = pid_chan("admin.list.fields");

    BrcHandle bh;
    bh.start(ep(), pk(), "prod." + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "prod." + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    std::string result = svc().list_channels_json_str();
    auto j = json::parse(result);

    // Find our channel entry
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

    // Verify required fields are present.  HEP-CORE-0023 §2.2 — channel
    // state surfaces only as `observable` (legacy `status` retired).
    EXPECT_TRUE(entry->contains("name"));
    EXPECT_TRUE(entry->contains("observable"));
    EXPECT_TRUE(entry->contains("consumer_count"));
    EXPECT_TRUE(entry->contains("producer_pid"));

    bh.stop();
}

// ============================================================================
// query_channel_snapshot tests
// ============================================================================

TEST_F(BrokerAdminTest, Snapshot_Empty)
{
    ChannelSnapshot snap = svc().query_channel_snapshot();
    EXPECT_TRUE(snap.channels.empty());
}

TEST_F(BrokerAdminTest, Snapshot_OneChannel)
{
    const std::string channel = pid_chan("admin.snap.one");

    BrcHandle bh;
    bh.start(ep(), pk(), "prod." + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "prod." + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    ChannelSnapshot snap = svc().query_channel_snapshot();
    ASSERT_GE(snap.channels.size(), 1u);

    // Find our channel
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
}

TEST_F(BrokerAdminTest, Snapshot_AfterConsumer)
{
    const std::string channel = pid_chan("admin.snap.consumer");

    // Register producer
    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), "prod." + channel);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, "prod." + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    // Send heartbeat so broker marks channel Ready (required for consumer registration)
    prod_bh.brc.send_heartbeat(channel, "prod." + channel, "producer", {});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Register consumer (separate BRC instance)
    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), "cons." + channel);
    auto cons_reg = cons_bh.brc.register_consumer(
        make_cons_opts(channel, "cons." + channel), 3000);
    ASSERT_TRUE(cons_reg.has_value()) << "register_consumer failed";

    ChannelSnapshot snap = svc().query_channel_snapshot();
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
}

// ============================================================================
// request_close_channel tests
// ============================================================================

TEST_F(BrokerAdminTest, CloseChannel_Existing)
{
    // Atomic teardown per HEP-CORE-0023 §2.1 — voluntary close emits a
    // best-effort CHANNEL_CLOSING_NOTIFY then immediately removes the
    // channel entry; no grace window, no FORCE_SHUTDOWN escalation.
    const std::string channel = pid_chan("admin.close.existing");

    BrcHandle bh;
    bh.start(ep(), pk(), "prod." + channel);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, "prod." + channel), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    // Request close
    svc().request_close_channel(channel);

    // Atomic teardown per HEP-CORE-0023 §2.1 — the channel must be gone
    // from the snapshot.  No intermediate Closing state.
    using pylabhub::tests::helper::poll_until;
    auto channel_gone = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel) return false;
        return true;
    };
    EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
        << "Channel still present 3s after request_close_channel";

    bh.stop();
}

TEST_F(BrokerAdminTest, CloseChannel_NonExistent)
{
    // Closing a nonexistent channel should not crash — silently ignored.
    // Because the broker takes no observable action on a bogus name
    // (no callback, no log, no snapshot change), this is a legitimate
    // "give async processing a window, then probe liveness" scenario —
    // the sleep is not ordering events, it's bounding the silent path.
    EXPECT_NO_THROW(svc().request_close_channel(pid_chan("admin.close.bogus")));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Explicit liveness probe: broker must still serve admin requests
    // (replaces the prior `(void)snap` which only caught outright
    // crashes on the snapshot call itself).
    EXPECT_NO_THROW({
        ChannelSnapshot snap = svc().query_channel_snapshot();
        (void)snap.channels.size();  // force materialization
    });
}
