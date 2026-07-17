/**
 * @file pattern4_broker_protocol_workers.cpp
 * @brief Subprocess broker for Pattern 4 broker-protocol tests.
 *
 * One entry point: `pattern4_broker_protocol.broker` — runs a
 * `BrokerService` against the port + CURVE bundle the parent wrote to
 * setup.json, retries bind on `EADDRINUSE`, waits for the parent's
 * quit signal, exits cleanly.
 *
 * The parent test drives the wire directly via `BrokerWireClient` (no
 * role subprocess).  This is Round 1 of the HubHostBrokerHandle
 * antipattern sweep (see docs/README/README_testing.md line 565 +
 * HEP-CORE-0036 §7.4 single-pumper invariant).
 *
 * Config flexibility: this worker accepts a `<profile>` argv[2]
 * placeholder ("default" for now).  Future tests needing custom broker
 * config (e.g., ChecksumRepairPolicy) plug in a new profile branch
 * rather than growing another worker binary.
 */
#include "pattern4_helpers.h"

#include "broker_wire_client.h"
#include "curve_test_setup.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/zmq_context.hpp"

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace pylabhub::tests::pattern4
{

namespace
{

// Safety timeout — mirrors pattern4_smoke_workers.cpp.  60 s tolerates
// -j 2 CI load without false-tripping; normal path is < 1 s once the
// parent calls signal_quit().
constexpr auto kBrokerProtocolSafetyTimeout = std::chrono::seconds{60};

// Bind-retry parameters per README_testing.md § "Bind robustness".
constexpr int  kBindMaxAttempts    = 4;
constexpr auto kBindInitialBackoff = std::chrono::milliseconds{10};

}  // namespace

int pattern4_broker_protocol_broker(const char *temp_dir_arg,
                                    const char *profile_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path    temp_dir = temp_dir_arg;
            const std::string profile  = profile_arg ? profile_arg : "default";
            const auto        setup =
                read_pattern4_setup(temp_dir / "setup.json");

            // HEP-CORE-0040 §172: seed KeyStore with hub identity + all
            // role identities.  seed_curve_identities is idempotent for
            // the broker (it only reads hub_identity + the ZAP
            // allowlist).
            pylabhub::tests::seed_curve_identities(setup.curve);

            pylabhub::broker::BrokerService::Config cfg;
            cfg.endpoint = setup.broker_endpoint;
            pylabhub::tests::apply_curve_to(cfg, setup.curve);
            // Config profiles: a migrated test that needs a non-default
            // broker config picks a named profile via argv[3] instead of
            // spawning its own broker binary.  Each branch sets only the
            // fields that test exercises; everything else stays default.
            if (profile == "default")
            {
                // Canonical broker config — the common case.
            }
            else if (profile == "hb_custom")
            {
                // reg_ack_heartbeat_block_honors_custom_config — the
                // REG_ACK heartbeat block must echo these negotiated
                // values (HEP-CORE-0023 §2.5.1).
                cfg.heartbeat_interval      = std::chrono::milliseconds{250};
                cfg.ready_miss_heartbeats   = 12;
                cfg.pending_miss_heartbeats = 8;
            }
            else if (profile == "checksum_notify")
            {
                // checksum_error_report_forwarded_to_producer — broker
                // forwards CHECKSUM_ERROR_REPORT as CHANNEL_EVENT_NOTIFY
                // rather than attempting repair (HEP-CORE-0019 Cat2).
                cfg.checksum_repair_policy =
                    pylabhub::broker::ChecksumRepairPolicy::NotifyOnly;
            }
            else if (profile == "fast_reclaim")
            {
                // producer_gets_closing_notify — short ready+pending
                // timeouts so a silent producer is demoted → terminated →
                // CHANNEL_CLOSING_NOTIFY well within the test window.
                cfg.ready_timeout_override   = std::chrono::milliseconds{500};
                cfg.pending_timeout_override = std::chrono::milliseconds{500};
            }
            else if (profile == "long_reclaim")
            {
                // producer_auto_deregisters — long timeouts so the test
                // verifies an explicit DEREG fired (channel reusable
                // immediately), not that a sweep evicted the channel.
                cfg.ready_timeout_override   = std::chrono::milliseconds{15000};
                cfg.pending_timeout_override = std::chrono::milliseconds{15000};
            }
            else if (profile == "consumer_timeout")
            {
                // ConsumerHeartbeatTimeout — heartbeat timeout is the
                // sole consumer-liveness mechanism; short ready/pending
                // so a silent consumer is reclaimed within the test
                // window.
                cfg.ready_timeout_override   = std::chrono::milliseconds{500};
                cfg.pending_timeout_override = std::chrono::milliseconds{500};
            }
            else
            {
                std::fprintf(stderr,
                             "pattern4_broker_protocol.broker: unknown "
                             "profile '%s'\n",
                             profile.c_str());
                FAIL() << "unknown profile: " << profile;
                return;
            }

            std::promise<std::string> ready_promise;
            auto                      ready_future = ready_promise.get_future();
            cfg.on_ready = [&ready_promise](const std::string &ep,
                                             const std::string &pk) {
                LOGGER_INFO("Pattern4BrokerProtocol: bound endpoint='{}' "
                            "pubkey='{}'",
                            ep, pk);
                ready_promise.set_value(ep);
            };

            pylabhub::hub::HubState                          state;
            std::unique_ptr<pylabhub::broker::BrokerService> broker;
            auto backoff = kBindInitialBackoff;
            for (int attempt = 1; attempt <= kBindMaxAttempts; ++attempt)
            {
                try
                {
                    broker = std::make_unique<
                        pylabhub::broker::BrokerService>(cfg, state);
                    break;
                }
                catch (const zmq::error_t &e)
                {
                    if (e.num() == EADDRINUSE && attempt < kBindMaxAttempts)
                    {
                        LOGGER_WARN("Pattern4BrokerProtocol: bind EADDRINUSE on "
                                    "endpoint='{}' — retrying in {}ms "
                                    "(attempt {}/{})",
                                    cfg.endpoint, backoff.count(),
                                    attempt, kBindMaxAttempts);
                        std::this_thread::sleep_for(backoff);
                        backoff *= 5;
                        continue;
                    }
                    throw;
                }
            }
            ASSERT_TRUE(broker)
                << "Pattern4BrokerProtocol: failed to bind after "
                << kBindMaxAttempts << " attempts on '" << cfg.endpoint << "'";

            std::thread broker_thread([&] { broker->run(); });

            ASSERT_EQ(ready_future.wait_for(std::chrono::seconds{2}),
                      std::future_status::ready)
                << "Pattern4BrokerProtocol: on_ready callback did not fire "
                   "within 2s";

            LOGGER_INFO("Pattern4BrokerProtocol: waiting on quit-signal pipe "
                        "(safety timeout {} s)",
                        kBrokerProtocolSafetyTimeout.count());
            const auto wait_result =
                pylabhub::tests::helper::wait_for_quit_or_safety_timeout(
                    kBrokerProtocolSafetyTimeout);
            switch (wait_result)
            {
            case pylabhub::tests::helper::QuitWaitResult::QuitSignal:
                LOGGER_INFO("Pattern4BrokerProtocol: quit signal received, "
                            "stopping");
                break;
            case pylabhub::tests::helper::QuitWaitResult::SafetyTimeout:
                LOGGER_WARN("Pattern4BrokerProtocol: safety timeout fired — "
                            "parent did not call signal_quit() within {} s",
                            kBrokerProtocolSafetyTimeout.count());
                break;
            case pylabhub::tests::helper::QuitWaitResult::NoQuitPipe:
                LOGGER_WARN("Pattern4BrokerProtocol: no PLH_TEST_QUIT_FD — "
                            "falling back to 3 s sleep before exit");
                std::this_thread::sleep_for(std::chrono::seconds{3});
                break;
            }

            broker->stop();
            broker_thread.join();
            LOGGER_INFO("Pattern4BrokerProtocol: exiting cleanly");
        },
        "pattern4_broker_protocol.broker",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// (Removed 2026-07-17: the `dying_consumer` worker + `DeadConsumerDetected`
// exercised the broker's PID-liveness sweep, deleted along with that
// mechanism — heartbeat timeout (HEP-CORE-0023 §2.1) is now the sole
// consumer-liveness path.  See the `consumer_timeout` profile +
// `ConsumerHeartbeatTimeout_*` for the surviving reclaim coverage.)

int dispatch_pattern4_broker_protocol(int argc, char **argv)
{
    if (argc < 2) return -1;
    const std::string mode = argv[1];
    const auto        dot  = mode.find('.');
    if (dot == std::string::npos) return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "pattern4_broker_protocol") return -1;

    if (argc < 3)
    {
        std::fprintf(stderr,
                     "pattern4_broker_protocol.%s: missing <temp_dir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *temp_dir = argv[2];

    const char *profile = argc >= 4 ? argv[3] : "default";
    if (scenario == "broker")
        return pattern4_broker_protocol_broker(temp_dir, profile);

    std::fprintf(stderr,
                 "pattern4_broker_protocol: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct Pattern4BrokerProtocolRegistrar
{
    Pattern4BrokerProtocolRegistrar()
    {
        ::register_worker_dispatcher(dispatch_pattern4_broker_protocol);
    }
};

static Pattern4BrokerProtocolRegistrar g_pattern4_broker_protocol_registrar;

}  // namespace pylabhub::tests::pattern4
