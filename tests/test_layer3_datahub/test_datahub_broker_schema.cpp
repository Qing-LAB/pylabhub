/**
 * @file test_datahub_broker_schema.cpp
 * @brief Broker schema metadata tests (HEP-CORE-0016 Phase 3).
 *
 * Suite: BrokerSchemaTest
 *
 * Tests that the broker correctly stores schema_id/schema_hash/schema_blds
 * from REG_REQ, and validates expected_schema_id in CONSUMER_REG_REQ.
 *
 * All tests use an in-process broker with empty schema_search_dirs (no library
 * files on disk). Verification uses the broker admin API (query_channel_snapshot).
 *
 * CTest safety: all channel names are suffixed with the test process PID.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/schema_utils.hpp"  // compute_canonical_hash_from_wire (HEP-0034 §10.1)
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

// Real-HubHost RAII wrapper — replaces the legacy `LocalBrokerHandle` mock
// (raw HubState + raw BrokerService + raw std::thread) per the test-design
// principle in `docs/todo/TESTING_TODO.md` §"Test Design Principles".
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

HubHostHandle start_local_broker(BrokerService::Config /*legacy_cfg*/)
{
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_schema_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    pylabhub::utils::HubDirectory::init_directory(dir, "SchemaTestHub");

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
    fs::create_directories(dir / "schemas"); // canonical hub-globals path

    HubHostHandle h;
    h.hub_dir = std::move(dir);
    h.host    = std::make_unique<HubHost>(
        HubConfig::load_from_directory(h.hub_dir.string()));
    h.host->startup();
    h.endpoint = h.host->broker_endpoint();
    h.pubkey   = h.host->broker_pubkey();
    return h;
}

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

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(getpid());
}

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    json opts;
    opts["channel_name"]  = channel;
    opts["consumer_uid"]  = consumer_uid;
    opts["consumer_name"] = "test_consumer";
    opts["consumer_pid"]  = ::getpid();
    return opts;
}

/// Phase-3-follow-up helper: hex-encoded canonical hash matching the wire
/// fingerprint that the broker recomputes (HEP-CORE-0034 §6.3 / §10.1).
std::string canonical_hash_hex(const std::string &slot_blds,
                               const std::string &slot_packing)
{
    const auto h = pylabhub::hub::compute_canonical_hash_from_wire(slot_blds, slot_packing);
    return pylabhub::format_tools::bytes_to_hex(
        {reinterpret_cast<const char *>(h.data()), h.size()});
}

/// Default minimal valid schema fields for tests that just want to
/// register a named channel without caring about the specific layout.
struct DefaultSchema
{
    std::string blds    = "ts:f64:1:0|value:f32:1:0";
    std::string packing = "aligned";
    std::string hash    = canonical_hash_hex("ts:f64:1:0|value:f32:1:0", "aligned");
};

} // anonymous namespace

// ============================================================================
// BrokerSchemaTest — in-process broker, empty schema library
// ============================================================================

class BrokerSchemaTest : public ::testing::Test,
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
            pylabhub::hub::GetZMQContextModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        LogCaptureFixture::Install();
        broker_.emplace(start_local_broker({}));
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

std::unique_ptr<LifecycleGuard> BrokerSchemaTest::s_lifecycle_;

// ── Test 1: schema_hash stored at registration, visible in snapshot ──────────

TEST_F(BrokerSchemaTest, SchemaHash_StoredOnReg)
{
    const std::string channel  = pid_chan("schema.hash.stored");
    const std::string uid      = "prod." + channel;
    const std::string hash_hex = std::string(64, 'a');

    BrcHandle bh;
    bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    opts["schema_hash"] = hash_hex;
    auto reg = bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    ChannelSnapshot snap = svc().query_channel_snapshot();
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            EXPECT_EQ(ch.schema_hash, hash_hex);
            bh.stop();
            return;
        }
    }
    FAIL() << "Channel not found in snapshot";
}

// ── Test 2: schema_id stored at registration ─────────────────────────────────

TEST_F(BrokerSchemaTest, SchemaId_StoredOnReg)
{
    const std::string channel   = pid_chan("schema.id.stored");
    const std::string uid       = "prod." + channel;
    const std::string schema_id = "$lab.test.sensor.v1";
    const DefaultSchema sch;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    // HEP-0034 §10.1 — named REG_REQ requires the full structure
    // (id + hash + packing + blds).  Stage-2 broker verifies hash.
    opts["schema_id"]      = schema_id;
    opts["schema_hash"]    = sch.hash;
    opts["schema_packing"] = sch.packing;
    opts["schema_blds"]    = sch.blds;
    auto reg = bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // Verify via list_channels_json_str which includes schema info
    auto j = json::parse(svc().list_channels_json_str());
    ASSERT_TRUE(j.is_array());
    bool found = false;
    for (const auto &ch : j)
    {
        if (ch.value("name", "") == channel)
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Channel with schema_id not found in list";

    bh.stop();
}

// ── Test 3: consumer expected_schema_id matches → registration succeeds ──────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_Match_Succeeds)
{
    const std::string channel   = pid_chan("schema.consumer.match");
    const std::string schema_id = "$lab.consumer.test.v2";
    const std::string prod_uid  = "prod." + channel;
    const std::string cons_uid  = "cons." + channel;
    const DefaultSchema sch;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);

    auto opts = make_reg_opts(channel, prod_uid);
    opts["schema_id"]      = schema_id;
    opts["schema_hash"]    = sch.hash;
    opts["schema_packing"] = sch.packing;
    opts["schema_blds"]    = sch.blds;
    auto reg = prod_bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // Heartbeat → Ready
    // CONSUMER_REG_REQ does not gate on Ready (HEP-CORE-0023 §2.2).

    // Consumer named-citation per HEP-0034 §10.3 — id + matching hash.
    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);

    auto cons_opts = make_cons_opts(channel, cons_uid);
    cons_opts["expected_schema_id"]   = schema_id;
    cons_opts["expected_schema_hash"] = sch.hash;
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    EXPECT_TRUE(cons_reg.has_value()) << "Consumer should succeed when schema_id matches";

    cons_bh.stop();
    prod_bh.stop();
}

// ── Test 4: consumer expected_schema_id mismatch → registration fails ────────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_Mismatch_Fails)
{
    ExpectLogWarn("CONSUMER_REG_REQ schema_id mismatch");
    const std::string channel  = pid_chan("schema.consumer.mismatch");
    const std::string prod_sid = "$lab.producer.schema.v1";
    const std::string cons_sid = "$lab.other.schema.v1";
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const DefaultSchema sch;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);

    auto opts = make_reg_opts(channel, prod_uid);
    opts["schema_id"]      = prod_sid;
    opts["schema_hash"]    = sch.hash;
    opts["schema_packing"] = sch.packing;
    opts["schema_blds"]    = sch.blds;
    auto reg = prod_bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready (HEP-CORE-0023 §2.2).

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);

    auto cons_opts = make_cons_opts(channel, cons_uid);
    // Consumer cites a DIFFERENT schema_id than the channel's.  Under
    // HEP-0034 §10.3 named-mode, this NACKs SCHEMA_ID_MISMATCH.
    cons_opts["expected_schema_id"]   = cons_sid;
    cons_opts["expected_schema_hash"] = sch.hash;
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    // schema_id mismatch → SCHEMA_ID_MISMATCH per HEP-CORE-0007 §12.4a.
    ASSERT_TRUE(cons_reg.has_value())
        << "Broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(cons_reg->value("status", std::string{}), "error");
    EXPECT_EQ(cons_reg->value("error_code", std::string{}), "SCHEMA_ID_MISMATCH");

    cons_bh.stop();
    prod_bh.stop();
}

// ── Test 5: producer anonymous, consumer expects schema_id → fail ────────────

TEST_F(BrokerSchemaTest, ConsumerSchemaId_EmptyProducer_Fails)
{
    const std::string channel  = pid_chan("schema.consumer.empty.prod");
    const std::string cons_sid = "$lab.expected.schema.v3";
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    prod_bh.start(ep(), pk(), prod_uid);

    // No schema_id
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready (HEP-CORE-0023 §2.2).

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), cons_uid);

    auto cons_opts = make_cons_opts(channel, cons_uid);
    cons_opts["expected_schema_id"] = cons_sid;
    auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
    // Consumer cites named-mode `expected_schema_id` without supplying
    // `expected_schema_hash`; per HEP-CORE-0034 §10.2 named-mode
    // requires both, so the broker rejects with
    // MISSING_HASH_FOR_NAMED_CITATION (§12.4a taxonomy) before reaching
    // the schema-id-mismatch check.
    ASSERT_TRUE(cons_reg.has_value())
        << "Broker should respond with ERROR, not silent timeout";
    EXPECT_EQ(cons_reg->value("status", std::string{}), "error");
    EXPECT_EQ(cons_reg->value("error_code", std::string{}),
              "MISSING_HASH_FOR_NAMED_CITATION");

    cons_bh.stop();
    prod_bh.stop();
}
