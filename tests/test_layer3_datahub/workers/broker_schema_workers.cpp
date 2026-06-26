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
#include "utils/role_reg_payload.hpp"
#include "utils/schema_utils.hpp"
#include "utils/security/shm_capability_channel.hpp" // #281 default_shm_capability_endpoint

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

// Thin wrappers around the canonical production payload builders
// (`pylabhub::hub::build_*_reg_payload` in `utils/role_reg_payload.hpp`).
// Schema fields are layered on top by callers below.
json make_reg_opts(const std::string &channel, const std::string &role_uid,
                   const std::string &zmq_pubkey)
{
    return pylabhub::hub::build_producer_reg_payload(
        // #281 (2026-06-23): `data_transport` is REQUIRED on REG_REQ
        // (broker rejects missing/empty).  Mirror production
        // producer_role_host's SHM wire shape — `has_shm=true` + canonical
        // endpoint via `default_shm_capability_endpoint`
        // (HEP-CORE-0041 §5.1).  No L2 listener is bound; broker only
        // stores the endpoint string.
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "test_producer",
            .role_type   = "producer",
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
            // HEP-CORE-0040 §172: fixture owns SMS + KeyStore + identity
            // seeding (the production-shaped path); start_hubhost_broker
            // only reads from key_store().
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test.l3", "test.broker_schema.harness", curve);
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
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto opts           = make_reg_opts(channel, uid, curve.role(uid).public_z85);
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
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto opts              = make_reg_opts(channel, uid, curve.role(uid).public_z85);
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
                          pylabhub::tests::role_keystore_name(prod_uid));

            auto opts              = make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85);
            opts["schema_id"]      = schema_id;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          pylabhub::tests::role_keystore_name(cons_uid));

            auto cons_opts                    = make_cons_opts(channel, cons_uid, curve.role(cons_uid).public_z85);
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
                          pylabhub::tests::role_keystore_name(prod_uid));

            auto opts              = make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85);
            opts["schema_id"]      = prod_sid;
            opts["schema_hash"]    = sch.hash;
            opts["schema_packing"] = sch.packing;
            opts["schema_blds"]    = sch.blds;
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            // Channel-Ready precondition: the broker enters
            // CONSUMER_REG's schema-match check only after the channel
            // is Ready (HEP-CORE-0019 §4.1 Phase 6).  One heartbeat
            // transitions producer presence Connected→Registered →
            // channel Ready.  Without this, the consumer reg fails
            // with CHANNEL_NOT_READY before the schema mismatch check
            // ever runs.
            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds{50});

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          pylabhub::tests::role_keystore_name(cons_uid));

            auto cons_opts                    = make_cons_opts(channel, cons_uid, curve.role(cons_uid).public_z85);
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
                          pylabhub::tests::role_keystore_name(prod_uid));

            auto reg = prod_bh.brc.register_channel(
                make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85), 3000);
            ASSERT_TRUE(reg.has_value());

            // Channel-Ready precondition — see schema_id_mismatch_fails
            // for the rationale.
            prod_bh.brc.send_heartbeat(channel, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds{50});

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          pylabhub::tests::role_keystore_name(cons_uid));

            auto cons_opts                  = make_cons_opts(channel, cons_uid, curve.role(cons_uid).public_z85);
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
