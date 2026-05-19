/**
 * @file broker_schema_workers.cpp
 * @brief Worker bodies for broker named-schema protocol tests
 *        (HEP-CORE-0034 path B; Pattern 3).  Migrated 2026-05-13 from
 *        in-process `SetUpTestSuite`-owned `LifecycleGuard`.
 */

#include "broker_schema_workers.h"

#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/schema_utils.hpp"

#include <atomic>
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
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pylabhub::tests::worker
{
namespace broker_schema
{

namespace
{

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l3_schema_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

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
    fs::create_directories(dir / "schemas");
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

std::string canonical_hash_hex(const std::string &slot_blds,
                               const std::string &slot_packing)
{
    const auto h = pylabhub::hub::compute_canonical_hash_from_wire(slot_blds,
                                                                    slot_packing);
    return pylabhub::format_tools::bytes_to_hex(
        {reinterpret_cast<const char *>(h.data()), h.size()});
}

struct DefaultSchema
{
    std::string blds    = "ts:f64:1:0|value:f32:1:0";
    std::string packing = "aligned";
    std::string hash    = canonical_hash_hex("ts:f64:1:0|value:f32:1:0", "aligned");
};

/// Run a worker body with a freshly spun-up HubHost + LogCaptureFixture.
template <typename Body>
int run_with_host(const char *tag, std::string_view worker_name,
                  Body &&body,
                  std::vector<std::string> expect_log_warns = {})
{
    return run_gtest_worker(
        [tag, body = std::forward<Body>(body),
         expect_log_warns = std::move(expect_log_warns)]() mutable {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : expect_log_warns)
                log_cap.ExpectLogWarn(w);

            const fs::path dir = unique_temp_dir(tag);
            write_test_hub_json(dir, "SchemaTestHub");
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
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

int schema_hash_stored_on_reg()
{
    return run_with_host(
        "SchemaHashStoredOnReg",
        "broker_schema::schema_hash_stored_on_reg",
        [](HubHost &host) {
            const std::string channel  = pid_chan("schema.hash.stored");
            const std::string uid      = "prod." + channel;
            const std::string hash_hex = std::string(64, 'a');

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);

            auto opts           = make_reg_opts(channel, uid);
            opts["schema_hash"] = hash_hex;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            ChannelSnapshot snap = host.broker().query_channel_snapshot();
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
        });
}

int schema_id_stored_on_reg()
{
    return run_with_host(
        "SchemaIdStoredOnReg",
        "broker_schema::schema_id_stored_on_reg",
        [](HubHost &host) {
            const std::string channel   = pid_chan("schema.id.stored");
            const std::string uid       = "prod." + channel;
            const std::string schema_id = "$lab.test.sensor.v1";
            const DefaultSchema sch;

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);

            auto opts              = make_reg_opts(channel, uid);
            opts["schema_id"]      = schema_id;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            auto j = json::parse(host.broker().list_channels_json_str());
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
        });
}

int consumer_schema_id_match_succeeds()
{
    return run_with_host(
        "ConsumerSchemaIdMatchSucceeds",
        "broker_schema::consumer_schema_id_match_succeeds",
        [](HubHost &host) {
            const std::string channel   = pid_chan("schema.consumer.match");
            const std::string schema_id = "$lab.consumer.test.v2";
            const std::string prod_uid  = "prod." + channel;
            const std::string cons_uid  = "cons." + channel;
            const DefaultSchema sch;

            BrcHandle prod_bh;
            prod_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          prod_uid);

            auto opts              = make_reg_opts(channel, prod_uid);
            opts["schema_id"]      = schema_id;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          cons_uid);

            auto cons_opts                    = make_cons_opts(channel, cons_uid);
            cons_opts["expected_schema_id"]   = schema_id;
            cons_opts["expected_schema_hash"] = sch.hash;
            auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
            EXPECT_TRUE(cons_reg.has_value())
                << "Consumer should succeed when schema_id matches";

            cons_bh.stop();
            prod_bh.stop();
        });
}

int consumer_schema_id_mismatch_fails()
{
    return run_with_host(
        "ConsumerSchemaIdMismatchFails",
        "broker_schema::consumer_schema_id_mismatch_fails",
        [](HubHost &host) {
            const std::string channel  = pid_chan("schema.consumer.mismatch");
            const std::string prod_sid = "$lab.producer.schema.v1";
            const std::string cons_sid = "$lab.other.schema.v1";
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;
            const DefaultSchema sch;

            BrcHandle prod_bh;
            prod_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          prod_uid);

            auto opts              = make_reg_opts(channel, prod_uid);
            opts["schema_id"]      = prod_sid;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          cons_uid);

            auto cons_opts                    = make_cons_opts(channel, cons_uid);
            cons_opts["expected_schema_id"]   = cons_sid;
            cons_opts["expected_schema_hash"] = sch.hash;
            auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
            ASSERT_TRUE(cons_reg.has_value())
                << "Broker should respond with ERROR, not silent timeout";
            EXPECT_EQ(cons_reg->value("status", std::string{}), "error");
            EXPECT_EQ(cons_reg->value("error_code", std::string{}),
                      "SCHEMA_ID_MISMATCH");

            cons_bh.stop();
            prod_bh.stop();
        },
        {"CONSUMER_REG_REQ schema_id mismatch"});
}

int consumer_schema_id_empty_producer_fails()
{
    return run_with_host(
        "ConsumerSchemaIdEmptyProducerFails",
        "broker_schema::consumer_schema_id_empty_producer_fails",
        [](HubHost &host) {
            const std::string channel  = pid_chan("schema.consumer.empty.prod");
            const std::string cons_sid = "$lab.expected.schema.v3";
            const std::string prod_uid = "prod." + channel;
            const std::string cons_uid = "cons." + channel;

            BrcHandle prod_bh;
            prod_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          prod_uid);

            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons_bh;
            cons_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          cons_uid);

            auto cons_opts                  = make_cons_opts(channel, cons_uid);
            cons_opts["expected_schema_id"] = cons_sid;
            auto cons_reg = cons_bh.brc.register_consumer(cons_opts, 3000);
            ASSERT_TRUE(cons_reg.has_value())
                << "Broker should respond with ERROR, not silent timeout";
            EXPECT_EQ(cons_reg->value("status", std::string{}), "error");
            EXPECT_EQ(cons_reg->value("error_code", std::string{}),
                      "MISSING_HASH_FOR_NAMED_CITATION");

            cons_bh.stop();
            prod_bh.stop();
        });
}

} // namespace broker_schema
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct BrokerSchemaRegistrar
{
    BrokerSchemaRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_schema")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_schema;

                if (sc == "schema_hash_stored_on_reg")
                    return schema_hash_stored_on_reg();
                if (sc == "schema_id_stored_on_reg")
                    return schema_id_stored_on_reg();
                if (sc == "consumer_schema_id_match_succeeds")
                    return consumer_schema_id_match_succeeds();
                if (sc == "consumer_schema_id_mismatch_fails")
                    return consumer_schema_id_mismatch_fails();
                if (sc == "consumer_schema_id_empty_producer_fails")
                    return consumer_schema_id_empty_producer_fails();
                return -1;
            });
    }
} g_registrar;

} // namespace
