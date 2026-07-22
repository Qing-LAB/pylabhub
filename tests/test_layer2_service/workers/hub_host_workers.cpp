/**
 * @file hub_host_workers.cpp
 * @brief Worker bodies for HubHost lifecycle tests
 *        (HEP-CORE-0033 §4 / Phase 6.1b; Pattern 3).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern to the subprocess-isolation model.
 * Bodies transplanted verbatim — the original file already pinned
 * outcome+path+speed on the failure-path tests (see the 2026-05-01
 * lesson at the prior test_hub_host.cpp:305-307); preserving those
 * pins is the migration's primary correctness obligation.
 *
 * Real production wiring per feedback_test_layering_and_no_mocks.md:
 * each test constructs a real `HubHost` from a real
 * `HubConfig::load_from_directory(...)` seeded with a real
 * `HubDirectory::init_directory(...)` — the same wiring path the
 * `plh_hub` binary uses.
 */

#include "hub_host_workers.h"

#include "curve_test_setup.h" // gen_curve_keypair, add_curve_identity
#include "log_capture_fixture.h"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/security/key_store.hpp" // kHubIdentityName

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::HubDirectory;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace hub_host
{

namespace
{

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l2_hubhost_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

/// Initialize a hub directory and patch its hub.json to a loopback /
/// no-CURVE configuration suitable for L2 in-process testing.
/// Transplanted verbatim from the original fixture.
void write_test_hub_json(const fs::path &dir, const std::string &name)
{
    fs::create_directories(dir);
    HubDirectory::init_directory(dir, name);

    const fs::path hub_json = dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json);
        ASSERT_TRUE(f.is_open()) << "test could not open " << hub_json;
        j = nlohmann::json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0"; // ephemeral
    j["admin"]["enabled"] = false;
    // Script-disabled by default: empty path → HubHost::startup() does
    // not construct HubScriptRunner (HEP-CORE-0011 §"Engine Construction
    // Lifecycle"; L2 scope only).
    j["script"]["path"] = "";
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
}

struct ConfiguredDir
{
    HubConfig cfg;
    fs::path dir;
};

/// Seed the hub broker's CURVE identity (`kHubIdentityName`) into the process
/// KeyStore, once per worker subprocess.  HEP-CORE-0035 §2 makes CURVE
/// unconditional, so `HubHost::startup()` → `BrokerService` ctor asserts
/// `secure().keys().has("hub_identity")` and throws on an empty pubkey.  This is
/// the framework's canonical seeding path (same as `AdminService` and
/// `broker_test_harness`), NOT a test bypass — it stands in for the vault the
/// production hub loads its identity from.  Must run after `SecureSubsystem` is
/// up (i.e. inside the worker lambda, which `make_config` is called from).
void seed_hub_identity_once()
{
    static const bool seeded = []
    {
        pylabhub::tests::add_curve_identity(pylabhub::utils::security::kHubIdentityName,
                                            pylabhub::tests::gen_curve_keypair());
        return true;
    }();
    (void)seeded;
}

ConfiguredDir make_config(const char *tag)
{
    seed_hub_identity_once();
    fs::path dir = unique_temp_dir(tag);
    write_test_hub_json(dir, "TestHub");
    return {HubConfig::load_from_directory(dir.string()), std::move(dir)};
}

void remove_tree(const fs::path &p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

/// Lifecycle module list shared by every worker in this TU.  HubHost's
/// startup transitively touches all five: Logger always, FileLock as
/// JsonConfig's dep, JsonConfig for HubConfig::load, SecureSubsystem for
/// the broker CURVE init path (harmless when use_curve=false at
/// runtime), and ZMQContext for the broker's ROUTER socket.
#define PLH_HUB_HOST_MODS                                                                          \
    Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),                                  \
        JsonConfig::GetLifecycleModule(),                                                          \
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),                          \
        pylabhub::hub::GetZMQContextModule()

} // namespace

int construct_without_startup_no_thread_spawn()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto cd = make_config("ctor");
            HubHost host(std::move(cd.cfg));
            EXPECT_FALSE(host.is_running());

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::construct_without_startup_no_thread_spawn", PLH_HUB_HOST_MODS);
}

int broker_hub_state_is_hub_host_hub_state()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            // HEP-CORE-0033 §4 ownership invariant: HubHost owns the
            // HubState by value; the broker holds a non-owning reference
            // to that exact instance.  Single load-bearing assertion of
            // the 6.1a refactor — if the broker ended up with its own
            // copy or a different HubState, role/admin/script reads
            // would diverge from what the broker writes.
            auto cd = make_config("state_identity");
            HubHost host(std::move(cd.cfg));

            host.startup();
            EXPECT_TRUE(host.is_running());

            EXPECT_EQ(&host.state(), &host.broker().hub_state())
                << "HubHost::state() and broker().hub_state() must point at "
                   "the same HubState instance (HEP-CORE-0033 §4 ownership)";

            host.shutdown();
            EXPECT_FALSE(host.is_running());

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::broker_hub_state_is_hub_host_hub_state", PLH_HUB_HOST_MODS);
}

int startup_idempotent()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto cd = make_config("startup_idem");
            HubHost host(std::move(cd.cfg));

            host.startup();
            EXPECT_TRUE(host.is_running());
            EXPECT_NO_THROW(host.startup()); // second call is a no-op
            EXPECT_TRUE(host.is_running());

            host.shutdown();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::startup_idempotent", PLH_HUB_HOST_MODS);
}

int shutdown_idempotent()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto cd = make_config("shutdown_idem");
            HubHost host(std::move(cd.cfg));

            host.startup();
            host.shutdown();
            EXPECT_FALSE(host.is_running());
            EXPECT_NO_THROW(host.shutdown());
            EXPECT_FALSE(host.is_running());

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::shutdown_idempotent", PLH_HUB_HOST_MODS);
}

int run_main_loop_blocks_until_request_shutdown()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto cd = make_config("run_loop");
            HubHost host(std::move(cd.cfg));

            host.startup();

            std::atomic<bool> loop_returned{false};
            std::thread main_thread(
                [&]
                {
                    host.run_main_loop();
                    loop_returned.store(true);
                });

            // Loop should still be blocked.
            std::this_thread::sleep_for(50ms);
            EXPECT_FALSE(loop_returned.load());

            // Wake it up.  Post-D2.3 (Option E follow-up) the
            // request_shutdown contract is the role-side "dumb signal"
            // pattern: flip the shutdown atomic + wake the run_main_loop
            // CV; do NOT actively stop admin/broker.  The synchronous,
            // ordered teardown happens exclusively in `shutdown()` on
            // the main thread (HEP-CORE-0033 §4.2: runner first, then
            // admin, then broker).  At L2 we verify the API contract
            // end-to-end: signal arrives, run_main_loop returns,
            // follow-up shutdown() completes cleanly.
            host.request_shutdown();

            if (main_thread.joinable())
                main_thread.join();
            EXPECT_TRUE(loop_returned.load());

            host.shutdown();
            EXPECT_FALSE(host.is_running());

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::run_main_loop_blocks_until_request_shutdown", PLH_HUB_HOST_MODS);
}

int startup_fails_cleanly_on_busy_port()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            // First host binds an ephemeral port; we capture it as the
            // busy address and point a second host at the same
            // endpoint.  The second startup() must throw and leave the
            // second HubHost in a clean state — no leaked broker
            // thread, no detached worker.
            auto cd1 = make_config("busy_first");
            HubHost host1(std::move(cd1.cfg));
            host1.startup();
            const std::string busy_endpoint = host1.broker_endpoint();
            ASSERT_FALSE(busy_endpoint.empty());

            auto cd2 = make_config("busy_second");
            {
                nlohmann::json j;
                const auto hub_json = cd2.dir / "hub.json";
                {
                    std::ifstream f(hub_json);
                    j = nlohmann::json::parse(f);
                }
                j["network"]["broker_endpoint"] = busy_endpoint;
                {
                    std::ofstream f(hub_json);
                    f << j.dump(2);
                }
                cd2.cfg = HubConfig::load_from_directory(cd2.dir.string());
            }

            HubHost host2(std::move(cd2.cfg));

            // Outcome + path + speed.  Throw alone is not sufficient —
            // a regression that breaks broker exception-forwarding
            // falls back to the 5 s ready_future timeout, and a plain
            // EXPECT_THROW would still pass (just 5 s later).  Pin
            // BOTH the speed (<1 s) and the path (NOT the timeout
            // message).  This is the 2026-05-01 lesson from the
            // original test_hub_host.cpp:305-307.
            bool threw = false;
            std::string err_what;
            const auto t0 = std::chrono::steady_clock::now();
            try
            {
                host2.startup();
            }
            catch (const std::exception &e)
            {
                threw = true;
                err_what = e.what();
            }
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count();

            EXPECT_TRUE(threw) << "second startup() on busy endpoint must throw";
            EXPECT_LT(elapsed_ms, 1000)
                << "startup() must fail FAST on bind error (<1 s); took " << elapsed_ms
                << " ms.  Slow failure indicates broker "
                   "exception is no longer being forwarded via "
                   "ready_promise — see the lambda in HubHost::startup() "
                   "that wraps broker.run() in a try/catch and calls "
                   "ready_promise->set_exception().";
            EXPECT_EQ(err_what.find("did not signal ready within 5s"), std::string::npos)
                << "startup() failed via the timeout path, not the "
                   "bind-error path; what(): "
                << err_what;
            EXPECT_FALSE(host2.is_running()) << "after failed startup, is_running must be false";

            host1.shutdown();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd1.dir);
            remove_tree(cd2.dir);
        },
        "hub_host::startup_fails_cleanly_on_busy_port", PLH_HUB_HOST_MODS);
}

int destructor_cleans_up_even_without_explicit_shutdown()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto cd = make_config("dtor");
            {
                HubHost host(std::move(cd.cfg));
                host.startup();
                EXPECT_TRUE(host.is_running());
                // Skip explicit shutdown — destructor must run it.
            }
            // Test passes if we reach here without hangs / crashes /
            // detached threads.  The bounded join in ThreadManager
            // catches any leak.

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::destructor_cleans_up_even_without_explicit_shutdown", PLH_HUB_HOST_MODS);
}

int config_accessor_returns_loaded_value()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            auto cd = make_config("cfg_access");
            const std::string original_uid = cd.cfg.identity().uid;

            HubHost host(std::move(cd.cfg));
            EXPECT_EQ(host.config().identity().uid, original_uid);
            EXPECT_EQ(host.config().identity().name, "TestHub");

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::config_accessor_returns_loaded_value", PLH_HUB_HOST_MODS);
}

int startup_after_shutdown_throws()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            // HubHost FSM single-use rule:
            //   Constructed --startup()--> Running --shutdown()--> ShutDown
            // ShutDown is terminal; second startup() must throw.
            auto cd = make_config("after_shutdown");
            HubHost host(std::move(cd.cfg));

            host.startup();
            host.shutdown();
            EXPECT_FALSE(host.is_running());

            // Pin both the exception type AND a message substring so a
            // regression returning a different std::logic_error (e.g.
            // from a downstream assert) does not silently masquerade as
            // the FSM single-use throw.
            bool threw = false;
            std::string msg;
            try
            {
                host.startup();
            }
            catch (const std::logic_error &e)
            {
                threw = true;
                msg = e.what();
            }
            EXPECT_TRUE(threw) << "second startup() must throw std::logic_error";
            EXPECT_NE(msg.find("after shutdown"), std::string::npos)
                << "wrong logic_error path; what(): " << msg;
            EXPECT_FALSE(host.is_running()) << "rejected startup() must not change FSM state";

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd.dir);
        },
        "hub_host::startup_after_shutdown_throws", PLH_HUB_HOST_MODS);
}

int failed_startup_allows_retry()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();

            // After a failed startup(), the FSM is rolled back to
            // Constructed, so the caller can retry on the same instance
            // once the underlying problem (here: port collision) is
            // fixed.  Counterpart to startup_after_shutdown_throws.
            auto cd1 = make_config("retry_blocker");
            HubHost blocker(std::move(cd1.cfg));
            blocker.startup();
            const std::string busy_endpoint = blocker.broker_endpoint();
            ASSERT_FALSE(busy_endpoint.empty());

            auto cd2 = make_config("retry_target");
            {
                nlohmann::json j;
                const auto hub_json = cd2.dir / "hub.json";
                {
                    std::ifstream f(hub_json);
                    j = nlohmann::json::parse(f);
                }
                j["network"]["broker_endpoint"] = busy_endpoint; // collide
                {
                    std::ofstream f(hub_json);
                    f << j.dump(2);
                }
                cd2.cfg = HubConfig::load_from_directory(cd2.dir.string());
            }

            HubHost host(std::move(cd2.cfg));

            // Outcome + speed + path (same 2026-05-01 lesson).
            bool threw = false;
            std::string err_what;
            const auto t0 = std::chrono::steady_clock::now();
            try
            {
                host.startup();
            }
            catch (const std::exception &e)
            {
                threw = true;
                err_what = e.what();
            }
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count();
            EXPECT_TRUE(threw);
            EXPECT_LT(elapsed_ms, 1000)
                << "startup() must fail FAST on bind error (<1 s); took " << elapsed_ms
                << " ms — broker exception not forwarded "
                   "via ready_promise.  See HubHost::startup() lambda.";
            EXPECT_EQ(err_what.find("did not signal ready within 5s"), std::string::npos)
                << "startup() failed via the timeout path, not the "
                   "bind-error path; what(): "
                << err_what;
            EXPECT_FALSE(host.is_running());

            // Free the busy endpoint and patch host's config to a
            // fresh one.
            blocker.shutdown();
            {
                nlohmann::json j;
                const auto hub_json = cd2.dir / "hub.json";
                {
                    std::ifstream f(hub_json);
                    j = nlohmann::json::parse(f);
                }
                j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
                {
                    std::ofstream f(hub_json);
                    f << j.dump(2);
                }
            }
            // The single-use guarantee bites only on the SAME instance
            // after a SUCCESSFUL shutdown.  This test demonstrates the
            // FSM rollback path: state is back to Constructed after a
            // FAILED startup, so a *fresh* HubHost on the patched
            // config is the right pattern.
            auto cd2_retry = HubConfig::load_from_directory(cd2.dir.string());
            HubHost retry_host(std::move(cd2_retry));
            EXPECT_NO_THROW(retry_host.startup());
            EXPECT_TRUE(retry_host.is_running());
            retry_host.shutdown();

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(cd1.dir);
            remove_tree(cd2.dir);
        },
        "hub_host::failed_startup_allows_retry", PLH_HUB_HOST_MODS);
}

} // namespace hub_host
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct HubHostRegistrar
{
    HubHostRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "hub_host")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_host;

                if (sc == "construct_without_startup_no_thread_spawn")
                    return construct_without_startup_no_thread_spawn();
                if (sc == "broker_hub_state_is_hub_host_hub_state")
                    return broker_hub_state_is_hub_host_hub_state();
                if (sc == "startup_idempotent")
                    return startup_idempotent();
                if (sc == "shutdown_idempotent")
                    return shutdown_idempotent();
                if (sc == "run_main_loop_blocks_until_request_shutdown")
                    return run_main_loop_blocks_until_request_shutdown();
                if (sc == "startup_fails_cleanly_on_busy_port")
                    return startup_fails_cleanly_on_busy_port();
                if (sc == "destructor_cleans_up_even_without_explicit_shutdown")
                    return destructor_cleans_up_even_without_explicit_shutdown();
                if (sc == "config_accessor_returns_loaded_value")
                    return config_accessor_returns_loaded_value();
                if (sc == "startup_after_shutdown_throws")
                    return startup_after_shutdown_throws();
                if (sc == "failed_startup_allows_retry")
                    return failed_startup_allows_retry();
                return -1;
            });
    }
} g_registrar;

} // namespace
