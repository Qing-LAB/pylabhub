/**
 * @file test_datahub_broker_consumer.cpp
 * @brief Consumer registration protocol integration tests.
 *
 * Tests the CONSUMER_REG_REQ / CONSUMER_DEREG_REQ broker protocol and
 * the consumer_count field in DISC_ACK, via real `HubHost` +
 * `BrokerRequestComm`.
 *
 * Pattern: in-process (Pattern 2 — `::testing::Test` + suite-level
 * `LifecycleGuard`).  All five tests exercise individual API calls
 * (CONSUMER_REG_REQ / CONSUMER_DEREG_REQ / DISC_REQ round-trips); none
 * test lifecycle behavior, so per the test-design principle in
 * `docs/todo/TESTING_TODO.md` §"Test Design Principles" they don't
 * need subprocess isolation.
 *
 * M1 of the broker test migration plan
 * (`docs/tech_draft/broker_test_migration_plan.md`) — first file
 * migrated off the `BrokerHandle` mock-host scaffolding.  The pattern
 * established here is the template for M2.
 */
#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"

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
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{

// ─── Test-config helpers (real HubConfig from real on-disk hub dir) ──────────

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l3_broker_consumer_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

/// Initialise a real hub directory and patch hub.json for in-process L3
/// broker testing — loopback ephemeral port, no CURVE auth, no admin,
/// no script.  Same shape as `test_hub_host.cpp::write_test_hub_json`.
void write_test_hub_json(const fs::path &dir, const std::string &name)
{
    fs::create_directories(dir);
    HubDirectory::init_directory(dir, name);

    const fs::path hub_json = dir / "hub.json";
    json j;
    {
        std::ifstream f(hub_json);
        ASSERT_TRUE(f.is_open()) << "test could not open " << hub_json;
        j = json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
}

// ─── BrokerRequestComm RAII helper (real production class) ──────────────────
//
// Thin wrapper that connects a `BrokerRequestComm`, runs its poll loop on
// a dedicated thread, and joins on destruction.  This is NOT a mock — it
// drives the real `BrokerRequestComm` exactly as production roles do
// (poll loop owned by a thread the role host launches).  Same shape as
// `test_datahub_broker_admin.cpp::BrcHandle`.

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

// ─── Payload helpers (production-shape JSON for register_channel /
//    register_consumer; same fields the role hosts populate) ──────────

json make_reg_opts(const std::string &channel, const std::string &role_uid,
                   uint64_t producer_pid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = producer_pid;
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

json make_cons_opts(const std::string &channel, const std::string &consumer_uid,
                    uint64_t consumer_pid)
{
    json opts;
    opts["channel_name"]   = channel;
    opts["consumer_uid"]   = consumer_uid;
    opts["consumer_name"]  = "test_consumer";
    opts["consumer_pid"]   = consumer_pid;
    return opts;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

} // anonymous namespace

// ─── Fixture: real HubHost owns broker thread via ThreadManager ──────────────

class DatahubBrokerConsumerTest : public ::testing::Test,
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
            pylabhub::hub::GetZMQContextModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        LogCaptureFixture::Install();
        const fs::path dir = unique_temp_dir(::testing::UnitTest::GetInstance()
                                                  ->current_test_info()->name());
        paths_to_clean_.push_back(dir);
        write_test_hub_json(dir, "TestHub");
        host_ = std::make_unique<HubHost>(HubConfig::load_from_directory(dir.string()));
        host_->startup();
        ASSERT_TRUE(host_->is_running());
    }

    void TearDown() override
    {
        host_.reset(); // ~HubHost calls ThreadManager::drain → bounded join
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    const std::string &ep() const { return host_->broker_endpoint(); }
    const std::string &pk() const { return host_->broker_pubkey(); }

    std::unique_ptr<HubHost> host_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
    std::vector<fs::path>                  paths_to_clean_;
};

std::unique_ptr<LifecycleGuard> DatahubBrokerConsumerTest::s_lifecycle_;

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST_F(DatahubBrokerConsumerTest, ConsumerRegChannelNotFound)
{
    // CONSUMER_REG_REQ for an unknown channel → CHANNEL_NOT_FOUND error.
    // Broker logs LOGGER_WARN only; declared as expected.
    ExpectLogWarn("CONSUMER_REG_REQ failed");

    BrcHandle bh;
    bh.start(ep(), pk(), "cons.unknown.uid001");

    auto resp = bh.brc.register_consumer(
        make_cons_opts(pid_chan("consumer.no_such_channel"), "cons.unknown.uid001", 12345),
        3000);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->value("status", std::string{}), "error");
    EXPECT_EQ(resp->value("error_code", std::string{}), "CHANNEL_NOT_FOUND");
}

TEST_F(DatahubBrokerConsumerTest, ConsumerRegHappyPath)
{
    const std::string channel = pid_chan("consumer.reg_happy");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod;
    prod.start(ep(), pk(), prod_uid);
    auto reg = prod.brc.register_channel(make_reg_opts(channel, prod_uid, 55001), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";
    ASSERT_EQ(reg->value("status", std::string{}), "success");

    // Producer must heartbeat before DISC_REQ resolves to DISC_ACK
    // (HEP-CORE-0023 §2.2 — producer-presence needs first_heartbeat_seen).
    prod.brc.send_heartbeat(channel, prod_uid, "producer", json::object());

    BrcHandle cons;
    cons.start(ep(), pk(), cons_uid);
    auto creg = cons.brc.register_consumer(
        make_cons_opts(channel, cons_uid, 55100), 3000);
    ASSERT_TRUE(creg.has_value());
    EXPECT_EQ(creg->value("status", std::string{}), "success");

    // DISC_ACK must show consumer_count >= 1.
    auto disc = cons.brc.discover_channel(channel, json::object(), 3000);
    ASSERT_TRUE(disc.has_value());
    EXPECT_EQ(disc->value("status", std::string{}), "success");
    EXPECT_GE(disc->value("consumer_count", uint32_t{0}), 1u);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerDeregHappyPath)
{
    const std::string channel = pid_chan("consumer.dereg_happy");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    constexpr uint64_t cons_pid = 55100;

    BrcHandle prod;
    prod.start(ep(), pk(), prod_uid);
    ASSERT_TRUE(prod.brc.register_channel(make_reg_opts(channel, prod_uid, 55001), 3000)
                    .has_value());
    prod.brc.send_heartbeat(channel, prod_uid, "producer", json::object());

    BrcHandle cons;
    cons.start(ep(), pk(), cons_uid);
    ASSERT_TRUE(cons.brc.register_consumer(make_cons_opts(channel, cons_uid, cons_pid), 3000)
                    .has_value());

    // consumer_count == 1 after register
    auto disc1 = cons.brc.discover_channel(channel, json::object(), 3000);
    ASSERT_TRUE(disc1.has_value());
    EXPECT_EQ(disc1->value("consumer_count", uint32_t{99}), 1u);

    // Deregister consumer (correct uid → success)
    EXPECT_TRUE(cons.brc.deregister_consumer(channel, 3000));

    // consumer_count == 0 after deregister
    auto disc2 = cons.brc.discover_channel(channel, json::object(), 3000);
    ASSERT_TRUE(disc2.has_value());
    EXPECT_EQ(disc2->value("consumer_count", uint32_t{99}), 0u);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerDeregPidMismatch)
{
    // Deregister with a uid that never registered → NOT_REGISTERED.
    // Broker logs LOGGER_WARN only.
    ExpectLogWarn("CONSUMER_DEREG_REQ failed");

    const std::string channel = pid_chan("consumer.dereg_pid_mismatch");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid_correct = "cons." + channel + ".correct";
    const std::string cons_uid_wrong   = "cons." + channel + ".wrong";

    BrcHandle prod;
    prod.start(ep(), pk(), prod_uid);
    ASSERT_TRUE(prod.brc.register_channel(make_reg_opts(channel, prod_uid, 56000), 3000)
                    .has_value());
    prod.brc.send_heartbeat(channel, prod_uid, "producer", json::object());

    BrcHandle cons_correct;
    cons_correct.start(ep(), pk(), cons_uid_correct);
    ASSERT_TRUE(cons_correct.brc.register_consumer(
                       make_cons_opts(channel, cons_uid_correct, 56001), 3000)
                    .has_value());

    // Mismatched dereg sender → broker rejects.
    BrcHandle cons_wrong;
    cons_wrong.start(ep(), pk(), cons_uid_wrong);
    EXPECT_FALSE(cons_wrong.brc.deregister_consumer(channel, 3000));

    // Original consumer still registered (consumer_count remains 1).
    auto disc = cons_correct.brc.discover_channel(channel, json::object(), 3000);
    ASSERT_TRUE(disc.has_value());
    EXPECT_EQ(disc->value("consumer_count", uint32_t{0}), 1u);
}

TEST_F(DatahubBrokerConsumerTest, DiscShowsConsumerCount)
{
    // DISC_ACK consumer_count: 0 initially → 1 after register → 0 after dereg.
    const std::string channel = pid_chan("consumer.disc_count");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod;
    prod.start(ep(), pk(), prod_uid);
    ASSERT_TRUE(prod.brc.register_channel(make_reg_opts(channel, prod_uid, 57000), 3000)
                    .has_value());
    prod.brc.send_heartbeat(channel, prod_uid, "producer", json::object());

    BrcHandle observer;
    observer.start(ep(), pk(), "observer." + channel);

    auto disc0 = observer.brc.discover_channel(channel, json::object(), 3000);
    ASSERT_TRUE(disc0.has_value());
    EXPECT_EQ(disc0->value("consumer_count", uint32_t{99}), 0u);

    BrcHandle cons;
    cons.start(ep(), pk(), cons_uid);
    ASSERT_TRUE(cons.brc.register_consumer(make_cons_opts(channel, cons_uid, 57100), 3000)
                    .has_value());

    auto disc1 = observer.brc.discover_channel(channel, json::object(), 3000);
    ASSERT_TRUE(disc1.has_value());
    EXPECT_EQ(disc1->value("consumer_count", uint32_t{99}), 1u);

    EXPECT_TRUE(cons.brc.deregister_consumer(channel, 3000));

    auto disc2 = observer.brc.discover_channel(channel, json::object(), 3000);
    ASSERT_TRUE(disc2.has_value());
    EXPECT_EQ(disc2->value("consumer_count", uint32_t{99}), 0u);
}
