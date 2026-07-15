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

// `make_reg_opts` / `make_cons_opts` consolidated into
// `tests/test_framework/broker_test_harness.h` (REVIEW_C2 F10
// 2026-06-29).  Schema fields are layered on top by callers below.

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
            // only reads from secure().keys().
            pylabhub::tests::seed_curve_identities(curve);
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
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
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
                // schema_id_stored_on_reg + consumer_schema_id_* MIGRATED
                // to tests/test_layer3_pattern4/test_pattern4_broker_schema.cpp
                // (task #52 Round 2).
                return -1;
            });
    }
} g_registrar;

} // namespace
