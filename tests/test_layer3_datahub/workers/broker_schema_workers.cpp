/**
 * @file broker_schema_workers.cpp
 * @brief Worker bodies for broker named-schema protocol tests
 *        (HEP-CORE-0034 path B; Pattern 3).  Migrated 2026-05-13 from
 *        in-process `SetUpTestSuite`-owned `LifecycleGuard`.
 */

#include "broker_schema_workers.h"

#include "broker_test_harness.h"
#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/format_tools.hpp"
#include "utils/hub_host.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/schema_utils.hpp"

#include <atomic>
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
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace broker_schema
{

namespace
{

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

json hubhost_overrides()
{
    return json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
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

/// Run a worker body with a freshly spun-up HubHostBrokerHandle +
/// LogCaptureFixture under real CURVE + admission (HEP-CORE-0035
/// §2 + §4.6.5).  Body receives (broker, curve) — see other
/// migrated workers for the pattern.
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
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

int schema_hash_stored_on_reg()
{
    const std::string channel = pid_chan("schema.hash.stored");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_schema::schema_hash_stored_on_reg",
        {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            const std::string hash_hex = std::string(64, 'a');

            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

            auto opts           = make_reg_opts(channel, uid);
            opts["schema_hash"] = hash_hex;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            ChannelSnapshot snap = broker.service().query_channel_snapshot();
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
    const std::string channel = pid_chan("schema.id.stored");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_schema::schema_id_stored_on_reg",
        {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            const std::string schema_id = "$lab.test.sensor.v1";
            const DefaultSchema sch;

            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

            auto opts              = make_reg_opts(channel, uid);
            opts["schema_id"]      = schema_id;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            auto j = json::parse(broker.service().list_channels_json_str());
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
    const std::string channel  = pid_chan("schema.consumer.match");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_schema::consumer_schema_id_match_succeeds",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const std::string schema_id = "$lab.consumer.test.v2";
            const DefaultSchema sch;

            pylabhub::tests::BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid,
                          curve.role(prod_uid));

            auto opts              = make_reg_opts(channel, prod_uid);
            opts["schema_id"]      = schema_id;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          curve.role(cons_uid));

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
    const std::string channel  = pid_chan("schema.consumer.mismatch");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_schema::consumer_schema_id_mismatch_fails",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const std::string prod_sid = "$lab.producer.schema.v1";
            const std::string cons_sid = "$lab.other.schema.v1";
            const DefaultSchema sch;

            pylabhub::tests::BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid,
                          curve.role(prod_uid));

            auto opts              = make_reg_opts(channel, prod_uid);
            opts["schema_id"]      = prod_sid;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          curve.role(cons_uid));

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
    const std::string channel  = pid_chan("schema.consumer.empty.prod");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_schema::consumer_schema_id_empty_producer_fails",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const std::string cons_sid = "$lab.expected.schema.v3";

            pylabhub::tests::BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid,
                          curve.role(prod_uid));

            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          curve.role(cons_uid));

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
