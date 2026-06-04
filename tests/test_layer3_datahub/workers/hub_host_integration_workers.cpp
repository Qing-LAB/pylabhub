/**
 * @file hub_host_integration_workers.cpp
 * @brief Worker bodies for HubHost ↔ BrokerService L3 integration tests
 *        (Pattern 3).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Each scenario constructs `HubHost`
 * (spawning a `BrokerService` thread) and a `BrokerRequestComm` client
 * — transitively touches Logger / FileLock / JsonConfig / CryptoUtils /
 * ZMQContext; Pattern 3 isolation required per README_testing.md.
 */

#include "hub_host_integration_workers.h"

#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"

#include "broker_test_harness.h"
#include "log_capture_fixture.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace pylabhub::utils;
using namespace pylabhub::broker;
using pylabhub::hub::BrokerRequestComm;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace hub_host_integration
{

namespace
{

json make_reg_opts(const std::string &channel, const std::string &uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = uid;
    opts["role_name"]         = "test_producer";
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
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
}

} // namespace

int hubhost_brokerreachable_afterstartup()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const std::string uid = "prod.test.uid_reachable";
            auto curve = pylabhub::tests::make_curve_setup({uid});
            auto broker = pylabhub::tests::start_hubhost_broker(
                hubhost_overrides(), curve);
            ASSERT_FALSE(broker.endpoint.empty())
                << "HubHost::broker_endpoint() empty after startup — "
                   "bind didn't happen";

            pylabhub::tests::BrcHandle client;
            client.start(broker.endpoint, broker.pubkey, uid,
                         curve.role(uid));

            client.stop();
            broker.stop_and_join();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_host_integration::hubhost_brokerreachable_afterstartup",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int hubhost_regreq_roundtripsviaspawnedbroker()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            const std::string uid     = "prod.cam.uid_regreq";
            const std::string channel = pid_chan("hubhost.regreq");
            auto curve = pylabhub::tests::make_curve_setup({uid});
            auto broker = pylabhub::tests::start_hubhost_broker(
                hubhost_overrides(), curve);

            pylabhub::tests::BrcHandle client;
            client.start(broker.endpoint, broker.pubkey, uid,
                         curve.role(uid));

            auto reg = client.brc.register_channel(
                make_reg_opts(channel, uid), 3000);
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
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

int hubhost_shutdown_breaksclientconnection()
{
    return run_gtest_worker(
        [] {
            LogCaptureFixture log_cap;
            log_cap.Install();

            log_cap.ExpectLogWarn("REG_REQ timed out");
            // Audit S1 (2026-05-18) — after DISCONNECTED, the BRC's
            // layer-1 connected-state gate suppresses subsequent
            // sends to the dead broker (skips libzmq send entirely
            // since reconnect_ivl=-1 means no future peer).
            // HEP-CORE-0023 §2.5.3.
            log_cap.ExpectLogWarn("suppressed — connection terminally dead");
            // After A2 (Wave-B M8 prep): BRC's socket monitor is now
            // polled as a first-class poll item (was a side-effect of
            // DEALER traffic), so ZMQ_EVENT_DISCONNECTED is detected
            // promptly when host->shutdown() closes the broker socket.
            // The hub-dead WARN was previously suppressed by the
            // detection delay; now it fires within this test's window.
            // Audit S1 (2026-05-18) — log line text updated to reflect
            // the "disconnect is terminal" policy (HEP-CORE-0023
            // §2.5.3); pin the stable substring "ZMQ_EVENT_DISCONNECTED"
            // which is present in both the old and new formats.
            log_cap.ExpectLogWarn("ZMQ_EVENT_DISCONNECTED");

            const std::string uid = "prod.test.uid_shutdown";
            auto curve = pylabhub::tests::make_curve_setup({uid});
            auto broker = pylabhub::tests::start_hubhost_broker(
                hubhost_overrides(), curve);

            // Use the production-shaped signal: BRC's `on_hub_dead`
            // callback fires when the socket monitor observes
            // ZMQ_EVENT_DISCONNECTED (HEP-CORE-0023 §2.5.3 terminal
            // disconnect).  Registering it BEFORE the BRC connects
            // ensures the handler is in place by the time the
            // monitor first reports an event.  The promise gives us
            // an event-driven sync — no polling loop required.
            std::promise<void> hub_dead_promise;
            auto hub_dead_future = hub_dead_promise.get_future();
            std::atomic<bool> hub_dead_fired{false};
            pylabhub::tests::BrcHandle client;
            client.brc.on_hub_dead([&] {
                if (!hub_dead_fired.exchange(true))
                    hub_dead_promise.set_value();
            });
            client.start(broker.endpoint, broker.pubkey, uid,
                         curve.role(uid));

            auto reg = client.brc.register_channel(
                make_reg_opts(pid_chan("hubhost.shutdown.precheck"), uid),
                3000);
            ASSERT_TRUE(reg.has_value());

            broker.host->shutdown();
            EXPECT_FALSE(broker.host->is_running());

            // Audit S1 (2026-05-18) — wait via signal for the BRC's
            // monitor to observe ZMQ_EVENT_DISCONNECTED.  Timeout is
            // a safety net, not the measurement.
            ASSERT_EQ(hub_dead_future.wait_for(std::chrono::seconds{5}),
                      std::future_status::ready)
                << "BRC on_hub_dead did not fire within 5s of broker "
                   "shutdown — monitor poll is not detecting the TCP "
                   "close (HEP-CORE-0023 §2.5.3 violation).";
            // Once on_hub_dead fired, is_connected MUST be false (the
            // same monitor handler that fires on_hub_dead also flips
            // the connected_ flag).
            EXPECT_FALSE(client.brc.is_connected())
                << "Audit S1: BRC's is_connected() MUST be false "
                   "immediately after on_hub_dead fires (same handler "
                   "writes both).";
            EXPECT_TRUE(client.brc.reconnect_disabled())
                << "Audit S1: BRC's DEALER socket MUST have "
                   "ZMQ_RECONNECT_IVL=-1 — pylabhub policy 'disconnect "
                   "is terminal' (HEP-CORE-0023 §2.5.3).";

            // Second register_channel after broker death — should
            // return failure FAST because the layer-1 gate suppresses
            // the send immediately.  Pre-S1 (with reconnect_ivl=100ms
            // + no gate) this would block in libzmq waiting for a
            // peer-that-never-comes; the BRC poll loop would be
            // pinned in send_multipart for the full request timeout.
            const auto t0 = std::chrono::steady_clock::now();
            auto reg2 = client.brc.register_channel(
                make_reg_opts(pid_chan("hubhost.shutdown.postcheck"),
                               "prod.test.uid_shutdown"),
                3000);
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    elapsed).count();
            EXPECT_FALSE(reg2.has_value())
                << "REG_REQ unexpectedly succeeded after host->shutdown()";
            // Layer-1 path-pin: register_channel must time out at the
            // application-level 3000ms (no ACK ever comes because the
            // send was suppressed).  This pins the END-OF-PATH; the
            // SUPPRESS log line (whitelisted via ExpectLogWarn above)
            // pins the START-OF-PATH.
            EXPECT_LT(elapsed_ms, 3500)
                << "register_channel took " << elapsed_ms
                << "ms — must complete near the 3000ms application "
                   "timeout (request never sent, ACK never came).  "
                   "Significant overshoot would indicate the BRC poll "
                   "thread got stuck somewhere — pre-S1 mode.";

            client.stop();
            // broker.stop_and_join() is a no-op here — host already
            // shut down above; harness dtor cleans hub_dir on scope exit.

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        "hub_host_integration::hubhost_shutdown_breaksclientconnection",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

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
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "hub_host_integration")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_host_integration;

                if (sc == "hubhost_brokerreachable_afterstartup")
                    return hubhost_brokerreachable_afterstartup();
                if (sc == "hubhost_regreq_roundtripsviaspawnedbroker")
                    return hubhost_regreq_roundtripsviaspawnedbroker();
                if (sc == "hubhost_shutdown_breaksclientconnection")
                    return hubhost_shutdown_breaksclientconnection();
                return -1;
            });
    }
} g_registrar;

} // namespace
