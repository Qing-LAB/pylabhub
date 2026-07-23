/**
 * @file broker_admin_workers.cpp
 * @brief Worker bodies for BrokerService admin API tests
 *        (Pattern 3).  Migrated 2026-05-13 from in-process
 *        `SetUpTestSuite`-owned `LifecycleGuard`.
 *
 * ── RATIONALE — HubHostBrokerHandle sweep disposition (task #52 Round 3) ─────
 * The wire-observable admin tests migrated to Pattern 4
 * (`test_pattern4_broker_admin.cpp`): `reg_validation_*` (REG_REQ field
 * validation) and `list_channels_*` / `snapshot_*` (read `CHANNEL_LIST_REQ`
 * over the wire).  What remains KEEPS the in-process broker:
 *   `close_channel_existing`, `close_channel_non_existent`.
 *   WHY IN-PROCESS BROKER: the STIMULUS is the direct C++ admin API
 *     `broker.service().request_close_channel(...)` — an in-process call with
 *     no wire trigger in this fixture (the admin REP plane is disabled).  The
 *     resulting channel-gone state IS wire-observable (`CHANNEL_LIST_REQ`),
 *     but a Pattern 4 test cannot INVOKE the close without the admin wire.
 *   WHY NOT THE SINGLE-PUMPER ANTIPATTERN: one `HubHost` broker = one ZAP
 *     pump; the client (when present) is a bare `BrcHandle` (no ZAP pump).
 *   READY TO MIGRATE (2026-07-22): both preconditions are now met — the admin
 *     plane is CURVE-secured (HEP-CORE-0033 §11, shipped 2026-07-19) and an
 *     `AdminWireClient` exists (`tests/test_framework/admin_wire_client.h`).
 *     The blocker is cleared; these two `close_channel_*` workers + the 3
 *     admin-triggered sweep tests tracked in AUTH_TODO Line E item (5) can now
 *     re-home to Pattern 4 by driving the close over the admin wire instead of
 *     the in-process `request_close_channel` call.  (Not yet done — mechanical.)
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
        {"admin", {{"enabled", false}}},
        {"script", {{"path", ""}}},
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
int run_with_host(std::string_view worker_name, std::vector<std::string> role_uids, Body &&body,
                  std::vector<std::string> expect_log_warns = {})
{
    return run_gtest_worker(
        [role_uids = std::move(role_uids), body = std::forward<Body>(body),
         expect_log_warns = std::move(expect_log_warns)]() mutable
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : expect_log_warns)
                log_cap.ExpectLogWarn(w);

            auto curve = pylabhub::tests::make_curve_setup(role_uids);
            // HEP-CORE-0040 §172: fixture owns SMS + KeyStore + identity
            // seeding (the production-shaped path); start_hubhost_broker
            // only reads from secure().keys().
            pylabhub::tests::seed_role_identities(curve);
            auto broker = pylabhub::tests::start_hubhost_broker(hubhost_overrides(), curve);
            ASSERT_TRUE(broker.host && broker.host->is_running());

            body(broker, curve);

            broker.stop_and_join();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        std::string(worker_name).c_str(), Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::hub::GetDataBlockModule(), pylabhub::hub::GetZMQContextModule());
}

} // namespace

// RATIONALE (task #52 Round 3, KEEP): in-process `request_close_channel`
// stimulus (admin wire plane disabled) — no wire trigger.  See file header.
int close_channel_existing()
{
    const std::string channel = pid_chan("admin.close.existing");
    const std::string uid = "prod." + channel;
    return run_with_host("broker_admin::close_channel_existing", {uid},
                         [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                                        pylabhub::tests::CurveSetup & /*curve*/)
                         {
                             pylabhub::tests::BrcHandle bh;
                             bh.start(broker.endpoint, broker.pubkey, uid,
                                      pylabhub::tests::role_keystore_name(uid));
                             auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
                             ASSERT_TRUE(reg.has_value()) << "register_channel failed";

                             broker.service().request_close_channel(channel);

                             auto channel_gone = [&]
                             {
                                 auto s = broker.service().query_channel_snapshot();
                                 for (const auto &ch : s.channels)
                                     if (ch.name == channel)
                                         return false;
                                 return true;
                             };
                             EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
                                 << "Channel still present 3s after request_close_channel";

                             bh.stop();
                         });
}

// RATIONALE (task #52 Round 3, KEEP): in-process `request_close_channel`
// no-throw robustness on an unknown channel — no wire trigger.  See file header.
int close_channel_non_existent()
{
    return run_with_host(
        "broker_admin::close_channel_non_existent", {},
        [](pylabhub::tests::HubHostBrokerHandle &broker, pylabhub::tests::CurveSetup &)
        {
            EXPECT_NO_THROW(broker.service().request_close_channel(pid_chan("admin.close.bogus")));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            EXPECT_NO_THROW({
                ChannelSnapshot snap = broker.service().query_channel_snapshot();
                (void)snap.channels.size();
            });
        });
}

// #281 REG_REQ wire-contract pins (reg_validation_*) MIGRATED to Pattern 4
// (task #52 Round 2, `test_pattern4_broker_admin.cpp` — error paths — and
// Round 3 success paths).  Their local `make_baseline_{shm,zmq}_reg` builders
// were removed here with them (dead code after migration).

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
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "broker_admin")
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
