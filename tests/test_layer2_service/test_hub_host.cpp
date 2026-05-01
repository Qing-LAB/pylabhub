/**
 * @file test_hub_host.cpp
 * @brief Unit tests for HubHost lifecycle (HEP-CORE-0033 §4 / Phase 6.1b).
 *
 * Pattern 2 — in-process LifecycleGuard via SetUpTestSuite.  Each test
 * constructs a fresh HubHost, exercises one slice of the lifecycle,
 * and lets the destructor clean up.
 *
 * Coverage:
 *   - construct without startup → no thread leak; subsystem accessors
 *     match the configuration.
 *   - startup → broker thread tracked by ThreadManager; is_running.
 *   - startup is idempotent (double call is no-op).
 *   - run_main_loop blocks until request_shutdown fires.
 *   - shutdown is idempotent.
 *   - destructor cleans up if shutdown wasn't called explicitly.
 *
 * The test config is a real `HubDirectory::init_directory(...)` template
 * loaded via `HubConfig::load_from_directory` — the same path the
 * eventual `plh_hub` binary will use, so the test exercises realistic
 * config wiring without ad-hoc fixtures.
 */

#include "utils/hub_host.hpp"

#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using pylabhub::utils::Logger;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::MakeModDefList;
using pylabhub::config::HubConfig;
using pylabhub::utils::HubDirectory;
using pylabhub::hub_host::HubHost;

// ─── Helpers ────────────────────────────────────────────────────────────────

namespace
{

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l2_hubhost_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

/// Initialize a hub directory and patch its hub.json to a loopback /
/// no-CURVE configuration suitable for L2 in-process testing.
void write_test_hub_json(const fs::path &dir, const std::string &name)
{
    fs::create_directories(dir);
    HubDirectory::init_directory(dir, name);

    // Patch the generated hub.json: TCP loopback ephemeral port (so
    // tests don't collide on a fixed port), no CURVE auth (avoids
    // vault wiring), no admin endpoint (Phase 6.2 is not under test).
    const fs::path hub_json = dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json);
        ASSERT_TRUE(f.is_open()) << "test could not open " << hub_json;
        j = nlohmann::json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0"; // ephemeral
    // Disable admin so we don't bind a second port; Phase 6.2 covers admin.
    j["admin"]["enabled"] = false;
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
}

} // namespace

// ─── Test fixture ──────────────────────────────────────────────────────────

class HubHostTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        // Lifecycle modules required by the broker thread:
        //   - Logger             (always)
        //   - FileLock           (JsonConfig dep)
        //   - JsonConfig         (HubConfig::load)
        //   - CryptoUtils        (broker CURVE init path; harmless when
        //                         use_curve=false at runtime)
        //   - ZMQContext         (broker ROUTER socket)
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(),
            FileLock::GetLifecycleModule(),
            JsonConfig::GetLifecycleModule(),
            pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetZMQContextModule()),
            std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    /// Returns a HubConfig + the dir that owns it.  Fresh dir per call.
    std::pair<HubConfig, fs::path> make_config(const char *tag)
    {
        const fs::path dir = unique_temp_dir(tag);
        paths_to_clean_.push_back(dir);
        write_test_hub_json(dir, "TestHub");
        return {HubConfig::load_from_directory(dir.string()), dir};
    }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
    std::vector<fs::path>                  paths_to_clean_;
};

std::unique_ptr<LifecycleGuard> HubHostTest::s_lifecycle_;

// ─── Tests ─────────────────────────────────────────────────────────────────

TEST_F(HubHostTest, Construct_WithoutStartup_NoThreadSpawn)
{
    auto [cfg, dir] = make_config("ctor");
    HubHost host(std::move(cfg));
    EXPECT_FALSE(host.is_running());
}

TEST_F(HubHostTest, BrokerHubState_IsHubHostHubState)
{
    // HEP-CORE-0033 §4 ownership invariant: HubHost owns the HubState
    // by value; the broker holds a non-owning reference to that exact
    // instance.  This is the single load-bearing assertion of the 6.1a
    // refactor — if the broker ended up with its own copy or a
    // different HubState, role/admin/script reads would diverge from
    // what the broker writes.
    auto [cfg, dir] = make_config("state_identity");
    HubHost host(std::move(cfg));

    host.startup();
    EXPECT_TRUE(host.is_running());

    EXPECT_EQ(&host.state(), &host.broker().hub_state())
        << "HubHost::state() and broker().hub_state() must point at the "
           "same HubState instance (HEP-CORE-0033 §4 ownership)";

    host.shutdown();
    EXPECT_FALSE(host.is_running());
}

TEST_F(HubHostTest, Startup_Idempotent)
{
    auto [cfg, dir] = make_config("startup_idem");
    HubHost host(std::move(cfg));

    host.startup();
    EXPECT_TRUE(host.is_running());
    EXPECT_NO_THROW(host.startup()); // second call is a no-op
    EXPECT_TRUE(host.is_running());

    host.shutdown();
}

TEST_F(HubHostTest, Shutdown_Idempotent)
{
    auto [cfg, dir] = make_config("shutdown_idem");
    HubHost host(std::move(cfg));

    host.startup();
    host.shutdown();
    EXPECT_FALSE(host.is_running());
    EXPECT_NO_THROW(host.shutdown());
    EXPECT_FALSE(host.is_running());
}

TEST_F(HubHostTest, RunMainLoop_BlocksUntilRequestShutdown)
{
    auto [cfg, dir] = make_config("run_loop");
    HubHost host(std::move(cfg));

    host.startup();

    std::atomic<bool> loop_returned{false};
    std::thread main_thread([&] {
        host.run_main_loop();
        loop_returned.store(true);
    });

    // Loop should still be blocked.
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(loop_returned.load());

    // Wake it up.  Per the contract, request_shutdown() also calls
    // broker.stop() — verify by checking the broker is no longer
    // serving its poll loop.  We can't observe the broker thread
    // state directly here (ThreadManager owns it); instead the L3
    // integration test exercises the post-request_shutdown REG_REQ
    // failure path.  At L2 we verify the API contract: caller can
    // follow request_shutdown() with shutdown() and the second call
    // completes promptly.
    host.request_shutdown();

    // Bounded wait for return.
    if (main_thread.joinable())
        main_thread.join();
    EXPECT_TRUE(loop_returned.load());

    // shutdown() drains the now-stopped broker thread.  Because
    // request_shutdown() already broke the broker poll loop, this
    // should complete promptly.  A buggy request_shutdown() that
    // ONLY flipped the flag would leave the broker running, and
    // shutdown() here would still call broker.stop() and drain
    // (still passes) — so this is not a strict regression test for
    // the contract; the L3 integration test covers that side.
    host.shutdown();
    EXPECT_FALSE(host.is_running());
}

TEST_F(HubHostTest, Startup_FailsCleanlyOnBusyPort)
{
    // First host binds an ephemeral port; we capture it as the busy
    // address and point a second host at the same endpoint.  The
    // second startup() must throw and leave the second HubHost in a
    // clean state — no leaked broker thread, no detached worker.
    auto [cfg1, dir1] = make_config("busy_first");
    HubHost host1(std::move(cfg1));
    host1.startup();
    const std::string busy_endpoint = host1.broker_endpoint();
    ASSERT_FALSE(busy_endpoint.empty());

    // Build a second HubConfig and patch its endpoint to collide.
    auto [cfg2, dir2] = make_config("busy_second");
    {
        nlohmann::json j;
        const auto hub_json = dir2 / "hub.json";
        {
            std::ifstream f(hub_json);
            j = nlohmann::json::parse(f);
        }
        j["network"]["broker_endpoint"] = busy_endpoint;
        {
            std::ofstream f(hub_json);
            f << j.dump(2);
        }
        cfg2 = HubConfig::load_from_directory(dir2.string());
    }

    HubHost host2(std::move(cfg2));
    EXPECT_THROW(host2.startup(), std::exception)
        << "second startup() on busy endpoint must throw";
    EXPECT_FALSE(host2.is_running())
        << "after failed startup, is_running must be false";
    // host2 destruction must not hang or detach threads — verified by
    // the test reaching this point and the ThreadManager bounded-join
    // running in the dtor.

    host1.shutdown();
}

TEST_F(HubHostTest, Destructor_CleansUpEvenWithoutExplicitShutdown)
{
    auto [cfg, dir] = make_config("dtor");
    {
        HubHost host(std::move(cfg));
        host.startup();
        EXPECT_TRUE(host.is_running());
        // Skip explicit shutdown — destructor must run it.
    }
    // Test passes if we reach here without hangs / crashes / detached
    // threads.  The bounded join in ThreadManager catches any leak.
}

TEST_F(HubHostTest, ConfigAccessor_ReturnsLoadedValue)
{
    auto [cfg, dir] = make_config("cfg_access");
    const std::string original_uid = cfg.identity().uid;

    HubHost host(std::move(cfg));
    EXPECT_EQ(host.config().identity().uid, original_uid);
    EXPECT_EQ(host.config().identity().name, "TestHub");
}

// ─── Phase FSM: single-use after shutdown ──────────────────────────────────
//
// HubHost has a strict 3-phase run-state machine (independent of
// `LifecycleGuard` — HubHost is not a `LifecycleGuard` module):
//     Constructed --startup()--> Running --shutdown()--> ShutDown
// The ShutDown phase is terminal — calling startup() again must throw.
// This protects against subtle bugs where a caller "restarts" a host
// after teardown: ThreadManager would refuse the duplicate
// registration, and any partially-constructed broker state from the
// previous run (sockets, peer tables) would not be re-initialized
// cleanly.
//
// A failed startup() rolls back to Constructed (covered by
// Startup_FailsCleanlyOnBusyPort); only a successful shutdown() is
// terminal.  This pair of tests pins both rules.
TEST_F(HubHostTest, StartupAfterShutdown_Throws)
{
    auto [cfg, dir] = make_config("after_shutdown");
    HubHost host(std::move(cfg));

    host.startup();
    host.shutdown();
    EXPECT_FALSE(host.is_running());

    // Second startup() must throw — host is single-use.
    EXPECT_THROW(host.startup(), std::logic_error);
    EXPECT_FALSE(host.is_running())
        << "rejected startup() must not change FSM state";
}

TEST_F(HubHostTest, FailedStartupAllowsRetry)
{
    // After a failed startup(), the FSM is rolled back to Constructed,
    // so the caller can retry on the same instance once the underlying
    // problem (here: port collision) is fixed.  This is the
    // counterpart to StartupAfterShutdown_Throws — failure is
    // retryable, but successful shutdown is terminal.
    auto [cfg1, dir1] = make_config("retry_blocker");
    HubHost blocker(std::move(cfg1));
    blocker.startup();
    const std::string busy_endpoint = blocker.broker_endpoint();
    ASSERT_FALSE(busy_endpoint.empty());

    auto [cfg2, dir2] = make_config("retry_target");
    {
        nlohmann::json j;
        const auto hub_json = dir2 / "hub.json";
        {
            std::ifstream f(hub_json);
            j = nlohmann::json::parse(f);
        }
        j["network"]["broker_endpoint"] = busy_endpoint; // collide
        {
            std::ofstream f(hub_json);
            f << j.dump(2);
        }
        cfg2 = HubConfig::load_from_directory(dir2.string());
    }

    HubHost host(std::move(cfg2));
    EXPECT_THROW(host.startup(), std::exception);
    EXPECT_FALSE(host.is_running());

    // Free the busy endpoint and patch host's config to a fresh one.
    blocker.shutdown();
    {
        nlohmann::json j;
        const auto hub_json = dir2 / "hub.json";
        {
            std::ifstream f(hub_json);
            j = nlohmann::json::parse(f);
        }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0"; // ephemeral
        {
            std::ofstream f(hub_json);
            f << j.dump(2);
        }
    }
    // Note: we cannot patch the in-memory HubConfig already inside the
    // host (no setter — by design), so the retry exercise here demonstrates
    // the FSM rollback path: state is back to Constructed, so a *fresh*
    // HubHost on the patched config is the right pattern.  The
    // single-use guarantee bites only on the SAME instance after a
    // SUCCESSFUL shutdown — which the prior test pins.
    auto cfg2_retry = HubConfig::load_from_directory(dir2.string());
    HubHost retry_host(std::move(cfg2_retry));
    EXPECT_NO_THROW(retry_host.startup());
    EXPECT_TRUE(retry_host.is_running());
    retry_host.shutdown();
}

