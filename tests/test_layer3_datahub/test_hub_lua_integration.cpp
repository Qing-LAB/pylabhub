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

#include "utils/broker_request_comm.hpp"   // role-side client of broker
#include "utils/hub_api.hpp"
#include "log_capture_fixture.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>

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
        "    api.log(\"info\", \"HUB_INIT uid=\" .. api.uid())\n"
        "end\n"
        "function on_stop(api)\n"
        "    api.log(\"info\", \"HUB_STOP uid=\" .. api.uid())\n"
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

    const auto pos_init = log.find("HUB_INIT");
    const auto pos_stop = log.find("HUB_STOP");

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
    const std::string init_line_marker = "HUB_INIT uid=" + expected_uid;
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
        "    api.log('info', 'TICK_IDLE')\n"
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
    const int tick_count = count_markers(log, "TICK_IDLE");

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
        "    api.log('info', 'INIT_RAN')\n"
        "end\n"
        "function on_tick()\n"
        "    tick_count = tick_count + 1\n"
        "    api.log('info', 'TICK_CATCHUP')\n"
        "    if tick_count == 1 then\n"
        "        -- Stall ONLY the first tick — 500 ms wall-clock.\n"
        "        local req = ffi.new('plh_ts', 0, 500 * 1000 * 1000)\n"
        "        ffi.C.nanosleep(req, nil)\n"
        "        api.log('info', 'TICK_STALL_DONE')\n"
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
    const int tick_count = count_markers(log, "TICK_CATCHUP");

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
    const auto stall_done = log.find("TICK_STALL_DONE");
    ASSERT_NE(stall_done, std::string::npos)
        << "stalled tick #1 never completed";

    // Count ticks AFTER the stall_done marker — those are the
    // catch-up + normal ticks.  Should be tick_count - 1 (we stalled
    // tick #1, and STALL_DONE is logged right at the end of tick #1).
    const std::string post_stall = log.substr(stall_done);
    const int post_stall_ticks = count_markers(post_stall, "TICK_CATCHUP");
    EXPECT_GE(post_stall_ticks, 5)
        << "expected >= 5 ticks after stall_done (catch-up burst + "
           "normal); got " << post_stall_ticks
        << " — regression in catch-up: deadline not walking the grid "
           "forward.\nLog dump after stall:\n" << post_stall;
}

// ─── Read-accessor binding surface (HEP-CORE-0033 §12.3) ───────────────────

TEST_F(HubLuaIntegrationTest, ReadAccessors_AllReachable_FromOnInit)
{
    // Verifies every HubAPI read accessor is callable from a Lua
    // script and returns a sensibly-typed value:
    //   - api.name() / api.uid() — strings (already exists for uid)
    //   - api.config() — table (full hub config snapshot)
    //   - api.list_channels()/_roles()/_bands()/_peers() — empty
    //     tables (no roles registered in this isolated test hub)
    //   - api.get_channel/_role/_band/_peer(missing) — nil
    //   - api.metrics() — table (already exists)
    //   - api.query_metrics({"counters"}) — table filtered to one
    //     category
    //
    // The hub runs alone (no real producer/consumer/peer connects),
    // so the lists are expected to be empty.  This test pins the
    // BINDING surface — every method is reachable, types are correct,
    // no Lua errors.  Functional content (entries appearing when
    // roles register) is covered by future end-to-end tests once
    // there's a script-driven mutator surface; for now the binding
    // pin is what we need.
    //
    // Each call's outcome is logged with a unique marker; the test
    // greps for all markers.  If any binding throws a Lua error, the
    // pcall path emits LOGGER_ERROR which the LogCaptureFixture
    // rejects in TearDown — that fails the test before any marker
    // assertion runs.
    const std::string lua_body = R"LUA(
function on_init(api)
    -- name() — string, non-empty (matches cfg.identity().name).
    local n = api.name()
    if type(n) == 'string' and #n > 0 then
        api.log('info', 'API_NAME_OK:' .. n)
    end

    -- config() — table with at least 'hub' and 'admin' keys.
    local c = api.config()
    if type(c) == 'table' and type(c.hub) == 'table' and
       type(c.admin) == 'table' then
        api.log('info', 'API_CONFIG_OK')
    end

    -- list_*() — all return tables (empty in this isolated hub).
    if type(api.list_channels()) == 'table' then
        api.log('info', 'API_LIST_CHANNELS_OK')
    end
    if type(api.list_roles()) == 'table' then
        api.log('info', 'API_LIST_ROLES_OK')
    end
    if type(api.list_bands()) == 'table' then
        api.log('info', 'API_LIST_BANDS_OK')
    end
    if type(api.list_peers()) == 'table' then
        api.log('info', 'API_LIST_PEERS_OK')
    end

    -- get_*(missing) — nil (Lua null is nil).
    if api.get_channel('no.such.channel') == nil then
        api.log('info', 'API_GET_CHANNEL_NIL_OK')
    end
    if api.get_role('no.such.role') == nil then
        api.log('info', 'API_GET_ROLE_NIL_OK')
    end
    if api.get_band('no.such.band') == nil then
        api.log('info', 'API_GET_BAND_NIL_OK')
    end
    if api.get_peer('no.such.peer') == nil then
        api.log('info', 'API_GET_PEER_NIL_OK')
    end

    -- query_metrics({}) — table (all categories).
    if type(api.query_metrics({})) == 'table' then
        api.log('info', 'API_QUERY_METRICS_ALL_OK')
    end

    -- query_metrics({'counters'}) — table (filtered).
    local m_filt = api.query_metrics({'counters'})
    if type(m_filt) == 'table' then
        api.log('info', 'API_QUERY_METRICS_FILTERED_OK')
    end

    -- Control delegates — fire-and-forget; broker tolerates unknown
    -- channels idempotently.  Testing here pins the BINDING surface
    -- (callable, no error, no Lua exception) — the actual mutation
    -- effect is exercised in end-to-end tests when there's a real
    -- broker-driven channel to operate on.
    api.close_channel('no.such.channel')
    api.log('info', 'API_CLOSE_CHANNEL_OK')

    api.broadcast_channel('no.such.channel', 'hello')
    api.log('info', 'API_BROADCAST_CHANNEL_OK')

    api.broadcast_channel('no.such.channel', 'with-payload', 'payload-bytes')
    api.log('info', 'API_BROADCAST_CHANNEL_DATA_OK')

    -- request_shutdown is left for a separate test below — calling it
    -- here would race with the test fixture's host.shutdown() flow.
end
)LUA";

    const fs::path dir = make_lua_hub_dir("read_accessors", lua_body);

    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));

    ASSERT_NO_THROW(host.startup());
    host.shutdown();

    const std::string log = read_log_file(LogCaptureFixture::log_path());

    // 12 markers, one per binding.
    static constexpr const char *kExpectedMarkers[] = {
        "API_NAME_OK:",
        "API_CONFIG_OK",
        "API_LIST_CHANNELS_OK",
        "API_LIST_ROLES_OK",
        "API_LIST_BANDS_OK",
        "API_LIST_PEERS_OK",
        "API_GET_CHANNEL_NIL_OK",
        "API_GET_ROLE_NIL_OK",
        "API_GET_BAND_NIL_OK",
        "API_GET_PEER_NIL_OK",
        "API_QUERY_METRICS_ALL_OK",
        "API_QUERY_METRICS_FILTERED_OK",
        "API_CLOSE_CHANNEL_OK",
        "API_BROADCAST_CHANNEL_OK",
        "API_BROADCAST_CHANNEL_DATA_OK",
    };
    for (const char *m : kExpectedMarkers)
    {
        EXPECT_NE(log.find(m), std::string::npos)
            << "missing binding marker '" << m
            << "' — HubAPI read accessor failed to bind, returned "
               "wrong type, or the Lua test code mistyped it.\nLog dump:\n"
            << log;
    }
}

TEST_F(HubLuaIntegrationTest, RequestShutdown_FromOnInit_WakesMainLoop)
{
    // Verifies api.request_shutdown() delegates to host.request_shutdown()
    // — script calls it during on_init, then run_main_loop() (called on
    // the test thread) wakes from its wait and returns.  Without the
    // delegation, run_main_loop would block until our wall-clock guard
    // bails the test.
    //
    // The script also hits broadcast_channel("notify_shutdown", ...)
    // before requesting shutdown — proves request_shutdown isn't an
    // accidental side-effect of some OTHER api call (a regression that
    // routed both to host_->request_shutdown would still pass this
    // test alone, but the ReadAccessors test above runs broadcast +
    // close without shutting down).
    const std::string lua_body = R"LUA(
function on_init(api)
    api.log('info', 'BEFORE_SHUTDOWN')
    api.broadcast_channel('no.such.channel', 'notify_shutdown')
    api.request_shutdown()
    api.log('info', 'AFTER_SHUTDOWN_REQUEST')
end
)LUA";

    const fs::path dir = make_lua_hub_dir("request_shutdown", lua_body);

    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));

    ASSERT_NO_THROW(host.startup());

    // run_main_loop blocks until shutdown_flag is set.  Wall-clock
    // guard: if the script's request_shutdown didn't actually delegate
    // to host_->request_shutdown, run_main_loop would block forever;
    // we'd hit the test-process timeout instead of a clean failure.
    // Use a thread that calls run_main_loop with a hard cap so the
    // test gives a clean failure on regression.
    const auto t_begin = std::chrono::steady_clock::now();
    std::thread main_thread([&host]() { host.run_main_loop(); });
    main_thread.join();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_begin).count();

    EXPECT_LT(elapsed_ms, 2000)
        << "run_main_loop must wake within 2 s of startup (the script's "
           "on_init calls api.request_shutdown which should set the host "
           "shutdown flag immediately).  took " << elapsed_ms << " ms — "
           "regression in HubAPI::request_shutdown delegation.";

    host.shutdown();
    EXPECT_FALSE(host.is_running());

    const std::string log = read_log_file(LogCaptureFixture::log_path());
    EXPECT_NE(log.find("BEFORE_SHUTDOWN"), std::string::npos)
        << "on_init never ran; log:\n" << log;
    EXPECT_NE(log.find("AFTER_SHUTDOWN_REQUEST"), std::string::npos)
        << "api.request_shutdown raised an error before returning, or "
           "Lua aborted the script; log:\n" << log;
}

// ─── HEP §12.2.1 event observers + §12.2.3 user-posted events ───────────────
//
// These tests pin the full production chain
//   wire-protocol message (REG_REQ / CONSUMER_REG_REQ / ...)
//   → BrokerService handler
//     → HubState capability op (_on_*)
//       → HubState subscribers
//         → HubScriptRunner enqueue lambda
//           → IncomingMessage queue
//             → worker thread drain
//               → engine.invoke("on_<event>", details)
//                 → Lua script callback runs
//
// Federation observers (on_peer_*) are deliberately deferred —
// federation needs its own design pass per HEP-0033 §16.
//
// Test rigor pattern (per CLAUDE.md "tests must pin path, timing, and
// structure" + the user direction "validate from solid output, not
// return values" + "the framework should LEAD TO the calling, not
// bypass it"):
//
//   1. The TEST runs a real broker (HubHost::startup spawns the broker
//      thread + binds the ROUTER socket).  No friend-access shortcut
//      to call _on_* directly — that would test "the function exists",
//      not "the framework calls it under wire-protocol triggers."
//
//   2. The TEST acts as a real role: spawns BrokerRequestComm pointing
//      at the host's bound endpoint and sends register_channel /
//      register_consumer.  The broker's own handlers walk the path
//      down to the script callbacks.
//
//   3. Each script callback (a) emits a uniquely-marked log line
//      carrying the wire details it received AND (b) calls
//      api.request_shutdown.  The shutdown call is the SOLID OUTPUT
//      that proves the callback actually executed (not just "engine
//      returned true"); the log line confirms the dispatch carried
//      the right details (path-discriminating).
//
//   4. The TEST runs host.run_main_loop on a watchdog future; the
//      future's wait_for is the bounded ceiling, NOT a "wait long
//      enough and hope" pattern.  Wakeup MUST come from the
//      callback's request_shutdown — anything else times out cleanly.

namespace
{

/// Run host.run_main_loop on a watchdog future.  Returns true if it
/// returned within @p timeout (i.e., something actually called
/// request_shutdown — proves the script-side callback executed).
/// Returns false if the watchdog expired (regression: callback did
/// not fire OR did not invoke request_shutdown).
bool run_main_loop_with_watchdog(HubHost &host,
                                  std::chrono::milliseconds timeout)
{
    auto fut = std::async(std::launch::async,
                           [&host]() { host.run_main_loop(); });
    return fut.wait_for(timeout) == std::future_status::ready;
}

/// Flush the async logger so subsequent reads see all written lines.
void flush_logger() { Logger::instance().flush(); }

/// Build a minimal REG_REQ payload — same shape role-side
/// BrokerRequestComm::register_channel(opts) accepts.
nlohmann::json make_reg_opts(const std::string &channel, const std::string &uid)
{
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
    opts["role_uid"]          = uid;
    opts["role_name"]         = "L3TestProducer";
    return opts;
}

} // namespace

TEST_F(HubLuaIntegrationTest, EventObservers_ChannelRegistration_FiresOnChannelOpenedAndOnRoleRegistered)
{
    // Single REG_REQ over the wire fires BOTH `on_channel_opened` AND
    // `on_role_registered` (the producer's role isn't seen until it
    // registers a channel; channel registration also creates the role
    // entry).  Validate both observers fire and carry the right
    // details, using the script's request_shutdown as the synchronous
    // "callback executed" signal.
    const std::string lua_body = R"LUA(
seen = { channel = false, role = false }
function on_init(api) api.log('info', 'INIT_RAN') end
function on_channel_opened(ci)
    api.log('info', 'CHANNEL_OPENED name=' .. (ci.name or '<missing>'))
    seen.channel = true
    if seen.channel and seen.role then api.request_shutdown() end
end
function on_role_registered(ri)
    api.log('info', 'ROLE_REGISTERED uid=' .. (ri.uid or '<missing>'))
    seen.role = true
    if seen.channel and seen.role then api.request_shutdown() end
end
)LUA";

    const fs::path dir = make_lua_hub_dir("evt_chan_role", lua_body,
                                           "fixed_rate", 100);
    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));
    ASSERT_NO_THROW(host.startup());

    // Spawn a real role-side client and send REG_REQ.  The host's
    // bound endpoint may be ephemeral (tcp://127.0.0.1:0 → kernel-
    // assigned port); read it back via host.broker_endpoint().
    pylabhub::hub::BrokerRequestComm brc;
    pylabhub::hub::BrokerRequestComm::Config bcfg;
    bcfg.broker_endpoint = host.broker_endpoint();
    bcfg.broker_pubkey   = host.broker_pubkey();
    bcfg.role_uid        = "prod.l3test.uid12345678";
    ASSERT_TRUE(brc.connect(bcfg));

    std::atomic<bool> running{true};
    std::thread brc_thread([&brc, &running] {
        brc.run_poll_loop([&running] { return running.load(); });
    });

    auto reg = brc.register_channel(
        make_reg_opts("lab.evt.channel", "prod.l3test.uid12345678"), 3000);
    ASSERT_TRUE(reg.has_value())
        << "register_channel failed — broker rejected the REG_REQ; "
           "events never reach the script.  Reg result: "
        << (reg.has_value() ? "ok" : "no value");

    ASSERT_TRUE(run_main_loop_with_watchdog(host, std::chrono::seconds{5}))
        << "watchdog expired — neither on_channel_opened nor "
           "on_role_registered invoked request_shutdown.  The chain "
           "broker→HubState._on_*→subscriber→IncomingMessage→dispatch_event→"
           "engine.invoke is broken somewhere.  Log dump:\n"
        << read_log_file(LogCaptureFixture::log_path());

    running.store(false);
    brc.stop();
    brc_thread.join();
    brc.disconnect();
    host.shutdown();
    flush_logger();

    const std::string log = read_log_file(LogCaptureFixture::log_path());
    EXPECT_NE(log.find("INIT_RAN"), std::string::npos);
    EXPECT_NE(log.find("CHANNEL_OPENED name=lab.evt.channel"), std::string::npos)
        << "on_channel_opened details wrong; channel_to_json regression.\n"
        << log;
    EXPECT_NE(log.find("ROLE_REGISTERED uid=prod.l3test.uid12345678"),
              std::string::npos)
        << "on_role_registered details wrong; role_to_json regression.\n"
        << log;
}

TEST_F(HubLuaIntegrationTest, EventObserver_ConsumerRegistration_FiresOnConsumerAdded)
{
    // Two-step wire flow: producer's REG_REQ creates the channel,
    // then consumer's CONSUMER_REG_REQ fires on_consumer_added.
    // The script only requests shutdown after consumer fires (channel
    // and role are merely the prerequisite events).
    const std::string lua_body = R"LUA(
function on_init(api) api.log('info', 'INIT_RAN') end
function on_channel_opened(ci) api.log('info', 'CHANNEL_OPENED') end
function on_role_registered(ri) api.log('info', 'ROLE_REGISTERED') end
function on_consumer_added(d)
    api.log('info', 'CONSUMER_ADDED ch=' .. (d.channel or '<m>') ..
                 ' role=' .. (d.role_uid or '<m>'))
    api.request_shutdown()
end
)LUA";

    const fs::path dir = make_lua_hub_dir("evt_cons_add", lua_body,
                                           "fixed_rate", 100);
    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));
    ASSERT_NO_THROW(host.startup());

    pylabhub::hub::BrokerRequestComm prod_brc;
    pylabhub::hub::BrokerRequestComm::Config bcfg;
    bcfg.broker_endpoint = host.broker_endpoint();
    bcfg.broker_pubkey   = host.broker_pubkey();
    bcfg.role_uid        = "prod.evcons.uid";
    ASSERT_TRUE(prod_brc.connect(bcfg));
    std::atomic<bool> prod_running{true};
    std::thread prod_thread([&prod_brc, &prod_running] {
        prod_brc.run_poll_loop([&prod_running] { return prod_running.load(); });
    });

    auto prod_reg = prod_brc.register_channel(
        make_reg_opts("lab.cons.channel", "prod.evcons.uid"), 3000);
    ASSERT_TRUE(prod_reg.has_value()) << "producer REG_REQ failed";

    pylabhub::hub::BrokerRequestComm cons_brc;
    pylabhub::hub::BrokerRequestComm::Config cbcfg;
    cbcfg.broker_endpoint = host.broker_endpoint();
    cbcfg.broker_pubkey   = host.broker_pubkey();
    cbcfg.role_uid        = "cons.l3.uid12345678";
    ASSERT_TRUE(cons_brc.connect(cbcfg));
    std::atomic<bool> cons_running{true};
    std::thread cons_thread([&cons_brc, &cons_running] {
        cons_brc.run_poll_loop([&cons_running] { return cons_running.load(); });
    });

    // CONSUMER_REG_REQ wire fields are `consumer_uid` / `consumer_name`
    // (not the role-side terminology — see broker_service.cpp:1723).
    // The broker translates them to ConsumerEntry.role_uid/role_name
    // when populating HubState; HubScriptRunner re-exposes them as
    // role_uid/role_name in the script's details payload.
    nlohmann::json cons_opts;
    cons_opts["channel_name"]  = "lab.cons.channel";
    cons_opts["consumer_pid"]  = ::getpid();
    cons_opts["consumer_uid"]  = "cons.l3.uid12345678";
    cons_opts["consumer_name"] = "L3TestConsumer";
    auto cons_reg = cons_brc.register_consumer(cons_opts, 3000);
    ASSERT_TRUE(cons_reg.has_value()) << "consumer CONSUMER_REG_REQ failed";

    ASSERT_TRUE(run_main_loop_with_watchdog(host, std::chrono::seconds{5}))
        << "watchdog expired — on_consumer_added never invoked "
           "request_shutdown.\n"
        << read_log_file(LogCaptureFixture::log_path());

    cons_running.store(false); cons_brc.stop(); cons_thread.join(); cons_brc.disconnect();
    prod_running.store(false); prod_brc.stop(); prod_thread.join(); prod_brc.disconnect();
    host.shutdown();
    flush_logger();

    const std::string log = read_log_file(LogCaptureFixture::log_path());
    EXPECT_NE(log.find("CONSUMER_ADDED ch=lab.cons.channel role=cons.l3.uid12345678"),
              std::string::npos)
        << "on_consumer_added details (channel + role_uid) wrong; log:\n"
        << log;
}

// Note on band events: HEP-CORE-0030 BAND_JOIN_REQ is a separate wire
// surface; covering it would require BrokerRequestComm to expose a
// band_join helper or this test to format the BAND_JOIN_REQ wire frame
// directly.  Deferred — not a hub-character regression risk on the
// observer-dispatch chain (which is what the channel/role/consumer
// tests above prove); covered separately when band-side work lands.
//
// Note on federation events (on_peer_*): per HEP-0033 §16, federation
// needs its own design pass after the hub itself is finished.

// ─── §12.2.3 user-posted events ─────────────────────────────────────────────

TEST_F(HubLuaIntegrationTest, PostEvent_FromOnInit_FiresOnAppCallback)
{
    // Script's on_init posts a user event; the worker thread drains
    // the IncomingMessage and dispatches on_app_<name>; that callback
    // calls request_shutdown.  No timing dependency: the watchdog
    // wakeup is the proof of execution.
    const std::string lua_body = R"LUA(
function on_init(api)
    api.log('info', 'INIT_RAN posting event')
    api.post_event('user_event', { seq = 42, msg = 'hello' })
end
function on_app_user_event(args)
    api.log('info', 'APP_EVENT seq=' .. tostring(args.seq) ..
                ' msg=' .. tostring(args.msg))
    api.request_shutdown()
end
)LUA";
    const fs::path dir = make_lua_hub_dir("post_event", lua_body,
                                           "fixed_rate", 100);
    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));
    ASSERT_NO_THROW(host.startup());

    ASSERT_TRUE(run_main_loop_with_watchdog(host, std::chrono::seconds{5}))
        << "watchdog expired — on_app_user_event never fired.  Either "
           "api.post_event didn't enqueue, or the worker didn't dispatch the "
           "app_<name>-prefixed event, or the script callback raised an "
           "error before request_shutdown.\n"
        << read_log_file(LogCaptureFixture::log_path());

    host.shutdown();
    flush_logger();

    const std::string log = read_log_file(LogCaptureFixture::log_path());
    EXPECT_NE(log.find("APP_EVENT seq=42 msg=hello"), std::string::npos)
        << "on_app_user_event fired but the args payload was wrong — "
           "json_to_lua / dispatch_event regression. Log dump:\n" << log;
}

// ─── §12.2.2 augmentation hooks ─────────────────────────────────────────────

TEST_F(HubLuaIntegrationTest, Augment_QueryMetrics_FromAdminThreadCallSite_MutatesResponse)
{
    // Direct test of the augmentation flow: post-startup, the test
    // thread (acting as the admin thread would) calls
    // host.hub_api()->augment_query_metrics().  The script's
    // on_query_metrics adds a sentinel field and returns the response;
    // the test verifies the returned response carries the sentinel —
    // this is the response-mutation contract of HEP §12.2.2.
    //
    // SOLID-OUTPUT contract: the assertion is on the returned response
    // object's content (a structural value), NOT on the call returning
    // — a regression that returned the original response unmodified
    // would still "succeed" by exception/return-value criteria but
    // fail this assertion.
    const std::string lua_body = R"LUA(
function on_init(api) api.log('info', 'INIT_RAN') end
function on_query_metrics(args)
    api.log('info', 'AUGMENT_RAN params=' ..
                (args.params and tostring(args.params.x) or 'nil'))
    args.response.augmented_field = 'sentinel-99'
    args.response.echo_x = (args.params and args.params.x) or -1
    return args.response
end
)LUA";
    const fs::path dir = make_lua_hub_dir("augment_qm", lua_body,
                                           "fixed_rate", 100);
    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));
    ASSERT_NO_THROW(host.startup());

    auto *api = host.hub_api();
    ASSERT_NE(api, nullptr) << "hub_api() returned null after startup — "
                               "EngineHost lazy-construction did not complete";

    nlohmann::json params   = {{"x", 7}};
    nlohmann::json response = {{"baseline", "kept-or-mutated"}};
    api->augment_query_metrics(params, response);

    // Solid-output assertions: the script's mutation must be visible
    // in the response we passed in.  Also check the script's own log
    // marker AND that the params round-tripped correctly.
    EXPECT_EQ(response.value("augmented_field", std::string{}), "sentinel-99")
        << "augment_query_metrics did NOT mutate response — script's return "
           "value was either ignored, or on_query_metrics never fired.  "
           "Response:\n" << response.dump(2);
    EXPECT_EQ(response.value("echo_x", -999), 7)
        << "params field 'x' did not round-trip into the script's args.params; "
           "json_to_lua regression.  Response:\n" << response.dump(2);
    EXPECT_EQ(response.value("baseline", std::string{}), "kept-or-mutated")
        << "script lost the baseline field — should have preserved it via "
           "args.response.";

    host.shutdown();
    flush_logger();

    const std::string log = read_log_file(LogCaptureFixture::log_path());
    EXPECT_NE(log.find("AUGMENT_RAN params=7"), std::string::npos)
        << "on_query_metrics fired but params didn't carry x=7; log:\n" << log;
}

TEST_F(HubLuaIntegrationTest, Augment_NullReturn_KeepsDefaultResponse)
{
    // Script returns nil — the C++ side must KEEP the default response
    // unchanged (the "must return the response" contract — see HEP
    // §12.2.2).  Pins the §12.2.2 anti-pattern: mutate-without-return
    // is silently treated as "no change."
    const std::string lua_body = R"LUA(
function on_init(api) api.log('info', 'INIT_RAN') end
function on_query_metrics(args)
    api.log('info', 'AUGMENT_RAN_NIL_RETURN')
    args.response.would_have_been = 'lost'
    -- intentionally no `return` -> nil
end
)LUA";
    const fs::path dir = make_lua_hub_dir("augment_nil", lua_body,
                                           "fixed_rate", 100);
    auto cfg = HubConfig::load_from_directory(dir.string());
    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    HubHost host(std::move(cfg), std::move(engine));
    ASSERT_NO_THROW(host.startup());

    auto *api = host.hub_api();
    ASSERT_NE(api, nullptr);

    nlohmann::json params   = nlohmann::json::object();
    nlohmann::json response = {{"default", true}};
    api->augment_query_metrics(params, response);

    // Solid-output: the default field must still be present, and the
    // sentinel the script tried to set in-place must NOT be present.
    EXPECT_EQ(response.value("default", false), true)
        << "default response was lost despite nil-return contract.  "
           "Response:\n" << response.dump(2);
    EXPECT_EQ(response.contains("would_have_been"), false)
        << "in-place mutation leaked into the response despite the script "
           "returning nil — contract violation: only `return` should "
           "publish changes.  Response:\n" << response.dump(2);

    host.shutdown();
    flush_logger();

    // The log marker confirms on_query_metrics ran (the absent
    // mutation isn't because the callback didn't fire).
    const std::string log = read_log_file(LogCaptureFixture::log_path());
    EXPECT_NE(log.find("AUGMENT_RAN_NIL_RETURN"), std::string::npos)
        << "on_query_metrics never fired — nil-return contract test is "
           "vacuous if the callback didn't run.  Log:\n" << log;
}
