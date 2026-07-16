/**
 * @file broker_admin_workers.cpp
 * @brief Worker bodies for BrokerService admin API tests
 *        (Pattern 3).  Migrated 2026-05-13 from in-process
 *        `SetUpTestSuite`-owned `LifecycleGuard`.
 */

#include "broker_admin_workers.h"

#include "broker_test_harness.h"
#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_sync_utils.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_host.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/security/shm_capability_channel.hpp" // #281 default_shm_capability_endpoint

#include <atomic>
#include <chrono>
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
using pylabhub::tests::helper::poll_until;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace broker_admin
{

namespace
{

json hubhost_overrides()
{
    return json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

// `make_reg_opts` / `make_cons_opts` now live in
// `tests/test_framework/broker_test_harness.h` as the single canonical
// 2-param helpers (REVIEW_C2 F10 consolidation 2026-06-29).

/// Run a worker body with a freshly spun-up HubHostBrokerHandle +
/// LogCaptureFixture under real CURVE + admission (HEP-CORE-0035
/// §2 + §4.6.5).  Body receives (broker, curve).
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
        pylabhub::hub::GetDataBlockModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace


int close_channel_existing()
{
    const std::string channel = pid_chan("admin.close.existing");
    const std::string uid     = "prod." + channel;
    return run_with_host(
        "broker_admin::close_channel_existing", {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            broker.service().request_close_channel(channel);

            auto channel_gone = [&] {
                auto s = broker.service().query_channel_snapshot();
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
        "broker_admin::close_channel_non_existent", {},
        [](pylabhub::tests::HubHostBrokerHandle &broker,
           pylabhub::tests::CurveSetup &) {
            EXPECT_NO_THROW(
                broker.service().request_close_channel(
                    pid_chan("admin.close.bogus")));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            EXPECT_NO_THROW({
                ChannelSnapshot snap = broker.service().query_channel_snapshot();
                (void)snap.channels.size();
            });
        });
}

// ============================================================================
// #281 (2026-06-23) — REG_REQ wire-contract pins for `data_transport`.
// ============================================================================
//
// Six wire-level pins that exercise the broker REG_REQ handler directly via
// `BrokerRequestComm::register_channel` (real CURVE + admission via the
// `run_with_host` harness):
//
//   * Four NEGATIVE pins: REG_REQ payloads that should be REJECTED with
//     INVALID_REQUEST per HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1.  The
//     broker emits a LOGGER_WARN naming the violation; the test harness
//     declares each via `ExpectLogWarn` so the warn is allowlisted (it is
//     EXPECTED, not collateral noise).  Without `ExpectLogWarn`, the
//     harness's end-of-test `AssertNoUnexpectedLogWarnError` would fail
//     even though the response shape is correct.
//
//   * Two POSITIVE pins: the canonical SHM and ZMQ wire shapes that
//     production producers emit (status="success").
//
// Why this lives in `broker_admin_workers.cpp`: the file already wires
// `run_with_host` (CURVE harness + KeyStore fixture) and `make_reg_opts`
// helper, and BrokerAdminTest is the natural fixture home for "broker
// rejects malformed wire shape" coverage.  The L3 broker test ladder
// (rungs 2/3 under Pattern4*) targets handshake / heartbeat / lifecycle
// flows, not REG_REQ field-level wire validation — so the pins live here
// rather than expanding the rung set.

namespace {

/// Build a baseline SHM REG_REQ payload (data_transport="shm" + valid
/// shm_capability_endpoint) that satisfies the broker's strict checks.
/// Tests then erase / mutate specific fields to exercise each rejection
/// branch.  Mirrors `make_reg_opts` shape but exposed locally so the
/// tests don't accidentally depend on a future change to that helper.
json make_baseline_shm_reg(const std::string &channel,
                           const std::string &role_uid,
                           const std::string &zmq_pubkey)
{
    return pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "reg_validation_producer",
            .role_type   = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = zmq_pubkey,
            .shm_capability_endpoint =
                pylabhub::utils::security::default_shm_capability_endpoint(channel),
        });
}

/// Build a baseline ZMQ REG_REQ payload.
json make_baseline_zmq_reg(const std::string &channel,
                           const std::string &role_uid,
                           const std::string &zmq_pubkey)
{
    return pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "reg_validation_producer",
            .role_type   = "producer",
            .has_shm    = false,
            .is_zmq_transport  = true,
            .zmq_node_endpoint = "tcp://127.0.0.1:0",
            .zmq_pubkey = zmq_pubkey,
            .shm_capability_endpoint = {},
        });
}

} // anonymous namespace




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

                // list_channels_* / snapshot_* / reg_validation_*_success
                // MIGRATED to Pattern 4 (task #52 Round 3 — read
                // CHANNEL_LIST_REQ / DISC_REQ over the wire).  All
                // reg_validation error paths migrated in Round 2.
                if (sc == "close_channel_existing")
                    return close_channel_existing();
                if (sc == "close_channel_non_existent")
                    return close_channel_non_existent();
                return -1;
            });
    }
} g_registrar;

} // namespace
