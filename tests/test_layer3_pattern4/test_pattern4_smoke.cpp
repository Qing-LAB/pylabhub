/**
 * @file test_pattern4_smoke.cpp
 * @brief Pattern 4 reference smoke test (task #220).
 *
 * Verifies the Pattern 4 infrastructure end-to-end with the smallest
 * useful scenario:
 *
 *   - parent picks an unused TCP port and writes setup.json under a
 *     per-test temp dir;
 *   - parent spawns a broker subprocess and a role subprocess
 *     (Pattern-3 workers running in separate OS processes);
 *   - broker subprocess binds CTRL ROUTER with CURVE + ZAP and logs
 *     the bound endpoint;
 *   - role subprocess constructs a BrokerRequestComm, performs the
 *     CURVE handshake against the broker, connects, and logs success;
 *   - parent live-polls each subprocess's captured stderr for the
 *     expected log markers within canonical timeouts;
 *   - both subprocesses self-time-out and exit cleanly.
 *
 * What this test does NOT yet exercise:
 *
 *   - REG_REQ / REG_ACK handshake (deferred — adds wire-protocol
 *     surface that's worth its own follow-up test once the bootstrap
 *     path is green);
 *   - Heartbeat installation post-REG_ACK;
 *   - Back-channel pipe quit signal (subprocesses use self-timeout —
 *     follow-up adds `WorkerProcess::with_quit_signal`).
 *
 * See `docs/README/README_testing.md` § Pattern 4 for the design
 * pattern this test exemplifies.
 */
#include "pattern4_helpers.h"

#include "shared_test_helpers.h"
#include "test_patterns.h"

#include "utils/timeout_constants.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::make_temp_dir;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4SmokeTest : public IsolatedProcessTest
{
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

    fs::path make_test_temp_dir(std::string_view label)
    {
        auto dir = make_temp_dir(label);
        paths_to_clean_.push_back(dir);
        return dir;
    }

    std::vector<fs::path> paths_to_clean_;
};

// ─── Smoke test: broker binds + role connects via CURVE ────────────────────

TEST_F(Pattern4SmokeTest, BrokerBindsAndRoleConnects)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    // ── 1. Parent: per-test temp dir + setup bundle on disk ──
    const fs::path temp_dir = make_test_temp_dir("broker_binds_and_role_connects");
    const auto setup        = make_pattern4_setup({"role.x"});
    write_pattern4_setup(setup, temp_dir / "setup.json");

    // ── 2. Spawn subprocesses with quit-signal pipes ──
    //
    // Both broker + role wait on the quit-signal pipe instead of a
    // fixed self-timeout, so the test exits as soon as the parent has
    // observed everything it needs.  See README_testing.md § "Pattern 4
    // — Termination via quit-signal pipe" for the canonical pattern.
    auto broker = SpawnWorkerWithQuitSignal("pattern4_smoke.broker",
                                            {temp_dir.string()});
    auto role   = SpawnWorkerWithQuitSignal("pattern4_smoke.role_x",
                                            {temp_dir.string()});

    // ── 3. Expected sequence — broker binds, role connects ──
    //
    // Timeouts come from canonical lib constants per Pattern 4 doc.
    // The smoke test path is fast (no REG_REQ); kMidTimeoutMs covers
    // subprocess startup + LifecycleGuard init + CURVE handshake on
    // a busy CI worker.
    expect_log(broker, "Pattern4Broker: bound endpoint",
               std::chrono::milliseconds{kMidTimeoutMs});
    expect_log(role,   "Pattern4Role[role.x]: BRC connected",
               std::chrono::milliseconds{kLongTimeoutMs});

    // ── 4. Parent-driven termination via quit-signal pipes ──
    //
    // Both subprocesses are blocked in wait_for_quit_or_safety_timeout.
    // signal_quit() closes the parent's write end of each pipe; the
    // worker's read() returns EOF and proceeds to clean exit.  Test
    // wall time is dominated by subprocess shutdown (Logger flush +
    // LifecycleGuard finalize), not by an arbitrary self-timeout.
    broker.signal_quit();
    role.signal_quit();

    // ExpectWorkerOk waits for the subprocess to exit, then checks
    // exit code 0 + the three completion markers (BEGIN / END_OK /
    // FINALIZED) per Pattern 3.
    ExpectWorkerOk(broker);
    ExpectWorkerOk(role);
}

} // namespace
