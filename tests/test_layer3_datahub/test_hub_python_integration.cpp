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
    // __init__.py exercises all 3 hub bindings via the api positional arg:
    //   - api.log     — verified by grep'ing the log file
    //   - api.uid()   — appears inside the log message, pins the
    //                    binding actually returns HubHost's uid
    //   - api.metrics() — called for its side effect (returns a dict;
    //                    we don't read it from C++, but calling it
    //                    proves the closure round-trips json.loads
    //                    on the broker's empty-state metrics without
    //                    raising — which would land as a Python error
    //                    LogCaptureFixture would reject in TearDown)
    //
    // Mirrors role-side convention: callbacks receive `api` as their
    // first positional arg.  Same shape as D3.3's Lua script
    // (api.log("info", ...) — dot syntax, not method-call colon).
    const std::string py_body =
        "def on_init(api):\n"
        "    m = api.metrics()  # callable, returns dict, no exception\n"
        "    api.log('info', 'PHASE7_D42_INIT uid=' + api.uid())\n"
        "\n"
        "def on_stop(api):\n"
        "    api.log('info', 'PHASE7_D42_STOP uid=' + api.uid())\n";

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

    // Wall-clock bound on startup.  Real PythonEngine adds
    // py::scoped_interpreter creation + import_role_script_module +
    // pylabhub_hub binding lookup — generous bound (8 s, vs. 5 s for
    // Lua) but still tight enough to catch a regression that degrades
    // into a multi-second hang.  Python init is heavier than LuaJIT.
    const auto t_startup = std::chrono::steady_clock::now();
    ASSERT_NO_THROW(host.startup());
    const auto startup_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_startup).count();
    EXPECT_LT(startup_ms, 8000)
        << "host.startup() with real PythonEngine must complete in <8 s; "
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

    const auto pos_init = log.find("PHASE7_D42_INIT");
    const auto pos_stop = log.find("PHASE7_D42_STOP");

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
    const std::string init_line_marker = "PHASE7_D42_INIT uid=" + expected_uid;
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
}
