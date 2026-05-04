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

#include "log_capture_fixture.h"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_api.hpp"            // HubStubEngine builds against HubAPI
#include "utils/hub_directory.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_api_base.hpp"      // StubEngine ApiT polymorph base
#include "utils/role_host_core.hpp"     // StubEngine init_engine_ takes RoleHostCore*
#include "utils/script_engine.hpp"      // HubStubEngine derives from ScriptEngine

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
    // Disable script runtime by default — Phase 7 D2.2 introduces strict
    // engine/path matching at startup (engine null + path set → throw).
    // Existing HubHost lifecycle tests don't exercise scripts; explicit
    // empty path keeps them script-disabled.  The new D2.2 script-
    // enabled tests use `make_config_with_script()` to opt back in.
    j["script"]["path"] = "";
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
}

} // namespace

// ─── Test fixture ──────────────────────────────────────────────────────────

class HubHostTest : public ::testing::Test,
                     public pylabhub::tests::LogCaptureFixture
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
    void SetUp() override
    {
        // Capture log output for the duration of each test.  Tests
        // that drive a known warning path declare it via
        // ExpectLogWarn(...).  Anything else is a failure.
        LogCaptureFixture::Install();
    }

    void TearDown() override
    {
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
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

    // Wake it up.  Post-D2.3 (Option E follow-up) the request_shutdown
    // contract is the role-side "dumb signal" pattern: flip the
    // shutdown atomic + wake the run_main_loop CV; do NOT actively
    // stop admin/broker.  The synchronous, ordered teardown happens
    // exclusively in `shutdown()` on the main thread (HEP-CORE-0033
    // §4.2: runner first, then admin, then broker).  At L2 we verify
    // the API contract end-to-end: signal arrives, run_main_loop
    // returns, follow-up shutdown() completes cleanly.
    host.request_shutdown();

    // Bounded wait for return.
    if (main_thread.joinable())
        main_thread.join();
    EXPECT_TRUE(loop_returned.load());

    // shutdown() does the actual broker/admin stop + ThreadManager
    // drain.  Should complete promptly even though request_shutdown
    // didn't pre-emptively touch broker — the broker's poll loop is
    // still running here, but `broker->stop()` inside shutdown() will
    // wake it on the next tick.
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

    // ── Outcome + path + speed ─────────────────────────────────────────────
    //
    // Throw alone is not sufficient — a regression that breaks broker
    // exception-forwarding falls back to the 5 s ready_future timeout,
    // and `EXPECT_THROW(...)` would still pass (just 5 s later).  Pin
    // BOTH the speed (must fail fast: <1 s) and the path (the message
    // must NOT be the timeout-path message), so any future regression
    // surfaces as a real failure and not just a slow test.  This is
    // the lesson from 2026-05-01: the prior test asserted only "throws"
    // and the slow path silently became normal for weeks.
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
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    EXPECT_TRUE(threw)
        << "second startup() on busy endpoint must throw";
    EXPECT_LT(elapsed_ms, 1000)
        << "startup() must fail FAST on bind error (<1 s); took "
        << elapsed_ms << " ms.  Slow failure indicates broker exception "
           "is no longer being forwarded via ready_promise — see the "
           "lambda in HubHost::startup() that wraps broker.run() in a "
           "try/catch and calls ready_promise->set_exception().";
    EXPECT_EQ(err_what.find("did not signal ready within 5s"),
              std::string::npos)
        << "startup() failed via the timeout path, not the bind-error path; "
           "what(): " << err_what;
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

    // Second startup() must throw — host is single-use.  Pin both the
    // exception type AND a message substring so that a regression
    // returning a different std::logic_error (e.g. from a downstream
    // assert) does not silently masquerade as the FSM single-use throw.
    bool threw = false;
    std::string msg;
    try { host.startup(); }
    catch (const std::logic_error &e) { threw = true; msg = e.what(); }
    EXPECT_TRUE(threw) << "second startup() must throw std::logic_error";
    EXPECT_NE(msg.find("after shutdown"), std::string::npos)
        << "wrong logic_error path; what(): " << msg;
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

    // Pin outcome + speed + path (see Startup_FailsCleanlyOnBusyPort
    // for the lesson: an outcome-only `EXPECT_THROW` would silently
    // accept a 5 s ready-future timeout regression).
    bool threw = false;
    std::string err_what;
    const auto t0 = std::chrono::steady_clock::now();
    try { host.startup(); }
    catch (const std::exception &e) { threw = true; err_what = e.what(); }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(threw);
    EXPECT_LT(elapsed_ms, 1000)
        << "startup() must fail FAST on bind error (<1 s); took "
        << elapsed_ms << " ms — broker exception not forwarded via "
           "ready_promise.  See HubHost::startup() lambda.";
    EXPECT_EQ(err_what.find("did not signal ready within 5s"),
              std::string::npos)
        << "startup() failed via the timeout path, not the bind-error path; "
           "what(): " << err_what;
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

// ───────────────────────────────────────────────────────────────────────────
// HEP-CORE-0033 Phase 7 D2.2 — script-enabled HubHost lifecycle
// ───────────────────────────────────────────────────────────────────────────
//
// Coverage for the runner wiring added in D2.2:
//   - 2-arg ctor accepts an injected ScriptEngine; runner builds in
//     startup() when both engine != null AND cfg.script().path is set.
//   - Mismatch (engine null + path set, or engine set + path empty)
//     throws fail-fast at startup() with a descriptive logic_error.
//   - Runner stops BEFORE admin and broker per HEP §4.2 step 2 ordering.
//   - eval_in_script() forwards to the runner's engine when running;
//     returns NotFound otherwise.
//
// Uses an in-file HubStubEngine that satisfies all ScriptEngine virtuals
// with no-op behaviour and reports successful build_api(HubAPI&) so the
// runner's startup path completes without loading any real script.
// Pattern mirrors `tests/.../role_data_loop_workers.cpp::StubEngine` for
// the role side.

namespace {

using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::InvokeResponse;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::InvokeInbox;
using pylabhub::scripting::InvokeStatus;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::ScriptEngine;
using pylabhub::hub_host::HubAPI;

/// No-op ScriptEngine that satisfies all virtuals.  Override
/// `build_api_(HubAPI&)` to return true so the runner's startup path
/// signals ready.  Tracks eval/invoke calls for assertions.
class HubStubEngine : public ScriptEngine
{
public:
    /// Distinct sentinel returned from eval() so tests can confirm the
    /// forwarding path actually traversed this engine.
    InvokeStatus  eval_status{InvokeStatus::Ok};
    nlohmann::json eval_value_payload = "stub_eval_ok";
    std::atomic<int> eval_calls{0};

protected:
    bool init_engine_(const std::string &, RoleHostCore *) override { return true; }
    bool build_api_(RoleAPIBase &) override { return true; }
    bool build_api_(HubAPI &) override { return true; }   // <-- hub-side enable
    void finalize_engine_() override {}

public:
    bool load_script(const std::filesystem::path &, const std::string &,
                     const std::string &) override { return true; }
    bool has_callback(const std::string &) const override { return false; }
    bool register_slot_type(const pylabhub::hub::SchemaSpec &,
                            const std::string &, const std::string &) override
    { return true; }
    size_t         type_sizeof(const std::string &) const override { return 0; }
    bool           invoke(const std::string &) override { return true; }
    bool           invoke(const std::string &, const nlohmann::json &) override { return true; }
    InvokeResponse eval(const std::string &) override
    {
        eval_calls.fetch_add(1, std::memory_order_relaxed);
        return {eval_status, eval_value_payload};
    }
    void invoke_on_init() override {}
    void invoke_on_stop() override {}
    InvokeResult invoke_produce(InvokeTx, std::vector<IncomingMessage> &) override
    { return InvokeResult::Commit; }
    InvokeResult invoke_consume(InvokeRx, std::vector<IncomingMessage> &) override
    { return InvokeResult::Commit; }
    InvokeResult invoke_process(InvokeRx, InvokeTx,
                                std::vector<IncomingMessage> &) override
    { return InvokeResult::Commit; }
    InvokeResult invoke_on_inbox(InvokeInbox) override { return InvokeResult::Commit; }
    uint64_t     script_error_count() const noexcept override { return 0; }
    bool         supports_multi_state() const noexcept override { return false; }
};

/// Patch hub.json to set script.path to a non-empty value (the dir
/// itself is fine — HubStubEngine.load_script ignores the path).
void make_script_enabled(const fs::path &dir)
{
    const fs::path hub_json = dir / "hub.json";
    nlohmann::json j;
    {
        std::ifstream f(hub_json);
        j = nlohmann::json::parse(f);
    }
    j["script"]["path"] = ".";  // non-empty — opt back into scripts
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
}

} // namespace

// ── Script-enabled lifecycle ────────────────────────────────────────────────

TEST_F(HubHostTest, ScriptEnabled_StartupConstructsRunner_ShutdownStopsCleanly)
{
    auto [cfg_disabled, dir] = make_config("se_lifecycle");
    make_script_enabled(dir);
    auto cfg = HubConfig::load_from_directory(dir.string());

    HubHost host(std::move(cfg), std::make_unique<HubStubEngine>());
    ASSERT_NO_THROW(host.startup());
    EXPECT_TRUE(host.is_running());

    // Runner is in flight — eval forwards to engine (single test thread,
    // no concurrent worker dispatch).  Confirms HubHost::eval_in_script
    // routes through HubScriptRunner::eval into the engine.
    auto resp = host.eval_in_script("dummy_code");
    EXPECT_EQ(resp.status, InvokeStatus::Ok)
        << "eval_in_script must forward to engine when runner is running";
    const auto resp_payload = resp.value.get<std::string>();
    EXPECT_EQ(resp_payload, "stub_eval_ok")
        << "eval_in_script must surface engine.eval()'s payload verbatim";

    host.shutdown();
    EXPECT_FALSE(host.is_running());

    // After shutdown, eval falls through to NotFound.
    auto resp_after = host.eval_in_script("dummy_code");
    EXPECT_EQ(resp_after.status, InvokeStatus::NotFound)
        << "eval_in_script must return NotFound after shutdown — runner is gone";
}

TEST_F(HubHostTest, ScriptDisabled_NoEngine_NoRunner_EvalReturnsNotFound)
{
    // Default fixture is script-disabled (write_test_hub_json sets
    // script.path = "").  Use the 2-arg ctor with nullptr engine to
    // pin the script-disabled path explicitly.
    auto [cfg, dir] = make_config("sd_no_engine");
    HubHost host(std::move(cfg), nullptr);
    ASSERT_NO_THROW(host.startup());
    EXPECT_TRUE(host.is_running());

    auto resp = host.eval_in_script("anything");
    EXPECT_EQ(resp.status, InvokeStatus::NotFound)
        << "eval_in_script must return NotFound when no runner exists";

    host.shutdown();
}

TEST_F(HubHostTest, EngineSetButPathEmpty_StartupThrowsLogicError)
{
    // Default fixture has script.path = "" (script-disabled).  Passing
    // an engine here is a config/main mismatch — startup must reject.
    auto [cfg, dir] = make_config("se_path_empty");
    HubHost host(std::move(cfg), std::make_unique<HubStubEngine>());

    try
    {
        host.startup();
        FAIL() << "startup() must throw on engine-set + path-empty mismatch";
    }
    catch (const std::logic_error &e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("script-engine / script-path mismatch"),
                  std::string::npos)
            << "exception must name the mismatch; what(): " << what;
        EXPECT_NE(what.find("engine INJECTED"), std::string::npos)
            << "exception must report engine state; what(): " << what;
        EXPECT_NE(what.find("path is empty"), std::string::npos)
            << "exception must report path state; what(): " << what;
    }
    catch (const std::exception &e)
    {
        FAIL() << "startup() threw wrong exception type: " << e.what();
    }

    EXPECT_FALSE(host.is_running())
        << "after a failed startup, host must not be running";
}

TEST_F(HubHostTest, NoEngineButPathSet_StartupThrowsLogicError)
{
    auto [cfg_disabled, dir] = make_config("sd_path_set");
    make_script_enabled(dir);
    auto cfg = HubConfig::load_from_directory(dir.string());

    HubHost host(std::move(cfg), nullptr);

    try
    {
        host.startup();
        FAIL() << "startup() must throw on engine-null + path-set mismatch";
    }
    catch (const std::logic_error &e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("script-engine / script-path mismatch"),
                  std::string::npos)
            << "exception must name the mismatch; what(): " << what;
        EXPECT_NE(what.find("engine absent"), std::string::npos)
            << "exception must report engine state; what(): " << what;
        EXPECT_NE(what.find("path is non-empty"), std::string::npos)
            << "exception must report path state; what(): " << what;
    }
    catch (const std::exception &e)
    {
        FAIL() << "startup() threw wrong exception type: " << e.what();
    }

    EXPECT_FALSE(host.is_running());
}

TEST_F(HubHostTest, ScriptEnabled_DestructorCleansUpRunnerWithoutExplicitShutdown)
{
    // Mirrors the existing Destructor_CleansUpEvenWithoutExplicitShutdown
    // shape but with the script-enabled path — verifies that ~HubHost's
    // idempotent shutdown also tears the runner down.
    auto [cfg_disabled, dir] = make_config("se_dtor");
    make_script_enabled(dir);
    auto cfg = HubConfig::load_from_directory(dir.string());

    {
        HubHost host(std::move(cfg), std::make_unique<HubStubEngine>());
        ASSERT_NO_THROW(host.startup());
        EXPECT_TRUE(host.is_running());
        // Let scope exit drive the destructor — must not panic, abort,
        // or emit unexpected warns/errors (LogCaptureFixture asserts
        // this in TearDown).
    }
    SUCCEED();
}
