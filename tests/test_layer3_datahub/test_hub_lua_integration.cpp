/**
 * @file test_hub_lua_integration.cpp
 * @brief L3 integration test: HubHost + LuaEngine + real init.lua
 *        (HEP-CORE-0033 Phase 7 D3.3).
 *
 * End-to-end verification of the Phase 7 D3 stack:
 *   1. HubHost ctor accepts a real `LuaEngine` via the 2-arg ctor
 *      (D2.2 wiring).
 *   2. `HubScriptRunner::worker_main_` runs the engine setup
 *      sequence (initialize → load_script → build_api) and reaches
 *      `invoke_on_init` synchronously (D3.1 phase ordering).
 *   3. `LuaEngine::build_api_(HubAPI&)` exposes the 3 closures as
 *      Lua globals (D3.2): `api:log`, `api:uid`, `api:metrics`.
 *   4. The Lua script's `on_init(api)` callback runs and the
 *      log line it emits via `api:log()` lands in the process
 *      logger sink with the `[hub-lua]` prefix.
 *   5. `HubHost::shutdown()` triggers `on_stop`, drains the runner,
 *      and joins the worker thread cleanly.
 *
 * No `sleep_for` to order operations — `host.startup()` blocks
 * on the runner's `ready_promise` (worker has run on_init by the
 * time startup returns); `host.shutdown()` synchronously joins
 * the worker (on_stop has run by the time shutdown returns).
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"

#include "engine_factory.hpp"     // make_engine_from_script_config (scripting)

#include "log_capture_fixture.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using pylabhub::utils::Logger;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::LifecycleGuard;
using pylabhub::utils::MakeModDefList;
using pylabhub::config::HubConfig;
using pylabhub::utils::HubDirectory;
using pylabhub::hub_host::HubHost;
using json = nlohmann::json;

namespace
{

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l3_lua_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

/// Read the entire log file captured by LogCaptureFixture into a
/// string.  Done once after shutdown so we observe the full flushed
/// output (the logger worker thread runs lazily; reading mid-test
/// could race with in-flight writes).
std::string read_log_file(const fs::path &path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ─── Fixture ────────────────────────────────────────────────────────────────

class HubLuaIntegrationTest : public ::testing::Test,
                               public pylabhub::tests::LogCaptureFixture
{
public:
    static void SetUpTestSuite()
    {
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
    void SetUp() override { LogCaptureFixture::Install(); }
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

    /// Build a hub directory wired for in-process L3 testing with Lua
    /// scripts:
    ///   - ephemeral broker endpoint (avoids port collision)
    ///   - admin disabled (Phase 6.2 not under test here)
    ///   - script.type=lua, script.path="." (resolves to <dir>/script/lua/)
    /// @param tag      directory-name tag (for grep-ability)
    /// @param lua_body verbatim body of init.lua written under
    ///                 `<dir>/script/lua/init.lua`
    /// @return         absolute path to the created hub directory
    fs::path make_lua_hub_dir(const char *tag, const std::string &lua_body)
    {
        const fs::path dir = unique_temp_dir(tag);
        HubDirectory::init_directory(dir, "L3LuaHub");
        paths_to_clean_.push_back(dir);

        json j;
        {
            std::ifstream f(dir / "hub.json");
            j = json::parse(f);
        }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["type"]             = "lua";
        j["script"]["path"]             = ".";
        {
            std::ofstream f(dir / "hub.json");
            f << j.dump(2);
        }

        fs::create_directories(dir / "script" / "lua");
        {
            std::ofstream f(dir / "script" / "lua" / "init.lua");
            f << lua_body;
        }
        return dir;
    }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
    std::vector<fs::path>                  paths_to_clean_;
};

std::unique_ptr<LifecycleGuard> HubLuaIntegrationTest::s_lifecycle_;

// ─── Tests ─────────────────────────────────────────────────────────────────

TEST_F(HubLuaIntegrationTest, RealLuaScript_OnInitOnStop_FireAndLog)
{
    // init.lua exercises all 3 hub closures via api global:
    //   - api:log     — verified by grep'ing the log file
    //   - api:uid     — appears inside the log message, pins the
    //                    binding actually returns HubHost's uid
    //   - api:metrics — called for its side effect (returns table;
    //                    we don't read it from C++, but calling it
    //                    proves the closure is callable + json_to_lua
    //                    round-trips the broker's empty-state metrics
    //                    without errors)
    //
    // The script also uses api:metrics() — even though we don't pin
    // its return shape here, the call must not raise a Lua error,
    // which would log via the engine's pcall-error path AND show up
    // as a LogCaptureFixture::AssertNoUnexpectedLogWarnError fail in
    // TearDown.  That's the contract: the closure either returns
    // sensibly or fails the test.
    // NB: dot syntax (not colon) — api closures are plain functions
    // with `self` baked in as an upvalue.  The colon `api:log(...)`
    // would pass the api table as an extra first arg, which the
    // closure doesn't expect.  Mirrors role-side convention
    // (api.log("info", "msg") — see role-side worker scripts).
    const std::string lua_body =
        "function on_init(api)\n"
        "    local m = api.metrics()  -- callable, no error\n"
        "    api.log(\"info\", \"PHASE7_D33_INIT uid=\" .. api.uid())\n"
        "end\n"
        "function on_stop(api)\n"
        "    api.log(\"info\", \"PHASE7_D33_STOP uid=\" .. api.uid())\n"
        "end\n";

    const fs::path dir = make_lua_hub_dir("oninit", lua_body);

    auto cfg = HubConfig::load_from_directory(dir.string());
    const std::string expected_uid = cfg.identity().uid;
    ASSERT_FALSE(expected_uid.empty())
        << "init_directory must have generated a hub uid";

    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    ASSERT_NE(engine, nullptr) << "factory must return a LuaEngine for type=lua";

    HubHost host(std::move(cfg), std::move(engine));

    // Wall-clock bound on startup.  Real LuaEngine adds load_script +
    // FFI sandbox setup — generous bound (5 s) but still tight enough
    // to catch a regression that degrades into a multi-second wait.
    const auto t_startup = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(host.startup());
    const auto startup_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_startup).count();
    EXPECT_LT(startup_ms, 5000)
        << "host.startup() with real LuaEngine must complete in <5 s; "
           "took " << startup_ms << " ms — regression in engine init / "
           "load_script / build_api / on_init paths";
    EXPECT_TRUE(host.is_running());

    // host.startup() blocks on the runner's ready_promise; by the
    // time it returns, on_init has already run synchronously on the
    // worker thread (see hub_script_runner.cpp step C).  No sleep
    // needed to "wait for the script to run".

    host.shutdown();
    EXPECT_FALSE(host.is_running());

    // host.shutdown() synchronously joins the worker; on_stop has
    // run by the time it returns.

    // ── Pin the structural log content ──────────────────────────────────
    //
    // The captured log file holds every LOGGER_* line emitted during
    // the test.  We grep for our two markers and the uid; all three
    // must be present + on_init must precede on_stop in the file.
    const std::string log = read_log_file(LogCaptureFixture::log_path());

    const auto pos_init = log.find("PHASE7_D33_INIT");
    const auto pos_stop = log.find("PHASE7_D33_STOP");

    ASSERT_NE(pos_init, std::string::npos)
        << "missing on_init log marker — api:log() did not route through "
           "HubAPI::log() to the process logger.  Log dump:\n"
        << log;
    ASSERT_NE(pos_stop, std::string::npos)
        << "missing on_stop log marker — script's on_stop never fired.  "
           "Log dump:\n" << log;
    EXPECT_LT(pos_init, pos_stop)
        << "on_init must appear before on_stop in the log";

    // Pin api:uid() actually returns the configured uid (not empty
    // string, not garbage).  Both markers carry it; checking on_init
    // is enough.
    const std::string init_line_marker = "PHASE7_D33_INIT uid=" + expected_uid;
    EXPECT_NE(log.find(init_line_marker), std::string::npos)
        << "api:uid() must return cfg.identity().uid; expected to find\n  "
        << init_line_marker << "\nLog dump:\n" << log;

    // Pin the [hub-lua] log_tag prefix produced by HubAPI::log().
    // HubAPI::log() routes through LOGGER_INFO with the [hub/<uid>]
    // prefix per src/utils/service/hub_api.cpp.  We verify the
    // prefix shape is intact (regression catch for the role-side
    // [<role_tag>-lua] vs hub-side [hub/<uid>] distinction).
    EXPECT_NE(log.find("[hub/" + expected_uid + "]"), std::string::npos)
        << "HubAPI::log() must emit lines with prefix [hub/<uid>]; not found in:\n"
        << log;
}

TEST_F(HubLuaIntegrationTest, ScriptSyntaxError_StartupThrows)
{
    // Malformed Lua — load_script must fail; HubScriptRunner sets
    // ready_promise(false); HubHost::startup() observes !is_running()
    // and throws runtime_error referencing the runner failure.
    //
    // Two ERROR lines are expected on this path (declared up-front so
    // LogCaptureFixture's TearDown doesn't reject them as stray):
    //   1. LuaEngine::load_script — `[hub] Failed to load '<path>':
    //      <lua_error>` — the engine's own diagnostic.
    //   2. HubScriptRunner::worker_main_ step B — `[HubScriptRunner:<uid>]
    //      engine.load_script(<path>) failed` — D3.1 wrap-up that
    //      surfaces which setup step failed before the runner exits.
    ExpectLogError("Failed to load");
    ExpectLogError("engine.load_script");

    const std::string broken_lua =
        "function on_init(api)\n"
        "    this is not valid lua syntax\n"
        "end\n";

    const fs::path dir = make_lua_hub_dir("syntax", broken_lua);

    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));

    try
    {
        host.startup();
        FAIL() << "startup() must throw when init.lua has a syntax error";
    }
    catch (const std::runtime_error &e)
    {
        const std::string what = e.what();
        EXPECT_NE(what.find("script runner failed to start"), std::string::npos)
            << "expected runtime_error mentioning the runner-startup failure; "
               "got: " << what;
    }
    catch (const std::exception &e)
    {
        FAIL() << "startup() threw wrong exception type: " << e.what();
    }

    EXPECT_FALSE(host.is_running());
}
