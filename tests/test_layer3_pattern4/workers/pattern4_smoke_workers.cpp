/**
 * @file pattern4_smoke_workers.cpp
 * @brief Subprocess worker entry points for the Pattern 4 smoke test
 *        (task #220 reference implementation).
 *
 * Two scenarios:
 *
 *   - `pattern4_smoke.broker` — runs a `BrokerService` against the
 *     port + CURVE bundle the parent wrote to setup.json, retries
 *     bind on `EADDRINUSE`, waits a short self-timeout window, exits.
 *
 *   - `pattern4_smoke.role_x` — runs a `BrokerRequestComm` against
 *     the same setup, performs the CURVE handshake + connect, waits
 *     a short self-timeout window, exits.
 *
 * Neither subprocess implements REG_REQ / heartbeat in this first
 * reference — that gets layered in once the bootstrap path is proven
 * green under `-j 2`.  The smoke test verifies only:
 *
 *   - the parent's port + setup.json plumbing works,
 *   - the broker subprocess binds + the role subprocess connects,
 *   - CURVE/ZAP succeeds across the process boundary,
 *   - both subprocesses shut down cleanly within their self-timeout.
 *
 * Self-timeout is a placeholder for a back-channel quit signal —
 * the `WorkerProcess::with_quit_signal` extension is tracked under
 * task #220 follow-up (Pattern 4 doc § "Quit mechanism").
 *
 * Mandatory KeyStore seeding (HEP-CORE-0040 §172): each subprocess
 * constructs its own `CurveKeyStoreFixture` from the setup bundle
 * BEFORE building the broker / BRC instance.  This mirrors the
 * production constraint that role identity keypairs live only in
 * the process `KeyStore` (locked memory).
 */
#include "pattern4_helpers.h"

#include "curve_test_setup.h"
#include "shared_test_helpers.h"

#include "test_entrypoint.h"

#include "utils/broker_request_comm.hpp"

#include <cstdlib>       // std::getenv (HEP-0032 §8 strict-mode gate)
#include <string_view>
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/security/secure_memory_subsystem.hpp"
#include "utils/security/key_store.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/zmq_context.hpp"

#include <cppzmq/zmq.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace pylabhub::tests::pattern4
{

namespace
{

// Safety timeout — fires ONLY if the parent crashes or forgets to call
// `signal_quit()`.  Normal path: parent calls `signal_quit()` after
// expect_log returns, the read() on PLH_TEST_QUIT_FD returns EOF, and
// the subprocess shuts down in <1 s.  60 s tolerates fully-loaded -j 2
// CI without false-tripping.  See README_testing.md § "Pattern 4 —
// Termination via quit-signal pipe" for the pattern.
constexpr auto kSmokeSafetyTimeout = std::chrono::seconds{60};

// Bind-retry parameters per Pattern 4 doc § "Bind robustness".
constexpr int  kBindMaxAttempts = 4;
constexpr auto kBindInitialBackoff = std::chrono::milliseconds{10};

} // anon

// ─── pattern4_smoke.broker ──────────────────────────────────────────────────

int pattern4_smoke_broker(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");

            // HEP-CORE-0040 §172: seed KeyStore with hub identity
            // before building BrokerService.  CurveKeyStoreFixture
            // also seeds role.* identities — harmless for the broker
            // (it doesn't read them) and matches what test_helpers do.
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "smoke.broker", setup.curve);

            // BrokerService::Config — explicit bind endpoint + admission
            // rules.  Production wire path (CURVE + ZAP unconditional).
            pylabhub::broker::BrokerService::Config cfg;
            cfg.endpoint = setup.broker_endpoint;
            pylabhub::tests::apply_curve_to(cfg, setup.curve);
            // HEP-CORE-0032 §8 — parent test may request strict-mode ABI
            // rejection via env var.  Env var (vs a JSON field) avoids
            // touching the Pattern4Setup schema shared with every other
            // Pattern4 test; only strict-mode L3 tests set this and it's
            // ignored by every other worker binary.
            if (const char *strict = std::getenv("PLH_TEST_STRICT_ABI_MISMATCH");
                strict != nullptr && std::string_view(strict) == "1")
            {
                cfg.strict_abi_mismatch = true;
                LOGGER_INFO("Pattern4Broker: strict_abi_mismatch=true "
                             "(PLH_TEST_STRICT_ABI_MISMATCH=1)");
            }

            // on_ready logs the bound endpoint so the parent can pin
            // it via expect_log.
            std::promise<std::string> ready_promise;
            auto                       ready_future = ready_promise.get_future();
            cfg.on_ready = [&ready_promise](const std::string &ep,
                                             const std::string &pk) {
                LOGGER_INFO("Pattern4Broker: bound endpoint='{}' pubkey='{}'",
                            ep, pk);
                ready_promise.set_value(ep);
            };

            // Retry-on-EADDRINUSE per Pattern 4 doc.
            pylabhub::hub::HubState                       state;
            std::unique_ptr<pylabhub::broker::BrokerService> broker;
            auto                                          backoff =
                kBindInitialBackoff;
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
            ASSERT_TRUE(broker) << "Pattern4Broker: failed to bind after "
                                << kBindMaxAttempts << " attempts on '"
                                << cfg.endpoint << "'";

            // Run broker on its own thread; main thread self-timeouts.
            std::thread broker_thread([&] { broker->run(); });

            // Block until on_ready fires so we know the broker actually
            // bound (vs threw during run() after construction succeeded).
            ASSERT_EQ(ready_future.wait_for(std::chrono::seconds{2}),
                      std::future_status::ready)
                << "Pattern4Broker: on_ready callback did not fire within 2s";

            LOGGER_INFO("Pattern4Broker: waiting on quit-signal pipe "
                        "(safety timeout {} s)",
                        kSmokeSafetyTimeout.count());
            const auto wait_result =
                pylabhub::tests::helper::wait_for_quit_or_safety_timeout(
                    kSmokeSafetyTimeout);
            switch (wait_result)
            {
            case pylabhub::tests::helper::QuitWaitResult::QuitSignal:
                LOGGER_INFO("Pattern4Broker: quit signal received, stopping");
                break;
            case pylabhub::tests::helper::QuitWaitResult::SafetyTimeout:
                LOGGER_WARN("Pattern4Broker: safety timeout fired — parent "
                            "did not call signal_quit() within {} s",
                            kSmokeSafetyTimeout.count());
                break;
            case pylabhub::tests::helper::QuitWaitResult::NoQuitPipe:
                // Legacy callers (no with_quit_signal=true) — short
                // fallback so the broker doesn't exit before the parent
                // has finished reading the shared log.
                LOGGER_WARN("Pattern4Broker: no PLH_TEST_QUIT_FD — falling "
                            "back to 3 s sleep before exit");
                std::this_thread::sleep_for(std::chrono::seconds{3});
                break;
            }

            broker->stop();
            broker_thread.join();
            LOGGER_INFO("Pattern4Broker: exiting cleanly");
        },
        "pattern4_smoke.broker",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── pattern4_smoke.role_x ──────────────────────────────────────────────────

int pattern4_smoke_role_x(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");
            const std::string role_uid = "role.x";

            // HEP-CORE-0040 §172: seed KeyStore with role identity.
            // CurveKeyStoreFixture seeds hub_identity too (harmless on
            // role side — broker never asks).
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "smoke.role_x", setup.curve);

            // BRC config — production shape.
            pylabhub::hub::BrokerRequestComm::Config brc_cfg;
            brc_cfg.broker_endpoint = setup.broker_endpoint;
            brc_cfg.broker_pubkey   = setup.curve.hub.public_z85;
            brc_cfg.keystore_name   =
                pylabhub::tests::role_keystore_name(role_uid);
            brc_cfg.role_uid        = role_uid;

            pylabhub::hub::BrokerRequestComm brc;
            LOGGER_INFO("Pattern4Role[{}]: BRC connecting to '{}'",
                        role_uid, setup.broker_endpoint);
            ASSERT_TRUE(brc.connect(brc_cfg))
                << "Pattern4Role: BRC::connect failed against endpoint='"
                << setup.broker_endpoint << "'";
            LOGGER_INFO("Pattern4Role[{}]: BRC connected", role_uid);

            // Run the BRC poll loop on a separate thread; main thread
            // self-timeouts.
            std::atomic<bool> running{true};
            std::thread       poll_thread([&] {
                brc.run_poll_loop([&] { return running.load(); });
            });

            LOGGER_INFO("Pattern4Role[{}]: waiting on quit-signal pipe "
                        "(safety timeout {} s)",
                        role_uid, kSmokeSafetyTimeout.count());
            const auto wait_result =
                pylabhub::tests::helper::wait_for_quit_or_safety_timeout(
                    kSmokeSafetyTimeout);
            switch (wait_result)
            {
            case pylabhub::tests::helper::QuitWaitResult::QuitSignal:
                LOGGER_INFO("Pattern4Role[{}]: quit signal received, stopping",
                            role_uid);
                break;
            case pylabhub::tests::helper::QuitWaitResult::SafetyTimeout:
                LOGGER_WARN("Pattern4Role[{}]: safety timeout fired — parent "
                            "did not call signal_quit() within {} s",
                            role_uid, kSmokeSafetyTimeout.count());
                break;
            case pylabhub::tests::helper::QuitWaitResult::NoQuitPipe:
                LOGGER_WARN("Pattern4Role[{}]: no PLH_TEST_QUIT_FD — falling "
                            "back to 3 s sleep before exit",
                            role_uid);
                std::this_thread::sleep_for(std::chrono::seconds{3});
                break;
            }

            running.store(false);
            brc.stop();
            poll_thread.join();
            brc.disconnect();
            LOGGER_INFO("Pattern4Role[{}]: exiting cleanly", role_uid);
        },
        "pattern4_smoke.role_x",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── Dispatcher registration ────────────────────────────────────────────────

int dispatch_pattern4_smoke(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    const std::string mode = argv[1];
    const auto        dot  = mode.find('.');
    if (dot == std::string::npos)
        return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "pattern4_smoke")
        return -1;

    if (argc < 3)
    {
        std::fprintf(stderr, "pattern4_smoke.%s: missing <temp_dir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *temp_dir = argv[2];

    if (scenario == "broker")
        return pattern4_smoke_broker(temp_dir);
    if (scenario == "role_x")
        return pattern4_smoke_role_x(temp_dir);

    std::fprintf(stderr, "pattern4_smoke: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct Pattern4SmokeRegistrar
{
    Pattern4SmokeRegistrar()
    {
        ::register_worker_dispatcher(dispatch_pattern4_smoke);
    }
};

static Pattern4SmokeRegistrar g_pattern4_smoke_registrar;

} // namespace pylabhub::tests::pattern4
