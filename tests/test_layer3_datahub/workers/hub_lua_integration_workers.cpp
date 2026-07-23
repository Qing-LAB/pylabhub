/**
 * @file hub_lua_integration_workers.cpp
 * @brief Worker bodies for HubHost + LuaEngine integration tests
 *        (HEP-CORE-0033 Phase 7 D3.3 + HEP-CORE-0011 §"Engine
 *        Construction Lifecycle"; Pattern 3).
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Pattern 3 fits well here: each
 * subprocess gets a fresh LuaJIT runtime state (no shared interpreter
 * across tests), eliminating the in-process re-init fragility that
 * the suite-level guard was working around.
 *
 * ── RATIONALE — HubHostBrokerHandle sweep disposition (task #52) ─────────────
 * KEEP the in-process co-host, INTRINSICALLY.  The event-observer tests pin the
 * FULL production chain wire → BrokerService → HubState → subscriber →
 * HubScriptRunner queue → worker thread → LuaEngine.invoke.  The subject under
 * test is that whole in-process stack (HubHost + HubScriptRunner + real
 * LuaEngine); a Pattern 4 subprocess broker + raw `BrokerWireClient` has no
 * LuaEngine on the far side, so it would test a different thing entirely.  One
 * `HubHost` broker = one ZAP pump; a single `BrokerRequestComm` client (DEALER,
 * no ZAP pump) — not the HEP-CORE-0036 §7.4 single-pumper antipattern.  (The
 * sweep's symbol enumeration missed this file because it co-hosts via
 * `HubHost::startup()`, not the `HubHostBrokerHandle` harness symbol.)
 *
 * Real production wiring per feedback_test_layering_and_no_mocks.md:
 *   - Real `HubConfig::load_from_directory` from a real hub directory
 *     written to a per-worker temp dir.
 *   - Real `HubHost` + real `HubScriptRunner` + real `LuaEngine`
 *     (constructed by the runner via the registered factory exactly
 *     the way `plh_hub`'s `main()` does).
 *   - Real `init.lua` on disk under `<dir>/script/lua/init.lua`.
 *   - Real wire-protocol REG_REQ / CONSUMER_REG_REQ via real
 *     `BrokerRequestComm` for the event-observer tests (full chain:
 *     wire → BrokerService → HubState → subscriber →
 *     HubScriptRunner queue → worker thread drain → engine.invoke).
 *     No friend-access shortcut — the framework leads to the calling.
 *
 * Per-worker body assertions transplanted verbatim from the original,
 * preserving the file's path/timing/structure pins:
 *   - startup speed bound (test #1: <1500 ms)
 *   - exception type + message substring (test #2)
 *   - tick-count bounds with explicit floor/ceiling rationale (#3, #4)
 *   - `run_main_loop_with_watchdog` as "callback executed" proof (#6–#9)
 *   - response-field pins for HEP §12.2.2 augmentation contract (#10, #11)
 *
 * @see HEP-CORE-0033 §12 (HubAPI surface, dispatch model, event observers,
 *      augmentation hooks, user-posted events)
 * @see HEP-CORE-0011 §"Engine Construction Lifecycle" (factory registry,
 *      worker-thread engine construction)
 */

#include "hub_lua_integration_workers.h"

#include "curve_test_setup.h"
#include "hub_vault_test_seed.h" // provision_hub_vault + load_hub_keypair_fresh (HEP-0035 §4.8)
#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_api.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_reg_payload.hpp"
#include "utils/script_engine_factory.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/known_roles.hpp"
#include "utils/security/shm_capability_channel.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::HubDirectory;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace hub_lua_integration
{

namespace
{

// ── Anon-namespace utilities (preserved from the original fixture) ──────────

fs::path unique_temp_dir(const char *tag)
{
    static std::atomic<int> ctr{0};
    fs::path d = fs::temp_directory_path() /
                 ("plh_l3_lua_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
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

int count_markers(const std::string &haystack, const std::string &marker)
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

/// Build a hub directory wired for in-process L3 testing with Lua scripts.
/// Returns the absolute path to the created directory.  Caller owns
/// cleanup (worker bodies `remove_all` at the end).
fs::path make_lua_hub_dir(const char *tag, const std::string &lua_body,
                          const std::string &loop_timing = "fixed_rate",
                          int target_period_ms = 1000)
{
    const fs::path dir = unique_temp_dir(tag);
    HubDirectory::init_directory(dir, "L3LuaHub");

    json j;
    {
        std::ifstream f(dir / "hub.json");
        j = json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"] = false;
    j["script"]["type"] = "lua";
    j["script"]["path"] = ".";
    j["loop_timing"] = loop_timing;
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

/// Run host.run_main_loop on a watchdog future.  Returns true if it
/// returned within @p timeout (something actually called
/// request_shutdown — proves the script-side callback executed).
bool run_main_loop_with_watchdog(HubHost &host, std::chrono::milliseconds timeout)
{
    auto fut = std::async(std::launch::async, [&host]() { host.run_main_loop(); });
    return fut.wait_for(timeout) == std::future_status::ready;
}

void flush_logger()
{
    Logger::instance().flush();
}

json make_reg_opts(const std::string &channel, const std::string &uid)
{
    // HEP-CORE-0036 §5b canonical shape — broker hard-errors on missing
    // role_type / data_transport / zmq_pubkey post-#290.  The pubkey is
    // read from the seeded KeyStore identity (HEP-CORE-0040 §172).
    namespace sec = pylabhub::utils::security;
    auto opts = pylabhub::hub::build_producer_reg_payload(pylabhub::hub::ProducerRegInputs{
        .channel = channel,
        .role_uid = uid,
        .role_name = "L3TestProducer",
        .role_type = "producer",
        .has_shm = true,
        .is_zmq_transport = false,
        .zmq_node_endpoint = {},
        .zmq_pubkey =
            std::string{sec::secure().keys().pubkey(pylabhub::tests::role_keystore_name(uid))},
        .shm_capability_endpoint = sec::default_shm_capability_endpoint(channel),
    });
    opts["producer_pid"] = ::getpid();
    return opts;
}

/// HEP-CORE-0036 §5b canonical consumer REG_REQ.
json make_cons_opts(const std::string &channel, const std::string &uid)
{
    namespace sec = pylabhub::utils::security;
    auto opts = pylabhub::hub::build_consumer_reg_payload(pylabhub::hub::ConsumerRegInputs{
        .channel = channel,
        .role_uid = uid,
        .role_name = "L3TestConsumer",
        .role_type = "consumer",
        .data_transport = "zmq",
        .zmq_pubkey =
            std::string{sec::secure().keys().pubkey(pylabhub::tests::role_keystore_name(uid))},
    });
    opts["consumer_pid"] = ::getpid();
    return opts;
}

void remove_tree(const fs::path &p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

/// Per-test contract for CURVE admission setup in this file:
///
///   A test that NEVER opens a `BrokerRequestComm` ("BRC-less" — only
///   exercises in-process HubHost / script lifecycle / admin paths)
///   passes `make_curve_setup({})` to seed `hub_identity` only.
///   No roles are seeded; `known_roles.json` is not written.  Nothing
///   ever triggers Layer-1 ZAP admission, so an empty admission file
///   is correct.
///
///   A test that DOES open a BRC and tries to register must:
///     1. List every BRC uid in `make_curve_setup({uid1, uid2, ...})`
///        so `seed_curve_identities()` seeds the per-role seckey under
///        `role.<uid>` (HEP-CORE-0040 §172).  The BRC reads it via
///        `keystore_name`.
///     2. Set `bcfg.keystore_name = role_keystore_name(uid)` on each
///        `BrokerRequestComm::Config` before `connect()`.  Default is
///        `"role_identity"` which the fixture does NOT seed.
///     3. Call `write_known_roles(dir, setup)` so the broker's ZAP
///        admission file lists those uids' pubkeys.
///     4. Use `make_reg_opts` / `make_cons_opts` below — they emit
///        HEP-CORE-0036 §5b canonical shape (the broker hard-errors
///        on missing role_type / data_transport / zmq_pubkey).
///
/// Module list every worker installs: Logger + FileLock + JsonConfig
/// + Crypto + ZMQContext.  HubConfig::load needs FileLock/JsonConfig;
/// the broker thread needs Logger/Crypto/ZMQContext.  HubScriptRunner
/// also calls `pylabhub::scripting::init_scripting()` — done at the
/// top of each worker body so the factory registry is populated
/// before `HubScriptRunner::worker_main_` step 0 reaches
/// `pylabhub::scripting::create_engine` (HEP-CORE-0011).
#define PLH_LUA_INT_MODS                                                                           \
    Logger::GetLifecycleModule(), FileLock::GetLifecycleModule(),                                  \
        JsonConfig::GetLifecycleModule(),                                                          \
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),                          \
        pylabhub::hub::GetZMQContextModule()

} // namespace

// ─── Test #1 ────────────────────────────────────────────────────────────────

int real_lua_script_on_init_on_stop_fire_and_log()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

            // init.lua exercises all 3 hub closures via api global:
            //   - api.log     — verified by grep'ing the log file
            //   - api.uid     — appears inside the log message, pins
            //                   the binding actually returns HubHost's uid
            //   - api.metrics — called for its side effect (proves the
            //                   closure is callable + json_to_lua
            //                   round-trips the broker's empty-state
            //                   metrics without errors)
            const std::string lua_body = "function on_init(api)\n"
                                         "    local m = api.metrics()  -- callable, no error\n"
                                         "    api.log(\"info\", \"HUB_INIT uid=\" .. api.uid())\n"
                                         "end\n"
                                         "function on_stop(api)\n"
                                         "    api.log(\"info\", \"HUB_STOP uid=\" .. api.uid())\n"
                                         "end\n";

            const fs::path dir = make_lua_hub_dir("oninit", lua_body);

            auto cfg = HubConfig::load_from_directory(dir.string());
            const std::string expected_uid = cfg.identity().uid;
            ASSERT_FALSE(expected_uid.empty()) << "init_directory must have generated a hub uid";

            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));

            // Wall-clock bound on startup.  Observed steady-state on
            // dev hardware: ~110 ms.  Bound 1500 ms gives ~13× headroom
            // for slow CI / sanitizer builds while still catching a
            // sub-second slowdown.
            const auto t_startup = std::chrono::steady_clock::now();
            ASSERT_NO_THROW(host.startup());
            const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t_startup)
                                        .count();
            EXPECT_LT(startup_ms, 1500)
                << "host.startup() with real LuaEngine must complete in "
                   "<1500 ms; took "
                << startup_ms
                << " ms — regression "
                   "in engine init / load_script / build_api / on_init paths";
            EXPECT_TRUE(host.is_running());

            host.shutdown();
            EXPECT_FALSE(host.is_running());

            // ── Pin the structural log content ──────────────────────
            const std::string log = read_log_file(log_cap.log_path());

            const auto pos_init = log.find("HUB_INIT");
            const auto pos_stop = log.find("HUB_STOP");

            ASSERT_NE(pos_init, std::string::npos)
                << "missing on_init log marker — api:log() did not route "
                   "through HubAPI::log() to the process logger.  Log:\n"
                << log;
            ASSERT_NE(pos_stop, std::string::npos)
                << "missing on_stop log marker — script's on_stop never "
                   "fired.  Log:\n"
                << log;
            EXPECT_LT(pos_init, pos_stop) << "on_init must appear before on_stop in the log";

            const std::string init_line_marker = "HUB_INIT uid=" + expected_uid;
            EXPECT_NE(log.find(init_line_marker), std::string::npos)
                << "api:uid() must return cfg.identity().uid; expected:\n  " << init_line_marker
                << "\nLog:\n"
                << log;

            EXPECT_NE(log.find("[hub/" + expected_uid + "]"), std::string::npos)
                << "HubAPI::log() must emit lines with prefix "
                   "[hub/<uid>]; not found in:\n"
                << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::real_lua_script_on_init_on_stop_fire_and_log", PLH_LUA_INT_MODS);
}

// A real Lua hub script calls api.admin_console_print — a structured table AND
// a bare string — from on_init.  Proves the whole script→buffer path
// (HEP-CORE-0033 §11.0.4): the Lua binding (lua_to_json) → HubAPI coercion
// (bare string → {"message":…}) → HubState console buffer → drain.  Verified by
// draining the host's output buffer directly (no admin console needed).
int real_lua_script_admin_console_print()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

            const std::string lua_body =
                "function on_init(api)\n"
                "    api.admin_console_print({event=\"hub_ready\", uid=api.uid()})\n"
                "    api.admin_console_print(\"plain hub message\")\n"
                "end\n";

            const fs::path dir = make_lua_hub_dir("aconsole", lua_body);
            auto cfg = HubConfig::load_from_directory(dir.string());
            const std::string expected_uid = cfg.identity().uid;

            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());
            EXPECT_TRUE(host.is_running());

            // on_init has fired during startup — drain the console buffer and
            // pin the two lines it appended (drain BEFORE shutdown so the
            // buffer is empty at teardown → no flush-to-log warning).
            const json drained = host.drain_console_output();
            EXPECT_EQ(drained.value("status", std::string{}), "ok");
            const auto &lines = drained["lines"];
            ASSERT_EQ(lines.size(), 2u) << "expected 2 admin_console_print lines; got:\n"
                                        << drained.dump(2);

            // Line 0 — a structured table passes through unchanged; it is
            // script-originated so request_id is empty.
            EXPECT_TRUE(lines[0].value("request_id", std::string{"X"}).empty());
            EXPECT_EQ(lines[0]["content"].value("event", std::string{}), "hub_ready");
            EXPECT_EQ(lines[0]["content"].value("uid", std::string{}), expected_uid);
            EXPECT_TRUE(lines[0].contains("ts"));

            // Line 1 — a bare string is coerced to {"message": …} by HubAPI.
            EXPECT_EQ(lines[1]["content"].value("message", std::string{}), "plain hub message");

            host.shutdown();
            EXPECT_FALSE(host.is_running());

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::real_lua_script_admin_console_print", PLH_LUA_INT_MODS);
}

// ─── Test #2 ────────────────────────────────────────────────────────────────

int script_syntax_error_startup_throws()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            // Two ERROR lines are expected on this path; declare them
            // up-front so LogCaptureFixture's AssertNoUnexpectedLogWarnError
            // doesn't reject them as stray.
            log_cap.ExpectLogError("Failed to load");
            log_cap.ExpectLogError("engine.load_script");
            pylabhub::scripting::init_scripting();

            const std::string broken_lua = "function on_init(api)\n"
                                           "    this is not valid lua syntax\n"
                                           "end\n";

            const fs::path dir = make_lua_hub_dir("syntax", broken_lua);

            auto cfg = HubConfig::load_from_directory(dir.string());
            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));

            try
            {
                host.startup();
                FAIL() << "startup() must throw when init.lua has a "
                          "syntax error";
            }
            catch (const std::runtime_error &e)
            {
                const std::string what = e.what();
                EXPECT_NE(what.find("script runner failed to start"), std::string::npos)
                    << "expected runtime_error mentioning the runner-startup "
                       "failure; got: "
                    << what;
            }
            catch (const std::exception &e)
            {
                FAIL() << "startup() threw wrong exception type: " << e.what();
            }

            EXPECT_FALSE(host.is_running());

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::script_syntax_error_startup_throws", PLH_LUA_INT_MODS);
}

// ─── Test #3 ────────────────────────────────────────────────────────────────

int on_tick_fires_periodically_when_idle()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

            // T1 — baseline contract: idle hub fires on_tick once per
            // period via the timeout-driven wake.  FixedRate at 100 ms;
            // run for 550 ms; expect 4-6 ticks.
            const std::string lua_body = "function on_tick()\n"
                                         "    api.log('info', 'TICK_IDLE')\n"
                                         "end\n";

            const fs::path dir = make_lua_hub_dir("tick_idle", lua_body, "fixed_rate", 100);

            auto cfg = HubConfig::load_from_directory(dir.string());

            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());

            std::this_thread::sleep_for(std::chrono::milliseconds{550});

            host.shutdown();

            const std::string log = read_log_file(log_cap.log_path());
            const int tick_count = count_markers(log, "TICK_IDLE");

            EXPECT_GE(tick_count, 4) << "expected >= 4 on_tick markers in 550 ms with "
                                        "period=100 ms; got "
                                     << tick_count
                                     << " — regression in deadline-driven tick (timeout path) "
                                        "or loop init.\nLog:\n"
                                     << log;
            EXPECT_LE(tick_count, 6) << "expected <= 6 on_tick markers in 550 ms with "
                                        "period=100 ms; got "
                                     << tick_count
                                     << " — regression in deadline advancement (over-firing)."
                                        "\nLog:\n"
                                     << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::on_tick_fires_periodically_when_idle", PLH_LUA_INT_MODS);
}

// ─── Test #4 ────────────────────────────────────────────────────────────────

int on_tick_catch_up_fixed_rate_with_compensation()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

            // T4 — FixedRateWithCompensation catch-up semantic.  Stall
            // first tick ~500 ms via FFI nanosleep (wall-clock-correct,
            // not CPU-time).  Expected total ticks: 6-12 (stalled #1 +
            // ≥4 catch-up + ≥1 normal).  Mutation-protects post-fix
            // "deadline only advances when tick fires" — pre-fix,
            // compute_next_deadline ran on every iteration including
            // event-driven wakes that didn't fire a tick, silently
            // downgrading FRWithComp to FixedRate-with-skips.
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
                "        local req = ffi.new('plh_ts', 0, 500 * 1000 * 1000)\n"
                "        ffi.C.nanosleep(req, nil)\n"
                "        api.log('info', 'TICK_STALL_DONE')\n"
                "    end\n"
                "end\n";

            const fs::path dir =
                make_lua_hub_dir("tick_catchup", lua_body, "fixed_rate_with_compensation", 100);

            auto cfg = HubConfig::load_from_directory(dir.string());

            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());

            std::this_thread::sleep_for(std::chrono::milliseconds{800});

            host.shutdown();

            const std::string log = read_log_file(log_cap.log_path());
            const int tick_count = count_markers(log, "TICK_CATCHUP");

            EXPECT_GE(tick_count, 6) << "expected >= 6 on_tick markers (stalled #1 + >=4 "
                                        "catch-up + >=1 normal); got "
                                     << tick_count
                                     << " — regression in FRWithComp catch-up: deadline "
                                        "likely being advanced on non-tick iterations again, "
                                        "swallowing missed slots.\nLog:\n"
                                     << log;
            EXPECT_LE(tick_count, 12) << "expected <= 12 on_tick markers; got " << tick_count
                                      << " — regression in catch-up termination: deadline "
                                         "isn't advancing past `now` after the burst, so "
                                         "on_tick keeps firing every iteration even at "
                                         "normal pace.\nLog:\n"
                                      << log;

            const auto stall_done = log.find("TICK_STALL_DONE");
            ASSERT_NE(stall_done, std::string::npos) << "stalled tick #1 never completed";

            const std::string post_stall = log.substr(stall_done);
            const int post_stall_ticks = count_markers(post_stall, "TICK_CATCHUP");
            EXPECT_GE(post_stall_ticks, 5) << "expected >= 5 ticks after stall_done (catch-up "
                                              "burst + normal); got "
                                           << post_stall_ticks
                                           << " — regression in catch-up: deadline not walking "
                                              "the grid forward.\nLog after stall:\n"
                                           << post_stall;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::on_tick_catch_up_fixed_rate_with_compensation", PLH_LUA_INT_MODS);
}

// ─── Test #5 ────────────────────────────────────────────────────────────────

int read_accessors_all_reachable_from_on_init()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

            const std::string lua_body = R"LUA(
function on_init(api)
    local n = api.name()
    if type(n) == 'string' and #n > 0 then
        api.log('info', 'API_NAME_OK:' .. n)
    end

    local c = api.config()
    if type(c) == 'table' and type(c.hub) == 'table' and
       type(c.admin) == 'table' then
        api.log('info', 'API_CONFIG_OK')
    end

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

    if type(api.query_metrics({})) == 'table' then
        api.log('info', 'API_QUERY_METRICS_ALL_OK')
    end

    local m_filt = api.query_metrics({'counters'})
    if type(m_filt) == 'table' then
        api.log('info', 'API_QUERY_METRICS_FILTERED_OK')
    end

    api.close_channel('no.such.channel')
    api.log('info', 'API_CLOSE_CHANNEL_OK')

    api.broadcast_channel('no.such.channel', 'hello')
    api.log('info', 'API_BROADCAST_CHANNEL_OK')

    api.broadcast_channel('no.such.channel', 'with-payload', 'payload-bytes')
    api.log('info', 'API_BROADCAST_CHANNEL_DATA_OK')
end
)LUA";

            const fs::path dir = make_lua_hub_dir("read_accessors", lua_body);

            auto cfg = HubConfig::load_from_directory(dir.string());
            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));

            ASSERT_NO_THROW(host.startup());
            host.shutdown();

            const std::string log = read_log_file(log_cap.log_path());

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
                    << "' — HubAPI read accessor failed to bind, "
                       "returned wrong type, or the Lua test code "
                       "mistyped it.\nLog:\n"
                    << log;
            }

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::read_accessors_all_reachable_from_on_init", PLH_LUA_INT_MODS);
}

// ─── Test #6 ────────────────────────────────────────────────────────────────

int request_shutdown_from_on_init_wakes_main_loop()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

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
            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));

            ASSERT_NO_THROW(host.startup());

            const auto t_begin = std::chrono::steady_clock::now();
            std::thread main_thread([&host]() { host.run_main_loop(); });
            main_thread.join();
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - t_begin)
                                        .count();

            EXPECT_LT(elapsed_ms, 2000) << "run_main_loop must wake within 2 s of startup (the "
                                           "script's on_init calls api.request_shutdown which "
                                           "should set the host shutdown flag immediately).  "
                                           "took "
                                        << elapsed_ms
                                        << " ms — regression in "
                                           "HubAPI::request_shutdown delegation.";

            host.shutdown();
            EXPECT_FALSE(host.is_running());

            const std::string log = read_log_file(log_cap.log_path());
            EXPECT_NE(log.find("BEFORE_SHUTDOWN"), std::string::npos) << "on_init never ran; log:\n"
                                                                      << log;
            EXPECT_NE(log.find("AFTER_SHUTDOWN_REQUEST"), std::string::npos)
                << "api.request_shutdown raised an error before "
                   "returning, or Lua aborted the script; log:\n"
                << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::request_shutdown_from_on_init_wakes_main_loop", PLH_LUA_INT_MODS);
}

// ─── Test #7 ────────────────────────────────────────────────────────────────

int event_observers_channel_registration_fires_on_channel_opened_and_on_role_registered()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

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

            const fs::path dir = make_lua_hub_dir("evt_chan_role", lua_body, "fixed_rate", 100);
            auto cfg = HubConfig::load_from_directory(dir.string());
            // HEP-CORE-0040 §172 + HEP-CORE-0035 §4.8 — seed only the
            // producer's `role.<uid>` identity; the hub's own
            // `hub_identity` comes from the vault via the production
            // `load_keypair` below (not a pre-seed).  The BRC instantiated
            // below reads the seckey via `keystore_name` and the broker's
            // vault-held known_roles admits the uid via ZAP.  Per-worker
            // RAII; one fixture per subprocess.
            const std::string prod_uid = "prod.l3test.uid12345678";
            auto ks_curve_ = pylabhub::tests::make_curve_setup({prod_uid});
            pylabhub::tests::seed_role_identities(ks_curve_);
            // The hub keypair AND known_roles ride the encrypted vault
            // (HEP-0035 §4.8).  Provision it, then read it back through
            // the production path — no plaintext file, no faked identity.
            pylabhub::tests::provision_hub_vault(cfg, ks_curve_);
            pylabhub::tests::load_hub_keypair_fresh(cfg);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());

            pylabhub::hub::BrokerRequestComm brc;
            pylabhub::hub::BrokerRequestComm::Config bcfg;
            bcfg.broker_endpoint = host.broker_endpoint();
            bcfg.broker_pubkey = host.broker_pubkey();
            bcfg.role_uid = prod_uid;
            bcfg.keystore_name = pylabhub::tests::role_keystore_name(prod_uid);
            ASSERT_TRUE(brc.connect(bcfg));

            std::atomic<bool> running{true};
            std::thread brc_thread([&brc, &running]
                                   { brc.run_poll_loop([&running] { return running.load(); }); });

            auto reg = brc.register_channel(make_reg_opts("lab.evt.channel", prod_uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed — broker rejected the "
                                            "REG_REQ; events never reach the script.";

            ASSERT_TRUE(run_main_loop_with_watchdog(host, std::chrono::seconds{5}))
                << "watchdog expired — neither on_channel_opened nor "
                   "on_role_registered invoked request_shutdown.  "
                   "Chain broker→HubState._on_*→subscriber→"
                   "IncomingMessage→dispatch_event→engine.invoke is "
                   "broken somewhere.  Log:\n"
                << read_log_file(log_cap.log_path());

            running.store(false);
            brc.stop();
            brc_thread.join();
            brc.disconnect();
            host.shutdown();
            flush_logger();

            const std::string log = read_log_file(log_cap.log_path());
            EXPECT_NE(log.find("INIT_RAN"), std::string::npos);
            EXPECT_NE(log.find("CHANNEL_OPENED name=lab.evt.channel"), std::string::npos)
                << "on_channel_opened details wrong; "
                   "channel_to_json regression.\n"
                << log;
            EXPECT_NE(log.find("ROLE_REGISTERED uid=prod.l3test.uid12345678"), std::string::npos)
                << "on_role_registered details wrong; "
                   "role_to_json regression.\n"
                << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::event_observers_channel_registration_"
        "fires_on_channel_opened_and_on_role_registered",
        PLH_LUA_INT_MODS);
}

// ─── Test #8 ────────────────────────────────────────────────────────────────

int event_observer_consumer_registration_fires_on_consumer_added()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

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

            const fs::path dir = make_lua_hub_dir("evt_cons_add", lua_body, "fixed_rate", 100);
            auto cfg = HubConfig::load_from_directory(dir.string());
            // HEP-CORE-0040 §172 + HEP-CORE-0035 §4.8 — seed only the
            // two BRC `role.<uid>` identities; the hub's own hub_identity
            // comes from the vault via the production `load_keypair`
            // below (not a pre-seed).  Per-worker RAII; one fixture per
            // subprocess.
            const std::string prod_uid = "prod.evcons.uid";
            const std::string cons_uid = "cons.l3.uid12345678";
            auto ks_curve_ = pylabhub::tests::make_curve_setup({prod_uid, cons_uid});
            pylabhub::tests::seed_role_identities(ks_curve_);
            // The hub keypair AND known_roles ride the encrypted vault
            // (HEP-0035 §4.8).  Provision it, then read it back through
            // the production path — no plaintext file, no faked identity.
            pylabhub::tests::provision_hub_vault(cfg, ks_curve_);
            pylabhub::tests::load_hub_keypair_fresh(cfg);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());

            pylabhub::hub::BrokerRequestComm prod_brc;
            pylabhub::hub::BrokerRequestComm::Config bcfg;
            bcfg.broker_endpoint = host.broker_endpoint();
            bcfg.broker_pubkey = host.broker_pubkey();
            bcfg.role_uid = prod_uid;
            bcfg.keystore_name = pylabhub::tests::role_keystore_name(prod_uid);
            ASSERT_TRUE(prod_brc.connect(bcfg));
            std::atomic<bool> prod_running{true};
            std::thread prod_thread(
                [&prod_brc, &prod_running]
                { prod_brc.run_poll_loop([&prod_running] { return prod_running.load(); }); });

            auto prod_reg =
                prod_brc.register_channel(make_reg_opts("lab.cons.channel", prod_uid), 3000);
            ASSERT_TRUE(prod_reg.has_value()) << "producer REG_REQ failed";
            // R6 producer-kLive gate — see HEP-CORE-0036 §5.2.
            prod_brc.send_heartbeat("lab.cons.channel", prod_uid, "producer", {});

            pylabhub::hub::BrokerRequestComm cons_brc;
            pylabhub::hub::BrokerRequestComm::Config cbcfg;
            cbcfg.broker_endpoint = host.broker_endpoint();
            cbcfg.broker_pubkey = host.broker_pubkey();
            cbcfg.role_uid = cons_uid;
            cbcfg.keystore_name = pylabhub::tests::role_keystore_name(cons_uid);
            ASSERT_TRUE(cons_brc.connect(cbcfg));
            std::atomic<bool> cons_running{true};
            std::thread cons_thread(
                [&cons_brc, &cons_running]
                { cons_brc.run_poll_loop([&cons_running] { return cons_running.load(); }); });

            auto cons_reg =
                cons_brc.register_consumer(make_cons_opts("lab.cons.channel", cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value()) << "consumer CONSUMER_REG_REQ failed";

            ASSERT_TRUE(run_main_loop_with_watchdog(host, std::chrono::seconds{5}))
                << "watchdog expired — on_consumer_added never "
                   "invoked request_shutdown.\n"
                << read_log_file(log_cap.log_path());

            cons_running.store(false);
            cons_brc.stop();
            cons_thread.join();
            cons_brc.disconnect();
            prod_running.store(false);
            prod_brc.stop();
            prod_thread.join();
            prod_brc.disconnect();
            host.shutdown();
            flush_logger();

            const std::string log = read_log_file(log_cap.log_path());
            EXPECT_NE(log.find("CONSUMER_ADDED ch=lab.cons.channel "
                               "role=cons.l3.uid12345678"),
                      std::string::npos)
                << "on_consumer_added details (channel + role_uid) "
                   "wrong; log:\n"
                << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::event_observer_consumer_registration_"
        "fires_on_consumer_added",
        PLH_LUA_INT_MODS);
}

// ─── Test #9 ────────────────────────────────────────────────────────────────

int post_event_from_on_init_fires_on_app_callback()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

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
            const fs::path dir = make_lua_hub_dir("post_event", lua_body, "fixed_rate", 100);
            auto cfg = HubConfig::load_from_directory(dir.string());
            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());

            ASSERT_TRUE(run_main_loop_with_watchdog(host, std::chrono::seconds{5}))
                << "watchdog expired — on_app_user_event never fired."
                << "  Either api.post_event didn't enqueue, or the "
                   "worker didn't dispatch the app_<name>-prefixed "
                   "event, or the script callback raised an error "
                   "before request_shutdown.\n"
                << read_log_file(log_cap.log_path());

            host.shutdown();
            flush_logger();

            const std::string log = read_log_file(log_cap.log_path());
            EXPECT_NE(log.find("APP_EVENT seq=42 msg=hello"), std::string::npos)
                << "on_app_user_event fired but the args payload was "
                   "wrong — json_to_lua / dispatch_event regression.  "
                   "Log:\n"
                << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::post_event_from_on_init_fires_on_app_callback", PLH_LUA_INT_MODS);
}

// ─── Test #10 ───────────────────────────────────────────────────────────────

int augment_query_metrics_from_admin_thread_call_site_mutates_response()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

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
            const fs::path dir = make_lua_hub_dir("augment_qm", lua_body, "fixed_rate", 100);
            auto cfg = HubConfig::load_from_directory(dir.string());
            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());

            auto *api = host.hub_api();
            ASSERT_NE(api, nullptr) << "hub_api() returned null after startup — EngineHost "
                                       "lazy-construction did not complete";

            json params = {{"x", 7}};
            json response = {{"baseline", "kept-or-mutated"}};
            api->augment_query_metrics(params, response);

            EXPECT_EQ(response.value("augmented_field", std::string{}), "sentinel-99")
                << "augment_query_metrics did NOT mutate response — "
                   "script's return value was either ignored, or "
                   "on_query_metrics never fired.  Response:\n"
                << response.dump(2);
            EXPECT_EQ(response.value("echo_x", -999), 7)
                << "params field 'x' did not round-trip into the "
                   "script's args.params; json_to_lua regression.  "
                   "Response:\n"
                << response.dump(2);
            EXPECT_EQ(response.value("baseline", std::string{}), "kept-or-mutated")
                << "script lost the baseline field — should have "
                   "preserved it via args.response.";

            host.shutdown();
            flush_logger();

            const std::string log = read_log_file(log_cap.log_path());
            EXPECT_NE(log.find("AUGMENT_RAN params=7"), std::string::npos)
                << "on_query_metrics fired but params didn't carry "
                   "x=7; log:\n"
                << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::augment_query_metrics_from_admin_thread_"
        "call_site_mutates_response",
        PLH_LUA_INT_MODS);
}

// ─── Test #11 ───────────────────────────────────────────────────────────────

int augment_null_return_keeps_default_response()
{
    return run_gtest_worker(
        []
        {
            LogCaptureFixture log_cap;
            log_cap.Install();
            pylabhub::scripting::init_scripting();

            // Script returns nil — the C++ side must KEEP the default
            // response unchanged.  Pins the §12.2.2 anti-pattern:
            // mutate-without-return is silently treated as "no change."
            const std::string lua_body = R"LUA(
function on_init(api) api.log('info', 'INIT_RAN') end
function on_query_metrics(args)
    api.log('info', 'AUGMENT_RAN_NIL_RETURN')
    args.response.would_have_been = 'lost'
    -- intentionally no `return` -> nil
end
)LUA";
            const fs::path dir = make_lua_hub_dir("augment_nil", lua_body, "fixed_rate", 100);
            auto cfg = HubConfig::load_from_directory(dir.string());
            // BRC-less path — see `write_known_roles` doc-block above
            // for the contract.  This test only exercises in-process
            // HubHost + script lifecycle; no client ever opens a BRC,
            // so empty `{}` is correct and `known_roles.json` is not
            // written.  Per-worker RAII; one fixture per subprocess.
            auto ks_curve_ = pylabhub::tests::make_curve_setup({});
            pylabhub::tests::seed_curve_identities(ks_curve_);

            HubHost host(std::move(cfg));
            ASSERT_NO_THROW(host.startup());

            auto *api = host.hub_api();
            ASSERT_NE(api, nullptr);

            json params = json::object();
            json response = {{"default", true}};
            api->augment_query_metrics(params, response);

            EXPECT_EQ(response.value("default", false), true)
                << "default response was lost despite nil-return "
                   "contract.  Response:\n"
                << response.dump(2);
            EXPECT_EQ(response.contains("would_have_been"), false)
                << "in-place mutation leaked into the response "
                   "despite the script returning nil — contract "
                   "violation: only `return` should publish changes.  "
                   "Response:\n"
                << response.dump(2);

            host.shutdown();
            flush_logger();

            const std::string log = read_log_file(log_cap.log_path());
            EXPECT_NE(log.find("AUGMENT_RAN_NIL_RETURN"), std::string::npos)
                << "on_query_metrics never fired — nil-return contract "
                   "test is vacuous if the callback didn't run.  Log:\n"
                << log;

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
            remove_tree(dir);
        },
        "hub_lua_integration::augment_null_return_keeps_default_response", PLH_LUA_INT_MODS);
}

} // namespace hub_lua_integration
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct HubLuaIntegrationRegistrar
{
    HubLuaIntegrationRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "hub_lua_integration")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_lua_integration;

                if (sc == "real_lua_script_on_init_on_stop_fire_and_log")
                    return real_lua_script_on_init_on_stop_fire_and_log();
                if (sc == "real_lua_script_admin_console_print")
                    return real_lua_script_admin_console_print();
                if (sc == "script_syntax_error_startup_throws")
                    return script_syntax_error_startup_throws();
                if (sc == "on_tick_fires_periodically_when_idle")
                    return on_tick_fires_periodically_when_idle();
                if (sc == "on_tick_catch_up_fixed_rate_with_compensation")
                    return on_tick_catch_up_fixed_rate_with_compensation();
                if (sc == "read_accessors_all_reachable_from_on_init")
                    return read_accessors_all_reachable_from_on_init();
                if (sc == "request_shutdown_from_on_init_wakes_main_loop")
                    return request_shutdown_from_on_init_wakes_main_loop();
                if (sc == "event_observers_channel_registration_fires_"
                          "on_channel_opened_and_on_role_registered")
                    return event_observers_channel_registration_fires_on_channel_opened_and_on_role_registered();
                if (sc == "event_observer_consumer_registration_fires_"
                          "on_consumer_added")
                    return event_observer_consumer_registration_fires_on_consumer_added();
                if (sc == "post_event_from_on_init_fires_on_app_callback")
                    return post_event_from_on_init_fires_on_app_callback();
                if (sc == "augment_query_metrics_from_admin_thread_"
                          "call_site_mutates_response")
                    return augment_query_metrics_from_admin_thread_call_site_mutates_response();
                if (sc == "augment_null_return_keeps_default_response")
                    return augment_null_return_keeps_default_response();
                return -1;
            });
    }
} g_registrar;

} // namespace
