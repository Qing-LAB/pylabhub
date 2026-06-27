/**
 * @file hub_python_integration_workers.cpp
 * @brief Worker bodies for HubHost+PythonEngine integration tests (Pattern 3).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  pybind11's `py::scoped_interpreter`
 * cannot be re-initialized cleanly within one process (the L2
 * PythonEngine suite already uses Pattern 3 for the same reason);
 * the migration here brings the L3 hub-side integration test under
 * the same isolation rule.
 */

#include "hub_python_integration_workers.h"

#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/script_engine_factory.hpp"
#include "../../src/scripting/python_interpreter_module.hpp"

#include "curve_test_setup.h"
#include "log_capture_fixture.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::utils::Logger;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::config::HubConfig;
using pylabhub::utils::HubDirectory;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace hub_python_integration
{

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

fs::path make_python_hub_dir(const char *tag, const std::string &py_body,
                              std::vector<fs::path> &cleanup)
{
    const fs::path dir = unique_temp_dir(tag);
    HubDirectory::init_directory(dir, "L3PyHub");
    cleanup.push_back(dir);

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

} // namespace

int realpythonscript_oninit_onstop_fireandlog()
{
    return run_gtest_worker(
        [] {
            // HEP-CORE-0011 §"Engine Construction Lifecycle": register the
            // dispatching ScriptEngine factory + GIL helpers, then load
            // the PythonInterpreter persistent dynamic module on THIS
            // thread (subprocess main thread) — same role as plh_hub's
            // main() in production.  Py_FinalizeEx will run on this
            // thread at LifecycleGuard teardown via finalize() Phase 2.
            pylabhub::scripting::init_scripting();
            ASSERT_TRUE(pylabhub::scripting::ensure_python_interpreter_loaded())
                << "PythonInterpreter failed to load — Python tests cannot run";

            LogCaptureFixture log_cap;
            log_cap.Install();
            std::vector<fs::path> cleanup;

            // __init__.py exercises every HubAPI binding the Python
            // side exposes: lifecycle (log / uid / metrics), then the
            // read-accessor surface (name / config / list_* / get_* /
            // query_metrics).
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

    api.close_channel('no.such.channel')
    api.log('info', 'API_CLOSE_CHANNEL_OK')
    api.broadcast_channel('no.such.channel', 'hello')
    api.log('info', 'API_BROADCAST_CHANNEL_OK')
    api.broadcast_channel('no.such.channel', 'with-payload', 'payload-bytes')
    api.log('info', 'API_BROADCAST_CHANNEL_DATA_OK')

def on_stop(api):
    api.log('info', 'HUB_STOP uid=' + api.uid())
)PY";

            const fs::path dir = make_python_hub_dir("oninit", py_body, cleanup);

            auto cfg = HubConfig::load_from_directory(dir.string());
            const std::string expected_uid = cfg.identity().uid;
            ASSERT_FALSE(expected_uid.empty())
                << "init_directory must have generated a hub uid";

            // HEP-CORE-0040 §172 + HEP-CORE-0035 §4.6.5 bypass: seed
            // the process KeyStore with a fresh hub identity before
            // HubHost::startup() constructs BrokerService, which
            // requires key_store().has("hub_identity").  The on-disk
            // vault round-trip (Argon2id) is intentionally skipped per
            // §4.6.5 — the L2 vault layer is covered by
            // test_hub_config; this test exercises the broker bind +
            // CURVE wire from KeyStore-seeded state onward.  No roles
            // needed: this test does not drive a BRC client.
            auto curve = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test", "test.l3.python_hub_integration", curve);

            HubHost host(std::move(cfg));

            // Wall-clock bound on startup.  Observed steady-state on
            // dev hardware: ~120 ms (py::scoped_interpreter init +
            // import_role_script_module + pylabhub_hub lookup +
            // build_api + on_init + ready_promise round-trip).  Bound
            // 2500 ms gives ~20× headroom for slow CI / sanitizer
            // builds; still catches a sub-2-second slowdown.
            const auto t_startup = std::chrono::steady_clock::now();
            ASSERT_NO_THROW(host.startup());
            const auto startup_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t_startup).count();
            EXPECT_LT(startup_ms, 2500)
                << "host.startup() with real PythonEngine must complete "
                   "in <2500 ms; took " << startup_ms << " ms — regression "
                   "in interpreter init / load_script / build_api / "
                   "on_init paths";
            EXPECT_TRUE(host.is_running());

            host.shutdown();
            EXPECT_FALSE(host.is_running());

            // ── Pin the structural log content ──────────────────────
            const std::string log = read_log_file(log_cap.log_path());

            const auto pos_init = log.find("HUB_INIT");
            const auto pos_stop = log.find("HUB_STOP");

            ASSERT_NE(pos_init, std::string::npos)
                << "missing on_init log marker — api.log() did not "
                   "route through HubAPI::log() to the process logger.  "
                   "Log dump:\n" << log;
            ASSERT_NE(pos_stop, std::string::npos)
                << "missing on_stop log marker — script's on_stop "
                   "never fired.  Log dump:\n" << log;
            EXPECT_LT(pos_init, pos_stop)
                << "on_init must appear before on_stop in the log";

            const std::string init_line_marker =
                "HUB_INIT uid=" + expected_uid;
            EXPECT_NE(log.find(init_line_marker), std::string::npos)
                << "api.uid() must return cfg.identity().uid; expected "
                   "to find\n  " << init_line_marker << "\nLog dump:\n" << log;

            EXPECT_NE(log.find("[hub/" + expected_uid + "]"),
                      std::string::npos)
                << "HubAPI::log() must emit lines with prefix "
                   "[hub/<uid>]; not found in:\n" << log;

            // ── Read-accessor binding surface ───────────────────────
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
                    << "' — pybind11 binding failed to bind, raised, "
                       "or returned wrong type.\nLog dump:\n" << log;
            }

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            for (const auto &p : cleanup)
            {
                std::error_code ec;
                fs::remove_all(p, ec);
            }
        },
        "hub_python_integration::realpythonscript_oninit_onstop_fireandlog",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace hub_python_integration
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ─────────────────────────────────────────────────────

namespace
{

struct HubPythonIntegrationRegistrar
{
    HubPythonIntegrationRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "hub_python_integration")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_python_integration;

                if (sc == "realpythonscript_oninit_onstop_fireandlog")
                    return realpythonscript_oninit_onstop_fireandlog();
                return -1;
            });
    }
} g_registrar;

} // namespace
