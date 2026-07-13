/**
 * @file broker_service_workers.cpp
 * @brief Worker bodies for `BrokerService`-touching L2 tests (HEP-CORE-
 *        0040 §172 + HEP-CORE-0043 §1.3).
 *
 * `BrokerService`'s ctor mutates the process-wide `SecureSubsystem`
 * KeyStore (either directly via `seed_curve_identities()` or the
 * `add_identity_from_z85()` shim).  Per HEP-CORE-0043 §1.3, every
 * L2 test that touches a process singleton MUST run its assertion
 * body in a fresh subprocess.  These workers are the subprocess
 * bodies; `test_hub_state.cpp` invokes them via
 * `IsolatedProcessTest::SpawnWorker("broker_service.<scenario>")`.
 *
 * Retirement of the pre-2026-07-13 direct-in-process TEST bodies:
 *   - `TEST(BrokerServicePlumbing, HubStateAccessorReturnsExternalAggregate)`
 *   - `TEST(BrokerServiceCtor, MissingHubIdentityInKeyStoreThrowsLogicError)`
 * Both were incorrectly written as raw TEST(...) with in-process
 * KeyStore mutations, meaning a second test in the same binary
 * inherited the polluted state (`hub_identity` seeded by the first
 * test caused the second test's negative assertion to fail under
 * any invocation that ran multiple tests in one process — e.g. a
 * raw-binary invocation bypassing `gtest_discover_tests`'
 * one-process-per-test isolation).  See
 * `test_curve_test_fixtures.cpp` (lines 18-20) for the same pattern
 * documented against the same singleton.
 */
#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"

#include "curve_test_setup.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;
using pylabhub::utils::security::secure;

namespace pylabhub::tests::worker
{
namespace broker_service
{

// ─── Positive plumbing pin: hub_state() returns the injected reference ──────

int hub_state_accessor_returns_external_aggregate()
{
    return run_gtest_worker(
        [] {
            // Seed KeyStore under the canonical name so BrokerService
            // ctor's `hub_identity` lookup succeeds.  This is the
            // POSITIVE side of the ctor's KeyStore contract; the
            // negative side is `ctor_missing_hub_identity_throws_logic_error`
            // below.  The process-wide KeyStore mutation is safe here
            // because this whole function runs in a fresh subprocess.
            auto setup = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(setup);

            pylabhub::hub::HubState state;
            pylabhub::broker::BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            pylabhub::broker::BrokerService broker(cfg, state);

            const auto &state_ref = broker.hub_state();
            auto        snap      = state_ref.snapshot();
            EXPECT_TRUE(snap.channels.empty());
            EXPECT_TRUE(snap.roles.empty());
            EXPECT_TRUE(snap.bands.empty());
            EXPECT_TRUE(snap.peers.empty());
            EXPECT_TRUE(snap.shm_blocks.empty());

            EXPECT_EQ(&state_ref, &state);
            EXPECT_EQ(&state_ref, &broker.hub_state());
        },
        "broker_service::hub_state_accessor_returns_external_aggregate",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

// ─── #161 Phase 1 mutation pin: ctor must throw when hub_identity is absent ─

int ctor_missing_hub_identity_throws_logic_error()
{
    return run_gtest_worker(
        [] {
            namespace sec = pylabhub::utils::security;

            // Seed an UNRELATED name so the KeyStore is non-empty but
            // the ctor's specific lookup for "hub_identity" misses.
            // Catching "empty KeyStore" as the failure mode would let a
            // future refactor that uses some other name (e.g.
            // "broker_identity") silently pass — this assertion forces
            // the ctor to use the exact HEP-CORE-0040 §172 contract name.
            const auto kp = pylabhub::tests::gen_curve_keypair();
            sec::secure().keys().add_identity_from_z85(
                "not_hub_identity", kp.public_z85, kp.secret_z85);

            pylabhub::hub::HubState state;
            pylabhub::broker::BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";

            try
            {
                pylabhub::broker::BrokerService broker(cfg, state);
                ADD_FAILURE()
                    << "Expected BrokerService ctor to throw "
                       "std::logic_error when 'hub_identity' is "
                       "absent from KeyStore; ctor returned normally.";
            }
            catch (const std::logic_error &e)
            {
                const std::string what = e.what();
                EXPECT_NE(what.find("hub_identity"), std::string::npos)
                    << "Ctor threw, but the message must name the "
                       "missing literal 'hub_identity' so an operator "
                       "can correct the vault / fixture wiring.  Got: "
                    << what;
            }
            catch (const std::exception &e)
            {
                ADD_FAILURE()
                    << "Ctor threw, but the type must be "
                       "std::logic_error (programmer-error contract, "
                       "HEP-CORE-0035 §4.6.5 no-bypass).  Got: "
                    << typeid(e).name() << ": " << e.what();
            }
        },
        "broker_service::ctor_missing_hub_identity_throws_logic_error",
        Logger::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule());
}

} // namespace broker_service
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ─────────────────────────────────────────────────────

namespace
{

struct BrokerServiceRegistrar
{
    BrokerServiceRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_service")
                {
                    return -1;
                }
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_service;

                if (sc == "hub_state_accessor_returns_external_aggregate")
                {
                    return hub_state_accessor_returns_external_aggregate();
                }
                if (sc == "ctor_missing_hub_identity_throws_logic_error")
                {
                    return ctor_missing_hub_identity_throws_logic_error();
                }
                return -1;
            });
    }
} g_registrar;

} // namespace
