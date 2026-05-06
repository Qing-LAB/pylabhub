/**
 * @file test_plh_hub_runmode.cpp
 * @brief plh_hub run-mode lifecycle tests (HUB_TEST_COVERAGE_PLAN slice 5).
 *
 * Spawns `plh_hub <hub_dir>` as a real subprocess, waits for it to
 * reach the broker-bound-and-listening state (verified via the log
 * file's "Broker: listening on …" marker, which `BrokerService::run`
 * emits exactly once after `bind` succeeds — broker_service.cpp:458),
 * sends SIGTERM, and verifies clean exit.
 *
 * Test rigor (per CLAUDE.md "tests must pin path, timing, and
 * structure" + "validate from solid output"):
 *
 *   - Wait condition is OUTPUT-based, not timing-based: poll the log
 *     file for the "Broker: listening" marker with a generous timeout
 *     (5s).  Regression that fails to bind times out cleanly; we
 *     never sleep "long enough and hope."
 *   - SIGTERM is the production shutdown signal — `plh_hub` installs
 *     `InteractiveSignalHandler` that flips a shutdown atomic; the
 *     bridge thread translates that to `host.request_shutdown()`,
 *     which wakes `run_main_loop()` and runs the §4.2 shutdown
 *     sequence.
 *   - Class D gate: stderr must NOT contain `[ERROR ]` after
 *     a clean SIGTERM-driven exit; an unexpected error during
 *     shutdown surfaces here.
 */

#include "plh_hub_fixture.h"

#include <chrono>
#include <csignal>
#include <fstream>
#include <thread>

using namespace pylabhub::tests::plh_hub_l4;
using pylabhub::tests::helper::WorkerProcess;

namespace
{

/// Poll the directory for the timestamped hub log file and return
/// its content.  Logging is configured with `timestamped: true` in
/// the init template, so the file is named
/// `<hub_dir>/logs/<uid>-YYYY-MM-DD-HH-MM-SS.uuuuuu.log`.  Returns
/// empty string if no log file exists yet.
std::string read_hub_log(const fs::path &hub_dir)
{
    const fs::path logs = hub_dir / "logs";
    std::error_code ec;
    if (!fs::is_directory(logs, ec))
        return {};

    // Pick the most recent .log file (timestamped names sort
    // lexicographically = chronologically).
    fs::path newest;
    for (const auto &e : fs::directory_iterator(logs, ec))
    {
        if (e.path().extension() == ".log" && e.path() > newest)
            newest = e.path();
    }
    if (newest.empty()) return {};

    std::ifstream f(newest);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>{});
    return content;
}

/// Poll for @p marker in the hub's log file with a hard ceiling of
/// @p timeout.  Returns true once seen; false on timeout.  Timing is
/// the watchdog ceiling, NOT the success criterion — a successful
/// startup sees the marker in <1s on dev hardware; the 5s ceiling
/// is for CI/sanitizer headroom.
bool wait_for_log_marker(const fs::path &hub_dir,
                          const std::string &marker,
                          std::chrono::milliseconds timeout =
                              std::chrono::milliseconds(5000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const std::string log = read_hub_log(hub_dir);
        if (log.find(marker) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// Configure an init'd hub directory for run-mode L4 testing.  Patches
/// the generated hub.json to make this a no-script / no-admin /
/// no-CURVE run — the test focuses on the broker run-mode path; admin
/// over-the-wire gets its own slice.
void configure_for_runmode(const fs::path &dir)
{
    nlohmann::json j;
    {
        std::ifstream f(dir / "hub.json");
        f >> j;
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";  // ephemeral
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";                   // no script
    j["hub"]["auth"]["keyfile"]     = "";                   // no CURVE
    std::ofstream f(dir / "hub.json");
    f << j.dump(2);
}

} // namespace

// ─── Tests ─────────────────────────────────────────────────────────────────

/// Baseline run-mode contract: plh_hub starts, broker binds, SIGTERM
/// causes orderly shutdown with exit 0.  The watchdog bounds prove
/// the contract via OBSERVABLE markers (broker-listening log line,
/// process exit), not via timing alone.
TEST_F(PlhHubCliTest, RunMode_StartsBindsAcceptsSigtermExitsZero)
{
    const fs::path dir = tmp("runmode_basic");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {dir.string(), "--name", "L4RuntimeHubBasic"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    configure_for_runmode(dir);

    // Spawn plh_hub <dir> as a long-running subprocess.  Don't wait on
    // exit yet — the binary blocks in run_main_loop until SIGTERM.
    WorkerProcess hub(plh_hub_binary(), dir.string(), {});

    // Wait for the broker-listening marker.  This is the SOLID-OUTPUT
    // proof that startup reached the run-loop; if startup fails (port
    // collision, config error, vault mismatch) the marker never
    // appears and the watchdog times out cleanly.
    ASSERT_TRUE(wait_for_log_marker(dir, "Broker: listening on"))
        << "plh_hub never reached broker-bind state.  Log:\n"
        << read_hub_log(dir);

    // Send SIGTERM.  plh_hub's signal handler flips g_shutdown; bridge
    // thread translates to host.request_shutdown(); run_main_loop wakes;
    // §4.2 shutdown sequence runs (admin drain → runner → broker stop
    // → thread_mgr drain).
    hub.send_signal(SIGTERM);

    // Bounded join.  Clean shutdown completes in <1s on dev hardware;
    // 10s ceiling is generous CI headroom.  Timeout = regression in
    // signal handling or shutdown ordering.
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = hub.wait_for_exit(/*timeout_s=*/10);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(rc, 0) << "plh_hub did not exit cleanly on SIGTERM (rc=" << rc
                     << ").  stderr:\n" << hub.get_stderr();
    EXPECT_LT(elapsed_ms, 10000)
        << "shutdown took " << elapsed_ms << " ms — bounded join timed out.  "
           "Regression in HEP-CORE-0033 §4.2 shutdown sequence.";

    // Class D gate: no stray ERROR-level logs.  An ERROR during
    // shutdown indicates a teardown bug (e.g., a subsystem failed
    // to drain) even when the exit code is 0.
    const std::string log = read_hub_log(dir);
    const std::string err = hub.get_stderr();
    auto contains_error = [](const std::string &haystack)
    {
        return haystack.find("[ERROR ]") != std::string::npos;
    };
    EXPECT_FALSE(contains_error(log))
        << "plh_hub log file contains [ERROR ] lines after clean SIGTERM exit:\n"
        << log;
    EXPECT_FALSE(contains_error(err))
        << "plh_hub stderr contains [ERROR ] lines after clean SIGTERM exit:\n"
        << err;
}

/// Verifies the orderly shutdown sequence by content:
///   - "Broker: listening on …" appears AFTER any pre-broker
///     module-init lines (proves init ordering).
///   - The plh_hub log shows the runner+admin drain step before
///     the broker stop step (proves §4.2 ordering).
TEST_F(PlhHubCliTest, RunMode_LogShowsCorrectStartupAndShutdownOrdering)
{
    const fs::path dir = tmp("runmode_ordering");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {dir.string(), "--name", "L4RuntimeHubOrdering"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    configure_for_runmode(dir);

    WorkerProcess hub(plh_hub_binary(), dir.string(), {});

    ASSERT_TRUE(wait_for_log_marker(dir, "Broker: listening on"))
        << "broker never bound. Log:\n" << read_hub_log(dir);

    hub.send_signal(SIGTERM);
    EXPECT_EQ(hub.wait_for_exit(10), 0);

    const std::string log = read_hub_log(dir);

    // The configured-logger sink only captures `LOGGER_*` lines (not
    // the `[DBG] [PLH_LifeCycle] ...` stderr-only LifecycleGuard
    // messages, which use a different channel).  So we pin ordering
    // via markers that live in the configured sink:
    //
    //   Startup chain:
    //     "spawned thread 'broker'"     — ThreadManager::spawn (HubHost.cpp)
    //     "Broker: listening on …"      — BrokerServiceImpl::run after bind
    //     "[HubHost:<uid>] startup complete (broker on …)"
    //   Shutdown chain (after SIGTERM):
    //     "[HubHost:<uid>] main loop woke for shutdown"
    //     "[HubHost:<uid>] shutdown initiated"
    //     "Broker: stopped."            — BrokerServiceImpl::run exits
    //     "[HubHost:<uid>] shutdown complete"

    // Startup: the broker's listening line MUST come AFTER the
    // ThreadManager spawned it and BEFORE startup-complete.
    const auto pos_spawn      = log.find("spawned thread 'broker'");
    const auto pos_broker_up  = log.find("Broker: listening on");
    const auto pos_startup_ok = log.find("startup complete (broker on");
    ASSERT_NE(pos_spawn,      std::string::npos) << log;
    ASSERT_NE(pos_broker_up,  std::string::npos) << log;
    ASSERT_NE(pos_startup_ok, std::string::npos) << log;
    EXPECT_LT(pos_spawn,      pos_broker_up);
    EXPECT_LT(pos_broker_up,  pos_startup_ok);

    // Shutdown: HEP-0033 §4.2 step ordering — main loop wakes,
    // shutdown initiated, broker stops, shutdown complete.
    const auto pos_wake     = log.find("main loop woke for shutdown");
    const auto pos_init     = log.find("] shutdown initiated");
    const auto pos_stopped  = log.find("Broker: stopped");
    const auto pos_complete = log.find("] shutdown complete");
    ASSERT_NE(pos_wake,     std::string::npos) << log;
    ASSERT_NE(pos_init,     std::string::npos) << log;
    ASSERT_NE(pos_stopped,  std::string::npos) << log;
    ASSERT_NE(pos_complete, std::string::npos) << log;
    EXPECT_LT(pos_startup_ok, pos_wake)
        << "shutdown signaled BEFORE startup completed — race.";
    EXPECT_LT(pos_wake,    pos_init);
    EXPECT_LT(pos_init,    pos_stopped);
    EXPECT_LT(pos_stopped, pos_complete);
}

/// Two SIGTERMs in quick succession must NOT cause a double-shutdown
/// hang or duplicate teardown.  HubHost::shutdown is documented
/// idempotent (HEP-0033 §4.2); this test pins that.
TEST_F(PlhHubCliTest, RunMode_DoubleSigtermIsIdempotent)
{
    const fs::path dir = tmp("runmode_double_sig");
    {
        WorkerProcess init(plh_hub_binary(), "--init",
            {dir.string(), "--name", "L4RuntimeHubDouble"});
        ASSERT_EQ(init.wait_for_exit(), 0) << init.get_stderr();
    }
    configure_for_runmode(dir);

    WorkerProcess hub(plh_hub_binary(), dir.string(), {});
    ASSERT_TRUE(wait_for_log_marker(dir, "Broker: listening on"));

    hub.send_signal(SIGTERM);
    // Small wait so the second SIGTERM lands during shutdown, not
    // before it starts.  Bounded; the assertion below is on EXIT
    // CODE + LOG content, not timing.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    hub.send_signal(SIGTERM);

    EXPECT_EQ(hub.wait_for_exit(10), 0)
        << "double SIGTERM caused non-zero exit.  stderr:\n" << hub.get_stderr();
    const std::string log = read_hub_log(dir);
    EXPECT_EQ(log.find("[ERROR ]"), std::string::npos)
        << "double SIGTERM produced ERROR-level log lines.  Log:\n" << log;
}
