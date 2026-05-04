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
    fs::path make_lua_hub_dir(const char *tag, const std::string &lua_body,
                               const std::string &loop_timing = "fixed_rate",
                               int target_period_ms = 1000)
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
        // Loop timing — the on_tick semantics tests pin specific
        // policies + short periods so the hub fires multiple ticks
        // within a sub-second test window.  Default keeps the
        // pre-existing 1 Hz fixed_rate config the prior tests relied
        // on (one cycle is long enough that on_tick never fires
        // during their startup→shutdown windows).
        j["loop_timing"]      = loop_timing;
        j["target_period_ms"] = target_period_ms;
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

    /// Count occurrences of `marker` in `haystack`.
    static int count_markers(const std::string &haystack, const std::string &marker)
    {
        int count = 0;
        size_t pos = 0;
        while ((pos = haystack.find(marker, pos)) != std::string::npos)
        {
            ++count;
            pos += marker.size();
        }
        return count;
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

    // Wall-clock bound on startup.  Observed steady-state on dev
    // hardware: ~110 ms (LuaJIT init + FFI sandbox + load_script +
    // build_api + on_init + ready_promise round-trip).  Bound 1500 ms
    // gives ~13× headroom for slow CI / sanitizer builds while still
    // catching a sub-second slowdown — looser bounds (the original
    // 5 s) accept multi-second regressions silently, which is exactly
    // the failure mode CLAUDE.md "tests must pin path, timing, and
    // structure" forbids.  Tighten further only if CI false-positives.
    const auto t_startup = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(host.startup());
    const auto startup_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_startup).count();
    EXPECT_LT(startup_ms, 1500)
        << "host.startup() with real LuaEngine must complete in <1500 ms; "
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

// ─── on_tick semantic tests (HEP-CORE-0033 §12.4 dispatch model) ───────────
//
// These tests pin the post-Phase-7-D-track on_tick contract:
//
//   on_tick fires once per period, deterministically, whenever the
//   deadline has been crossed.  The check is `now() >= deadline`
//   AFTER event-handler dispatch — events alone don't shift the
//   schedule.
//
// Coverage (event-injection scenarios T2/T3 from the test plan are
// SKIPPED — Phase 7 minimum HubAPI surface doesn't expose channel /
// role mutators that scripts could call to inject events from inside
// the test, and spinning up a real producer subprocess just for these
// tests is too heavy.  The HubLuaIntegrationTest sibling for Python
// also doesn't get on_tick tests for the same reason — single-thread
// rule means we keep that test minimal too).

TEST_F(HubLuaIntegrationTest, OnTick_FiresPeriodically_WhenIdle)
{
    // T1 — baseline contract: an idle hub fires on_tick once per
    // period via the timeout-driven wake (no events injected; only
    // wakeup source is `wait_for_incoming` returning at deadline).
    //
    // Config: FixedRate at 100 ms.  Run for 550 ms post-startup.
    //
    // Expected ticks: 4-6.
    //   floor: first tick fires after one full period of warm-up
    //     (deadline initialized to startup + 100 ms), so the first
    //     possible tick is at ~T+100 ms.  By T+550 ms we should see
    //     ticks at ~100/200/300/400/500 → 5.
    //   - allow 4 (CI jitter shaving the last tick)
    //   - allow 6 (CI scheduling pushing one in)
    // NOTE: on_tick takes NO arguments — `engine.invoke("on_tick")`
    // (called via `HubAPI::dispatch_tick`) passes 0 args.  The api
    // table is reachable via the Lua global set by
    // `LuaEngine::build_api_(HubAPI&)`.  Mirrors the role-side
    // convention for non-data-loop callbacks (event handlers receive
    // their typed payload arg; tick gets nothing).
    const std::string lua_body =
        "function on_tick()\n"
        "    api.log('info', 'PHASE7_E_TICK_T1')\n"
        "end\n";

    const fs::path dir = make_lua_hub_dir(
        "tick_idle", lua_body, /*loop_timing=*/"fixed_rate",
        /*target_period_ms=*/100);

    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());

    HubHost host(std::move(cfg), std::move(engine));
    ASSERT_NO_THROW(host.startup());

    // Sleep for 550 ms — long enough to observe ~5 ticks at 100 ms.
    // Acceptable in a test (no other thread synchronization point
    // exists for "the worker has fired N ticks"; the runner is
    // intentionally encapsulated and exposes no tick-count).
    std::this_thread::sleep_for(std::chrono::milliseconds{550});

    host.shutdown();

    const std::string log = read_log_file(LogCaptureFixture::log_path());
    const int tick_count = count_markers(log, "PHASE7_E_TICK_T1");

    EXPECT_GE(tick_count, 4)
        << "expected >= 4 on_tick markers in 550 ms with period=100 ms; "
           "got " << tick_count
        << " — regression in deadline-driven tick (timeout path) or "
           "loop init.\nLog dump:\n" << log;
    EXPECT_LE(tick_count, 6)
        << "expected <= 6 on_tick markers in 550 ms with period=100 ms; "
           "got " << tick_count
        << " — regression in deadline advancement (over-firing).\nLog dump:\n"
        << log;
}

TEST_F(HubLuaIntegrationTest, OnTick_CatchUp_FixedRateWithCompensation)
{
    // T4 — FixedRateWithCompensation catch-up semantic.  After a
    // long-running script callback (here: on_init busy-loops for
    // ~500 ms), `wait_for_incoming` immediately returns 0 ms (the
    // already-passed deadline) and the gate fires repeatedly while
    // `compute_next_deadline` walks the absolute grid forward by
    // `period` until `deadline > now`.  Result: a tight burst of
    // on_tick markers covering the missed slots, then normal-pace
    // ticks.
    //
    // Config: FRWithComp at 100 ms.  Stall the FIRST on_tick callback
    // for ~500 ms (counter-gated so subsequent ticks return fast).
    // By the time tick #1 returns, the schedule grid (T+100/200/300/
    // 400/500) is 4 slots in the past.  `wait_for_incoming` returns
    // 0 immediately (deadline-already-passed) and the gate fires
    // repeatedly while `compute_next_deadline` walks the grid forward
    // — burst of catch-up ticks until `deadline > now`.
    //
    // Why stall in on_tick (not on_init)?  The loop's deadline init
    // runs AFTER on_init returns — so a stalled on_init doesn't put
    // anything into the past.  The stall must happen INSIDE the loop,
    // on a callback that runs after the deadline grid is laid.
    //
    // Sleep 800 ms post-startup → covers warm-up (100 ms) + stall
    // (500 ms) + catch-up burst + ≥1 normal-pace tick.
    //
    // Expected timeline:
    //   T+0       startup, on_init logs (fast).
    //   T+0+ε     worker enters loop, deadline = T+100.
    //   T+100     tick #1 fires; busy-loops to T+600.
    //   T+600+ε   catch-up burst: ticks #2..#6 fire in quick
    //             succession as compute_next_deadline walks
    //             T+200/300/400/500/600 forward.
    //   T+700     tick #7 fires (normal pace).
    //   T+800     test ends shortly after tick #8 fires.
    //
    // Expected total ticks: ≥ 6 (stalled #1 + ≥4 catch-up + ≥1
    // normal).  Upper bound 12 (room for clock jitter).
    //
    // Why this matters: pre-fix, `compute_next_deadline` was called
    // every iteration (including on event-driven wakes that didn't
    // fire a tick), advancing the deadline forward without firing
    // on_tick.  Under heavy event activity OR long callbacks, the
    // FRWithComp "compensation" semantic was silently downgraded to
    // FixedRate-with-skips.  This test mutation-protects the post-
    // fix behavior: only advance deadline when tick fires.
    // Stall mechanism: FFI nanosleep for a true wall-clock sleep.
    // A busy-loop on os.clock() measures CPU time, which on a
    // CPU-contended machine (e.g. ctest -j2) drifts arbitrarily off
    // wall time and shrinks/extends the apparent stall — flaky.
    // nanosleep is the worker thread blocking on the kernel for a
    // guaranteed wall-clock duration regardless of CPU pressure.
    const std::string lua_body =
        "local ffi = require('ffi')\n"
        "ffi.cdef[[\n"
        "typedef struct plh_ts { long tv_sec; long tv_nsec; } plh_ts;\n"
        "int nanosleep(const plh_ts *req, plh_ts *rem);\n"
        "]]\n"
        "tick_count = 0\n"
        "function on_init(api)\n"
        "    api.log('info', 'PHASE7_E_INIT_T4')\n"
        "end\n"
        "function on_tick()\n"
        "    tick_count = tick_count + 1\n"
        "    api.log('info', 'PHASE7_E_TICK_T4')\n"
        "    if tick_count == 1 then\n"
        "        -- Stall ONLY the first tick — 500 ms wall-clock.\n"
        "        local req = ffi.new('plh_ts', 0, 500 * 1000 * 1000)\n"
        "        ffi.C.nanosleep(req, nil)\n"
        "        api.log('info', 'PHASE7_E_TICK_STALL_DONE_T4')\n"
        "    end\n"
        "end\n";

    const fs::path dir = make_lua_hub_dir(
        "tick_catchup", lua_body,
        /*loop_timing=*/"fixed_rate_with_compensation",
        /*target_period_ms=*/100);

    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());

    HubHost host(std::move(cfg), std::move(engine));

    ASSERT_NO_THROW(host.startup());

    // Cover warm-up (100 ms) + stall (500 ms) + catch-up + ≥1 normal.
    std::this_thread::sleep_for(std::chrono::milliseconds{800});

    host.shutdown();

    const std::string log = read_log_file(LogCaptureFixture::log_path());
    const int tick_count = count_markers(log, "PHASE7_E_TICK_T4");

    EXPECT_GE(tick_count, 6)
        << "expected >= 6 on_tick markers (stalled #1 + >=4 catch-up + "
           ">=1 normal); got " << tick_count
        << " — regression in FRWithComp catch-up: deadline likely "
           "being advanced on non-tick iterations again, swallowing "
           "missed slots.\nLog dump:\n" << log;
    EXPECT_LE(tick_count, 12)
        << "expected <= 12 on_tick markers; got " << tick_count
        << " — regression in catch-up termination: deadline isn't "
           "advancing past `now` after the burst, so on_tick keeps "
           "firing every iteration even at normal pace.\nLog dump:\n"
        << log;

    // Pin that the stalled tick #1 actually completed (busy-loop
    // exited normally), and that subsequent ticks fired AFTER it.
    const auto stall_done = log.find("PHASE7_E_TICK_STALL_DONE_T4");
    ASSERT_NE(stall_done, std::string::npos)
        << "stalled tick #1 never completed";

    // Count ticks AFTER the stall_done marker — those are the
    // catch-up + normal ticks.  Should be tick_count - 1 (we stalled
    // tick #1, and STALL_DONE is logged right at the end of tick #1).
    const std::string post_stall = log.substr(stall_done);
    const int post_stall_ticks = count_markers(post_stall, "PHASE7_E_TICK_T4");
    EXPECT_GE(post_stall_ticks, 5)
        << "expected >= 5 ticks after stall_done (catch-up burst + "
           "normal); got " << post_stall_ticks
        << " — regression in catch-up: deadline not walking the grid "
           "forward.\nLog dump after stall:\n" << post_stall;
}
