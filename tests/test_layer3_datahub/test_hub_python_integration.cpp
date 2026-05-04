/**
 * @file test_hub_python_integration.cpp
 * @brief L3 integration test: HubHost + PythonEngine + real __init__.py
 *        (HEP-CORE-0033 Phase 7 D4.2; mirrors test_hub_lua_integration.cpp).
 *
 * End-to-end verification of the Phase 7 D4 stack:
 *   1. HubHost ctor accepts a real `PythonEngine` via the 2-arg ctor
 *      (engine factory dispatch covers script.type == "python").
 *   2. `HubScriptRunner::worker_main_` runs the engine setup
 *      sequence (initialize → load_script → build_api) and reaches
 *      `invoke_on_init` synchronously.
 *   3. `PythonEngine::build_api_(HubAPI&)` (D4.1) imports the
 *      `pylabhub_hub` PYBIND11_EMBEDDED_MODULE, casts api as
 *      `pylabhub_hub.HubAPI`, sets it as `<script_module>.api`, and
 *      stores it in `api_obj_` so invoke_on_init/on_stop pass it as
 *      a positional arg.
 *   4. The Python script's `on_init(api)` callback runs; the log
 *      line it emits via `api.log()` lands in the process logger
 *      sink with the `[hub/<uid>]` prefix produced by HubAPI::log()
 *      (independent of engine type — same prefix as Lua test).
 *   5. `HubHost::shutdown()` triggers `on_stop`, drains the runner,
 *      and joins the worker thread cleanly.
 *
 * ## Single-test rationale
 *
 * Only ONE TEST_F runs Python in this binary on purpose.  pybind11's
 * `py::scoped_interpreter` calls `Py_InitializeEx` / `Py_FinalizeEx`
 * around the engine's lifetime; multiple init/finalize cycles in a
 * single process are problematic for embedded modules (the pybind11
 * `internals` singleton + ctypes module state may not fully reset).
 * The L2 PythonEngine test suite uses Pattern 3 (subprocess workers)
 * for exactly this reason — see tests/test_layer2_service/test_python_engine.cpp.
 *
 * Coverage rationale: the HubScriptRunner setup-failure path
 * (load_script returns false → ready_promise(false) → startup throws)
 * is engine-type-agnostic and already covered by D3.3
 * `ScriptSyntaxError_StartupThrows`.  We don't repeat it for Python.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"

#include "engine_factory.hpp"

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
                 ("plh_l3_py_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
                  std::to_string(ctr.fetch_add(1)));
    fs::remove_all(d);
    return d;
}

std::string read_log_file(const fs::path &path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ─── Fixture ────────────────────────────────────────────────────────────────

class HubPythonIntegrationTest : public ::testing::Test,
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

    /// Build a hub directory wired for in-process L3 testing with a
    /// Python script:
    ///   - ephemeral broker endpoint (avoids port collision)
    ///   - admin disabled (Phase 6.2 not under test here)
    ///   - script.type=python, script.path="."
    ///     (resolves to <dir>/script/python/__init__.py)
    /// @param tag      directory-name tag (for grep-ability)
    /// @param py_body  verbatim body of __init__.py written under
    ///                 `<dir>/script/python/__init__.py`
    /// @return         absolute path to the created hub directory
    fs::path make_python_hub_dir(const char *tag, const std::string &py_body)
    {
        const fs::path dir = unique_temp_dir(tag);
        HubDirectory::init_directory(dir, "L3PyHub");
        paths_to_clean_.push_back(dir);

        json j;
        {
            std::ifstream f(dir / "hub.json");
            j = json::parse(f);
        }
        j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
        j["admin"]["enabled"]           = false;
        j["script"]["type"]             = "python";
        j["script"]["path"]             = ".";
        {
            std::ofstream f(dir / "hub.json");
            f << j.dump(2);
        }

        fs::create_directories(dir / "script" / "python");
        {
            std::ofstream f(dir / "script" / "python" / "__init__.py");
            f << py_body;
        }
        return dir;
    }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
    std::vector<fs::path>                  paths_to_clean_;
};

std::unique_ptr<LifecycleGuard> HubPythonIntegrationTest::s_lifecycle_;

// ─── Tests ─────────────────────────────────────────────────────────────────

TEST_F(HubPythonIntegrationTest, RealPythonScript_OnInitOnStop_FireAndLog)
{
    // __init__.py exercises every HubAPI binding the Python side
    // expose: lifecycle (log / uid / metrics), then the read-accessor
    // surface (name / config / list_* / get_* / query_metrics).
    // Single-test design constraint: pybind11 scoped_interpreter
    // re-init in one process is unsafe (see L2 PythonEngine subprocess
    // pattern), so every Python-side binding assertion must live in
    // this one test.
    //
    // Markers HUB_INIT / HUB_STOP exercise lifecycle; each accessor
    // emits an API_*_OK marker after a type-shape check.  Greps below
    // assert all markers present.
    //
    // Lists are empty in this isolated hub (no real producer/consumer
    // connects).  get_*(missing) returns Python None.  Functional
    // content (entries appearing post-registration) is covered by
    // future end-to-end tests that drive the broker; this test pins
    // the BINDING surface — every method is reachable, types are
    // correct, no Python exceptions.
    const std::string py_body = R"PY(
def on_init(api):
    api.log('info', 'HUB_INIT uid=' + api.uid())
    m = api.metrics()  # callable, returns dict, no exception
    if isinstance(m, dict):
        api.log('info', 'METRICS_OK')

    # Phase 8a read accessors.
    n = api.name()
    if isinstance(n, str) and len(n) > 0:
        api.log('info', 'API_NAME_OK:' + n)

    c = api.config()
    if isinstance(c, dict) and 'hub' in c and 'admin' in c:
        api.log('info', 'API_CONFIG_OK')

    # list_*() — all return lists (empty in isolated hub).
    if isinstance(api.list_channels(), list):
        api.log('info', 'API_LIST_CHANNELS_OK')
    if isinstance(api.list_roles(), list):
        api.log('info', 'API_LIST_ROLES_OK')
    if isinstance(api.list_bands(), list):
        api.log('info', 'API_LIST_BANDS_OK')
    if isinstance(api.list_peers(), list):
        api.log('info', 'API_LIST_PEERS_OK')

    # get_*(missing) — None.
    if api.get_channel('no.such.channel') is None:
        api.log('info', 'API_GET_CHANNEL_NIL_OK')
    if api.get_role('no.such.role') is None:
        api.log('info', 'API_GET_ROLE_NIL_OK')
    if api.get_band('no.such.band') is None:
        api.log('info', 'API_GET_BAND_NIL_OK')
    if api.get_peer('no.such.peer') is None:
        api.log('info', 'API_GET_PEER_NIL_OK')

    # query_metrics() — both shapes.
    if isinstance(api.query_metrics(), dict):
        api.log('info', 'API_QUERY_METRICS_ALL_OK')
    if isinstance(api.query_metrics(['counters']), dict):
        api.log('info', 'API_QUERY_METRICS_FILTERED_OK')

    # Control delegates — fire-and-forget; broker tolerates unknown
    # channels idempotently.  Pins the BINDING surface only.
    # request_shutdown is exercised by the Lua test's dedicated case
    # (it shuts the hub down, conflicting with this test's lifecycle).
    api.close_channel('no.such.channel')
    api.log('info', 'API_CLOSE_CHANNEL_OK')
    api.broadcast_channel('no.such.channel', 'hello')
    api.log('info', 'API_BROADCAST_CHANNEL_OK')
    api.broadcast_channel('no.such.channel', 'with-payload', 'payload-bytes')
    api.log('info', 'API_BROADCAST_CHANNEL_DATA_OK')

def on_stop(api):
    api.log('info', 'HUB_STOP uid=' + api.uid())
)PY";

    const fs::path dir = make_python_hub_dir("oninit", py_body);

    auto cfg = HubConfig::load_from_directory(dir.string());
    const std::string expected_uid = cfg.identity().uid;
    ASSERT_FALSE(expected_uid.empty())
        << "init_directory must have generated a hub uid";

    auto engine =
        pylabhub::scripting::make_engine_from_script_config(cfg.script());
    ASSERT_NE(engine, nullptr)
        << "factory must return a PythonEngine for type=python";

    HubHost host(std::move(cfg), std::move(engine));

    // Wall-clock bound on startup.  Observed steady-state on dev
    // hardware: ~120 ms (py::scoped_interpreter init +
    // import_role_script_module + pylabhub_hub lookup + build_api +
    // on_init + ready_promise round-trip).  Bound 2500 ms gives ~20×
    // headroom for slow CI / sanitizer builds + first-run venv import
    // cache warmup; still catches a sub-2-second slowdown (looser
    // bounds — the original 8 s — accept multi-second regressions
    // silently, the failure mode CLAUDE.md "tests must pin path,
    // timing, and structure" forbids).  Tighten further only if CI
    // false-positives.  Python's bound is wider than Lua's 1500 ms
    // because Python init is heavier (interpreter + module import).
    const auto t_startup = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(host.startup());
    const auto startup_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_startup).count();
    EXPECT_LT(startup_ms, 2500)
        << "host.startup() with real PythonEngine must complete in <2500 ms; "
           "took " << startup_ms << " ms — regression in interpreter init / "
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
    const std::string log = read_log_file(LogCaptureFixture::log_path());

    const auto pos_init = log.find("HUB_INIT");
    const auto pos_stop = log.find("HUB_STOP");

    ASSERT_NE(pos_init, std::string::npos)
        << "missing on_init log marker — api.log() did not route through "
           "HubAPI::log() to the process logger.  Log dump:\n" << log;
    ASSERT_NE(pos_stop, std::string::npos)
        << "missing on_stop log marker — script's on_stop never fired.  "
           "Log dump:\n" << log;
    EXPECT_LT(pos_init, pos_stop)
        << "on_init must appear before on_stop in the log";

    // Pin api.uid() actually returns the configured uid (not empty
    // string, not garbage).  Both markers carry it; checking on_init
    // is enough.
    const std::string init_line_marker = "HUB_INIT uid=" + expected_uid;
    EXPECT_NE(log.find(init_line_marker), std::string::npos)
        << "api.uid() must return cfg.identity().uid; expected to find\n  "
        << init_line_marker << "\nLog dump:\n" << log;

    // Pin the [hub/<uid>] log_tag prefix produced by HubAPI::log().
    // HubAPI::log() routes through LOGGER_INFO with the [hub/<uid>]
    // prefix per src/utils/service/hub_api.cpp — same shape regardless
    // of script engine.  This catches a regression that swaps the
    // prefix between engines (the Lua and Python paths must converge
    // on the same HubAPI::log() implementation, not duplicate it).
    EXPECT_NE(log.find("[hub/" + expected_uid + "]"), std::string::npos)
        << "HubAPI::log() must emit lines with prefix [hub/<uid>]; "
           "not found in:\n" << log;

    // ── Read-accessor binding surface ───────────────────────────────────
    //
    // Each accessor emits an API_*_OK marker after passing a
    // type-shape check inside on_init.  Failing to bind, raising on
    // call, or returning the wrong shape leaves the marker missing here.
    // (A raised Python exception would also land as LOGGER_ERROR via
    // on_python_error_, which TearDown's AssertNoUnexpectedLogWarnError
    // would reject — but the marker grep is the affirmative signal.)
    static constexpr const char *kAccessorMarkers[] = {
        "METRICS_OK",
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
    for (const char *m : kAccessorMarkers)
    {
        EXPECT_NE(log.find(m), std::string::npos)
            << "missing read-accessor binding marker '" << m
            << "' — pybind11 binding failed to bind, raised, or returned "
               "wrong type.\nLog dump:\n" << log;
    }
}
