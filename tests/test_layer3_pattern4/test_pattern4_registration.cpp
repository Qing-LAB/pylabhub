/**
 * @file test_pattern4_registration.cpp
 * @brief Pattern 4 rung 2 — Pattern4RegistrationTest (task #221).
 *
 * Verifies the producer registration contract end-to-end across a
 * broker subprocess + producer-role subprocess:
 *
 *   1. Parent picks an unused TCP port + shared-log path, writes
 *      setup.json under a per-test temp dir;
 *   2. Spawns broker + producer-role subprocesses; each redirects its
 *      Logger sink to the shared log via set_shared_log();
 *   3. Producer-role constructs RoleAPIBase + RoleHandler + sends
 *      REG_REQ via register_producer_channel;
 *   4. Broker accepts REG_REQ + sends REG_ACK with initial_allowlist;
 *   5. Producer-role's Presence FSM transitions
 *      Unregistered → RegRequestPending → Registered;
 *   6. Parent pins the 5-marker sequence across both subprocesses via
 *      expect_log_sequence on the shared log (file-position IS
 *      time-order under O_APPEND — no timestamp parsing needed).
 *
 * Production INFO markers exercised:
 *   - role:   "presence channel='reg.test' state Unregistered->RegRequestPending"
 *   - broker: "Broker: REG_REQ accepted role='<uid>' channel='reg.test' producer_pubkey='...'"
 *   - broker: "Broker: REG_ACK sending channel='reg.test' ..."
 *   - role:   "REG_ACK received channel='reg.test' status=success initial_allowlist=[]"
 *   - role:   "presence channel='reg.test' state RegRequestPending->Registered"
 *
 * The role uid is constructed via `pylabhub::scripting::make_role_uid`
 * — single source of truth for HEP-CORE-0033 §G2.2.0b grammar.
 *
 * See docs/README/README_testing.md § "Pattern 4 — ... — Test ladder"
 * rung 2 for the contract this test pins.
 */
#include "pattern4_helpers.h"

#include "shared_test_helpers.h"
#include "test_patterns.h"

#include "utils/role_uid.hpp"
#include "utils/timeout_constants.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::tests::pattern4::expect_log_sequence;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::make_temp_dir;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4RegistrationTest : public IsolatedProcessTest
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

// ─── Rung 2: producer REG_REQ + REG_ACK + Presence FSM transitions ─────────

TEST_F(Pattern4RegistrationTest, ProducerRegistersAndStateAdvances)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    // ── 1. Parent: per-test temp dir + setup bundle (incl. shared log) ──
    //
    // The role_uid here MUST match the one the producer-role worker
    // composes (same RoleUidTag + name + instance) — both go through
    // `make_role_uid` so any grammar evolution updates both sites.
    const std::string role_uid = pylabhub::scripting::make_role_uid(
        pylabhub::scripting::RoleUidTag::Producer, "pattern4reg", 1u);
    const fs::path temp_dir = make_test_temp_dir("producer_registers");
    auto setup = make_pattern4_setup({role_uid});
    setup.shared_log_path = (temp_dir / "shared.log").string();
    write_pattern4_setup(setup, temp_dir / "setup.json");

    const fs::path shared_log = setup.shared_log_path;

    // ── 2. Spawn broker FIRST and wait for it to bind ──
    //
    // Without this gate, the role's BRC poll thread can start
    // microseconds before the broker has bound + installed its ZAP
    // handler.  Symptom: REG_REQ races the bind window and the
    // CURVE handshake silently drops, REG_REQ times out at 5s,
    // test fails ~2/3 runs.  Per memory feedback_no_flake_explanations
    // this is NOT a flake — it's a test design race, fixed here by
    // making subprocess ordering deterministic on the shared log.
    //
    // Broker is spawned with `with_quit_signal=true` so the parent
    // can tell it to exit AS SOON as the assertion sequence passes,
    // rather than waiting for a fixed self-timeout.  See
    // README_testing.md § "Pattern 4 — Termination via quit-signal
    // pipe" for the canonical pattern.
    auto broker = SpawnWorkerWithQuitSignal("pattern4_registration.broker", {temp_dir.string()});
    expect_log_sequence(shared_log, {"Pattern4Broker: bound endpoint='"},
                        milliseconds{kMidTimeoutMs});
    // Broker is now provably bound + ZAP-installed.  Safe to spawn role.
    auto role = SpawnWorker("pattern4_registration.producer_role", {temp_dir.string()});

    // ── 3. Pin the 5-marker sequence (cross-process, in order) ──
    //
    // Each marker substring is uniquely-grep-able and stable across
    // the marker contract changes downstream rungs may negotiate.
    // Per "Logging discipline" all markers are one-shot INFO emitted
    // from production code (role_api_base.cpp + broker_service.cpp).
    //
    // kLongTimeoutMs accommodates worst-case CURVE handshake +
    // handler-thread startup on a busy -j 2 CI box; the happy path
    // typically completes well under kShortTimeoutMs.
    expect_log_sequence(
        shared_log,
        {
            "event=PresenceStateTransition channel='reg.test' role_type=producer from=Unregistered "
            "to=RegRequestPending",
            fmt::format("event=RegReqAccepted role='{}' "
                        "channel='reg.test' producer_pubkey='",
                        role_uid),
            "event=RegAckSending channel='reg.test'",
            "event=RegAckReceived channel='reg.test' status=success initial_allowlist=",
            "event=PresenceStateTransition channel='reg.test' role_type=producer "
            "from=RegRequestPending to=Registered",
        },
        milliseconds{kLongTimeoutMs});

    // ── 4. Termination: role exits on its own; broker quits via pipe ──
    //
    // Role: did its work (REG_REQ + REG_ACK + state transitions) and
    // returns from its lambda; wait_for_exit() returns within a few
    // hundred ms (Logger flush + LifecycleGuard finalize).
    //
    // Broker: still blocked in wait_for_quit_or_safety_timeout() inside
    // its worker lambda.  `broker.signal_quit()` closes the parent's
    // write end of the quit pipe; the broker's read() returns EOF and
    // it proceeds to broker->stop() + clean exit.  No self-timeout
    // wasted — the test takes only as long as the work plus a small
    // shutdown delay (~1-2 s typical).
    role.wait_for_exit();
    broker.signal_quit();
    broker.wait_for_exit();

    // [WORKER_BEGIN]/[WORKER_END_OK]/[WORKER_FINALIZED] markers stay on
    // per-subprocess stderr (fmt::print, not Logger sink), so
    // ExpectWorkerOk's verification path is unchanged from rung 1.
    ExpectWorkerOk(broker);
    ExpectWorkerOk(role);
}

} // namespace
