/**
 * @file hub_host_integration_workers.cpp
 * @brief Worker bodies for HubHost ↔ BrokerService L3 integration tests
 *        (Pattern 3).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Each scenario constructs `HubHost`
 * (spawning a `BrokerService` thread) and a `BrokerRequestComm` client
 * — transitively touches Logger / FileLock / JsonConfig / SecureSubsystem /
 * ZMQContext; Pattern 3 isolation required per README_testing.md.
 *
 * ── RATIONALE — HubHostBrokerHandle sweep disposition (task #52) ─────────────
 * Both workers KEEP the in-process co-host, INTRINSICALLY: the subject under
 * test IS `HubHost` composing + spawning its `BrokerService` (reachable after
 * startup; REG_REQ round-trips via the spawned broker).  You cannot test
 * HubHost integration without instantiating HubHost, so a subprocess-broker
 * Pattern 4 harness would test a different thing entirely.  One `HubHost`
 * broker = one ZAP pump; a single `BrokerRequestComm` client (DEALER, no ZAP
 * pump) — not the HEP-CORE-0036 §7.4 single-pumper antipattern.
 */

#include "hub_host_integration_workers.h"

#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/security/shm_capability_channel.hpp"

#include "broker_test_harness.h"
#include "log_capture_fixture.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace pylabhub::utils;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace hub_host_integration
{

namespace
{

// HEP-CORE-0036 §5b + #281 canonical REG_REQ wire shape — mirrors the
// canonical `make_reg_opts` builder in
// `tests/test_framework/broker_test_harness.h`.  Broker hard-errors on
// missing `role_type` / `data_transport` / pubkey fields per
// `apply_*_reg_ack` invariants.
json make_reg_opts(const std::string &channel, const pylabhub::tests::CurveSetup &curve,
                   const std::string &uid)
{
    auto opts = pylabhub::hub::build_producer_reg_payload(pylabhub::hub::ProducerRegInputs{
        .channel = channel,
        .role_uid = uid,
        .role_name = "test_producer",
        .role_type = "producer",
        .has_shm = true,
        .is_zmq_transport = false,
        .zmq_node_endpoint = {},
        .zmq_pubkey = curve.role(uid).public_z85,
        .shm_capability_endpoint =
            pylabhub::utils::security::default_shm_capability_endpoint(channel),
    });
    opts["producer_pid"] = ::getpid();
    return opts;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

// Shared hub.json overrides for HubHost integration fixtures
// (ephemeral broker port, admin off, no script).
json hubhost_overrides()
{
    return json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin", {{"enabled", false}}},
        {"script", {{"path", ""}}},
    };
}

} // namespace

int hubhost_brokerreachable_afterstartup()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const std::string uid = "prod.test.uid_reachable";
            auto curve = pylabhub::tests::make_curve_setup({uid});
            pylabhub::tests::seed_role_identities(curve);
            auto broker = pylabhub::tests::start_hubhost_broker(hubhost_overrides(), curve);
            ASSERT_FALSE(broker.endpoint.empty())
                << "HubHost::broker_endpoint() empty after startup — "
                   "bind didn't happen";

            pylabhub::tests::BrcHandle client;
            client.start(broker.endpoint, broker.pubkey, uid,
                         pylabhub::tests::role_keystore_name(uid));

            client.stop();
            broker.stop_and_join();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_host_integration::hubhost_brokerreachable_afterstartup", Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(), JsonConfig::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int hubhost_regreq_roundtripsviaspawnedbroker()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const std::string uid = "prod.cam.uid_regreq";
            const std::string channel = pid_chan("hubhost.regreq");
            auto curve = pylabhub::tests::make_curve_setup({uid});
            pylabhub::tests::seed_role_identities(curve);
            auto broker = pylabhub::tests::start_hubhost_broker(hubhost_overrides(), curve);

            pylabhub::tests::BrcHandle client;
            client.start(broker.endpoint, broker.pubkey, uid,
                         pylabhub::tests::role_keystore_name(uid));

            auto reg = client.brc.register_channel(make_reg_opts(channel, curve, uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "REG_REQ timed out";
            EXPECT_EQ(reg->value("status", ""), "success");

            ASSERT_TRUE(reg->contains("heartbeat"));
            EXPECT_EQ((*reg)["heartbeat"].value("heartbeat_interval_ms", -1),
                      broker.host->config().broker().heartbeat_interval_ms);

            const auto snap = broker.host->state().snapshot();
            EXPECT_TRUE(snap.channels.count(channel) > 0)
                << "channel '" << channel
                << "' missing from HubHost::state() snapshot after REG_ACK";

            client.stop();
            broker.stop_and_join();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_host_integration::hubhost_regreq_roundtripsviaspawnedbroker",
        Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// REMOVED 2026-06-27 — `hubhost_shutdown_breaksclientconnection`.
// The L3 TEST_F that drove it cannot pin its target contract
// (HEP-CORE-0023 §2.5.3 "disconnect is terminal") in-process: under
// shared `pylabhub::hub::get_zmq_context()` libzmq's CURVE engine
// suppresses ZMQ_EVENT_DISCONNECTED on clean peer close — a
// false-negative that cannot be worked around at L3.  The contract
// holds cross-process; L4 replacement task tracked in
// `docs/todo/TESTING_TODO.md`.  Full closure analysis archived at
// `docs/archive/transient-2026-06-05/todo-completions/AUTH_TODO_completions.md`.

} // namespace hub_host_integration
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct HubHostIntegrationRegistrar
{
    HubHostIntegrationRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "hub_host_integration")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_host_integration;

                if (sc == "hubhost_brokerreachable_afterstartup")
                    return hubhost_brokerreachable_afterstartup();
                if (sc == "hubhost_regreq_roundtripsviaspawnedbroker")
                    return hubhost_regreq_roundtripsviaspawnedbroker();
                return -1;
            });
    }
} g_registrar;

} // namespace
