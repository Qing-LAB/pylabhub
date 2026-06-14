/**
 * @file pattern4_registration_workers.cpp
 * @brief Subprocess workers for Pattern 4 rung 2 — Pattern4RegistrationTest.
 *
 * Two scenarios:
 *
 *   - `pattern4_registration.broker` — runs a BrokerService that
 *     accepts a producer-side REG_REQ; the parent pins the broker-side
 *     INFO markers `Broker: REG_REQ accepted ...` + `Broker: REG_ACK
 *     sending ...` via the shared log stream.
 *
 *   - `pattern4_registration.producer_role` — constructs a
 *     RoleAPIBase + RoleHandler stack, opens a CURVE-authenticated
 *     BRC against the broker, calls `register_producer_channel`, and
 *     observes the Presence FSM transitions
 *     `Unregistered → RegRequestPending → Registered`.
 *
 * Both subprocesses redirect their Logger sink to
 * `setup.shared_log_path` via `pylabhub::tests::pattern4::set_shared_log`
 * immediately after the LifecycleGuard brings up the Logger module.
 * The parent reads the merged log stream with `expect_log_sequence`
 * for cross-process order pinning (file-position IS time-order under
 * `O_APPEND` — see docs/README/README_testing.md § Pattern 4
 * "Verification — log-driven sequence assertion").
 *
 * See docs/README/README_testing.md § "Pattern 4 — ... — Test ladder"
 * rung 2 for the contract this worker pair pins.
 */
#include "pattern4_helpers.h"

#include "curve_test_setup.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_handler.hpp"
#include "utils/role_host_core.hpp"
#include "utils/role_uid.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_memory_subsystem.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace pylabhub::tests::pattern4
{

namespace
{

// Bind-retry parameters per Pattern 4 doc § "Bind robustness".
constexpr int  kBindMaxAttempts    = 4;
constexpr auto kBindInitialBackoff = std::chrono::milliseconds{10};

// Safety timeout for the broker subprocess — fires ONLY if the parent
// crashed or forgot to call `signal_quit()`.  Normal path: parent calls
// `broker.signal_quit()` after its assertions pass, the read() on
// PLH_TEST_QUIT_FD returns EOF, and the broker shuts down in <1 s.
//
// 60 s is generous enough that even a fully-loaded -j 2 CI machine
// won't false-trigger; the cost is only paid on actual parent failure.
constexpr auto kBrokerSafetyTimeout = std::chrono::seconds{60};

// Switch the Logger sink to the shared log iff the parent provided one.
// Empty path means rung-1 smoke-style per-stderr verification.
void maybe_redirect_to_shared_log(const Pattern4Setup &setup)
{
    if (!setup.shared_log_path.empty())
        set_shared_log(setup.shared_log_path);
}

} // anon

// ─── pattern4_registration.broker ───────────────────────────────────────────

int pattern4_registration_broker(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");

            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "registration.broker", setup.curve);

            maybe_redirect_to_shared_log(setup);

            pylabhub::broker::BrokerService::Config cfg;
            cfg.endpoint = setup.broker_endpoint;
            pylabhub::tests::apply_curve_to(cfg, setup.curve);

            std::promise<std::string> ready_promise;
            auto                       ready_future = ready_promise.get_future();
            cfg.on_ready = [&ready_promise](const std::string &ep,
                                             const std::string &pk) {
                LOGGER_INFO("Pattern4Broker: bound endpoint='{}' pubkey='{}'",
                            ep, pk);
                ready_promise.set_value(ep);
            };

            pylabhub::hub::HubState                            state;
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
                        LOGGER_WARN("Pattern4Broker: bind EADDRINUSE on "
                                    "endpoint='{}' - retrying in {}ms "
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
                << "Pattern4Broker: failed to bind after "
                << kBindMaxAttempts << " attempts on '"
                << cfg.endpoint << "'";

            std::thread broker_thread([&] { broker->run(); });

            ASSERT_EQ(ready_future.wait_for(std::chrono::seconds{2}),
                      std::future_status::ready)
                << "Pattern4Broker: on_ready callback did not fire within 2s";

            LOGGER_INFO("Pattern4Broker: waiting on quit-signal pipe "
                        "(safety timeout {} s)",
                        kBrokerSafetyTimeout.count());
            const auto wait_result =
                pylabhub::tests::helper::wait_for_quit_or_safety_timeout(
                    kBrokerSafetyTimeout);
            switch (wait_result)
            {
            case pylabhub::tests::helper::QuitWaitResult::QuitSignal:
                LOGGER_INFO("Pattern4Broker: quit signal received, stopping");
                break;
            case pylabhub::tests::helper::QuitWaitResult::SafetyTimeout:
                LOGGER_WARN("Pattern4Broker: safety timeout fired — parent "
                            "did not call signal_quit() within {} s",
                            kBrokerSafetyTimeout.count());
                break;
            case pylabhub::tests::helper::QuitWaitResult::NoQuitPipe:
                // Spawned without with_quit_signal=true — fall back to a
                // short sleep so the worker doesn't immediately exit
                // before the parent has finished reading the shared log.
                LOGGER_WARN("Pattern4Broker: no PLH_TEST_QUIT_FD — falling "
                            "back to 3 s sleep before exit");
                std::this_thread::sleep_for(std::chrono::seconds{3});
                break;
            }

            broker->stop();
            broker_thread.join();
            LOGGER_INFO("Pattern4Broker: exiting cleanly");
        },
        "pattern4_registration.broker",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── pattern4_registration.producer_role ────────────────────────────────────

int pattern4_registration_producer_role(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");
            // Constructed via `make_role_uid` — single source of
            // truth for HEP-CORE-0033 §G2.2.0b grammar; the broker's
            // wire-boundary validator uses the same predicate, so a
            // uid that survives this construction cannot be rejected
            // for grammar reasons.
            const std::string role_uid = pylabhub::scripting::make_role_uid(
                pylabhub::scripting::RoleUidTag::Producer,
                "pattern4reg", 1u);
            const std::string channel = "reg.test";

            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "registration.producer_role", setup.curve);

            // CurveKeyStoreFixture seeds identities under "role.<uid>"
            // (test convention for multi-role-per-process); production
            // BRC looks up the singular "role_identity" name per
            // HEP-CORE-0040 §172 + key_store.hpp kRoleIdentityName.
            // Pattern 4 is one-role-per-process (production shape), so
            // also seed the singular name with this role's keypair.
            pylabhub::tests::CurveKeyStoreFixture::add_identity(
                pylabhub::utils::security::kRoleIdentityName,
                setup.curve.role_keys.at(role_uid));

            maybe_redirect_to_shared_log(setup);

            // RoleAPIBase + RoleHandler scaffolding — same shape as
            // existing L3 tests (datahub_role_state_workers.cpp:935).
            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);
            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("pattern4_registration");
            api.set_channel(channel);

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker        = setup.broker_endpoint;
            hub_cfg.broker_pubkey = setup.curve.hub.public_z85;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = channel;
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            auto handler =
                std::make_unique<pylabhub::scripting::RoleHandler>(
                    std::move(presences));

            ASSERT_TRUE(api.start_handler_threads(std::move(handler)))
                << "Pattern4Role: start_handler_threads must succeed "
                   "against the broker subprocess";
            ASSERT_NE(api.handler(), nullptr);

            // REG_REQ payload — production shape per HEP-CORE-0036 §5.
            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
            reg_opts["role_uid"]          = role_uid;
            reg_opts["role_name"]         = "pattern4_registration";
            reg_opts["zmq_pubkey"]        =
                setup.curve.role_keys.at(role_uid).public_z85;

            // Use kMidTimeoutMs (not kLongTimeoutMs) — REG_REQ over
            // local loopback completes in milliseconds when the broker
            // is healthy; a 60s wait just slows down the failure
            // diagnostic when something is wrong.
            auto reg_resp = api.register_producer_channel(
                reg_opts, pylabhub::kMidTimeoutMs);
            ASSERT_TRUE(reg_resp.has_value())
                << "Pattern4Role: REG_REQ must reach broker + return ACK "
                   "(timeout/disconnect would point to broker bind or "
                   "CURVE handshake failure)";
            ASSERT_EQ(reg_resp->value("status", std::string{}), "success")
                << "Pattern4Role: REG_ACK status must be success — "
                   "got error_code='"
                << reg_resp->value("error_code", std::string{}) << "'";

            // Hold the role open briefly so the broker's REG_ACK
            // marker has time to land in the shared log before the
            // shutdown teardown overlaps with subsequent rung tests.
            std::this_thread::sleep_for(std::chrono::milliseconds{200});

            api.stop_handler_threads();
            LOGGER_INFO("Pattern4Role[{}]: exiting cleanly", role_uid);
        },
        "pattern4_registration.producer_role",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── Dispatcher registration ────────────────────────────────────────────────

int dispatch_pattern4_registration(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    const std::string mode = argv[1];
    const auto        dot  = mode.find('.');
    if (dot == std::string::npos)
        return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "pattern4_registration")
        return -1;

    if (argc < 3)
    {
        std::fprintf(stderr,
                     "pattern4_registration.%s: missing <temp_dir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *temp_dir = argv[2];

    if (scenario == "broker")
        return pattern4_registration_broker(temp_dir);
    if (scenario == "producer_role")
        return pattern4_registration_producer_role(temp_dir);

    std::fprintf(stderr,
                 "pattern4_registration: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct Pattern4RegistrationRegistrar
{
    Pattern4RegistrationRegistrar()
    {
        ::register_worker_dispatcher(dispatch_pattern4_registration);
    }
};

static Pattern4RegistrationRegistrar g_pattern4_registration_registrar;

} // namespace pylabhub::tests::pattern4
