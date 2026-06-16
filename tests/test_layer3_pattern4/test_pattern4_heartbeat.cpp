/**
 * @file test_pattern4_heartbeat.cpp
 * @brief Pattern 4 rung 3 — Pattern4HeartbeatTest (task #223).
 *
 * Pins HEP-CORE-0023 §2.5 heartbeat contract end-to-end across a
 * broker subprocess + producer-role subprocess, on FOUR distinct
 * axes (sequence + timing + payload + state):
 *
 *   1. Negotiation: the hub-authoritative `heartbeat_interval_ms`
 *      delivered in REG_ACK is honored by the role.  Pin: parse the
 *      value from broker's "REG_ACK sending ... heartbeat_interval_ms=N"
 *      line + role's "heartbeat: periodic tick installed at Mms" and
 *      assert M ≤ N (role respects hub ceiling).
 *
 *   2. First tick: broker's `Broker: first heartbeat received from
 *      role='...'` fires exactly once per presence per session.  Pin
 *      its presence in the sequence.
 *
 *   3. Cadence × steady-state: both role and broker emit one-shot
 *      shutdown-summary `heartbeat counter: ...` lines naming the
 *      observed count.  Parent extracts the numbers and asserts they
 *      land inside `expected_count ± 30%` for the measurement window
 *      driven by the parent's sleep + quit-signal pipe.
 *
 *   4. Symmetry: role-side `sent` count is within 30% of broker-side
 *      `received` count.  Drop on either side fails this.
 *
 * Mutation discipline: disable `install_heartbeat` in production lib
 * → counters stay 0, axes 3 + 4 fail with the actual counts in the
 * diagnostic.  Change role to ignore hub's negotiated interval →
 * axis 1 fails.  Drop alternate ticks broker-side → received count
 * ≈ half of sent → axis 4 fails.
 *
 * Cross-references:
 *   - HEP-CORE-0023 §2.5 (cadence + timeout-multiplier)
 *   - HEP-CORE-0023 §2.5.1 (role-preferred-vs-hub-authority)
 *   - HEP-CORE-0023 §2.5.2 (per-presence heartbeat contract)
 *   - docs/README/README_testing.md § Pattern 4 — Test ladder rung 3
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
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::tests::pattern4::expect_log_sequence;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::make_temp_dir;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4HeartbeatTest : public IsolatedProcessTest
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

// Read full shared log into memory.  Test scale (<100 KB) — fine.
std::string read_shared_log(const fs::path &shared_log)
{
    std::ifstream in(shared_log, std::ios::in | std::ios::binary);
    if (!in) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Find the first line that contains `anchor`, then extract the
// integer that follows `field`.  Returns -1 on miss; caller's
// ASSERT_GE rejects.
[[nodiscard]] int extract_int_after(const std::string &text,
                                     std::string_view anchor,
                                     std::string_view field)
{
    const auto anchor_pos = text.find(anchor);
    if (anchor_pos == std::string::npos) return -1;
    // Find the end of the line containing the anchor — extraction
    // must stay within that single line so the marker contract is
    // unambiguous.
    const auto line_end = text.find('\n', anchor_pos);
    const auto field_pos = text.find(field, anchor_pos);
    if (field_pos == std::string::npos ||
        (line_end != std::string::npos && field_pos > line_end))
        return -1;
    const auto digits_start = field_pos + field.size();
    std::size_t i = digits_start;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
        ++i;
    if (i == digits_start) return -1;
    try { return std::stoi(text.substr(digits_start, i - digits_start)); }
    catch (...) { return -1; }
}

// Same as `extract_int_after`, but anchors on the LAST occurrence of
// the marker rather than the first.  Used for the broker's periodic
// snapshot lines — we want the most recent count, not the first.
[[nodiscard]] int extract_int_after_last(const std::string &text,
                                          std::string_view anchor,
                                          std::string_view field)
{
    const auto anchor_pos = text.rfind(anchor);
    if (anchor_pos == std::string::npos) return -1;
    const auto line_end = text.find('\n', anchor_pos);
    const auto field_pos = text.find(field, anchor_pos);
    if (field_pos == std::string::npos ||
        (line_end != std::string::npos && field_pos > line_end))
        return -1;
    const auto digits_start = field_pos + field.size();
    std::size_t i = digits_start;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
        ++i;
    if (i == digits_start) return -1;
    try { return std::stoi(text.substr(digits_start, i - digits_start)); }
    catch (...) { return -1; }
}

// ─── Rung 3: heartbeat cadence + first-tick + counters + symmetry ──────────

TEST_F(Pattern4HeartbeatTest, CadenceNegotiatedAndSteadyState)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    // The role's preferred cadence intentionally exceeds the typical
    // hub-default of 500 ms — that forces the role's "aligned with hub"
    // branch into the DOWNGRADE path (matches the worker's constant),
    // exercising the negotiation axis 1.
    constexpr int role_cfg_ms = 1000;

    // ── 1. Parent: temp dir + setup (with shared log) ──
    //
    // The role drives its own measurement window internally and exits
    // naturally; only the broker uses the quit-signal pipe (because
    // the broker doesn't know when the role is done).  The test
    // doesn't try to control elapsed time precisely — it reads the
    // actual elapsed time from the role's shutdown-summary line and
    // computes the observed rate from there.  This makes the rung
    // robust to CI scheduling jitter.
    const std::string role_uid = pylabhub::scripting::make_role_uid(
        pylabhub::scripting::RoleUidTag::Producer, "pattern4hb", 1u);
    const fs::path temp_dir = make_test_temp_dir("cadence_negotiated_and_steady");
    auto           setup    = make_pattern4_setup({role_uid});
    setup.shared_log_path   = (temp_dir / "shared.log").string();
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const fs::path shared_log = setup.shared_log_path;

    // ── 2. Spawn broker FIRST + wait for bind ──
    // Broker subprocess decides when to exit on its own (detects
    // "role done" by observing the heartbeat counter stop growing) —
    // no quit-signal pipe needed.
    auto broker = SpawnWorker("pattern4_heartbeat.broker",
                              {temp_dir.string()});
    expect_log_sequence(
        shared_log,
        {"Pattern4Broker: bound endpoint='"},
        milliseconds{kMidTimeoutMs});
    auto role = SpawnWorker("pattern4_heartbeat.producer_role",
                            {temp_dir.string()});

    // ── 3. Pin sequence up through first heartbeat received ──
    //
    // The 5 first-rung-2 markers + 3 new rung-3 markers in order.  The
    // role's "heartbeat: aligned with hub" line is the negotiation
    // observable; "periodic tick installed" is the install
    // observable; broker's "first heartbeat received" is the first
    // arrival observable.  expect_log_sequence sorts the log by
    // embedded timestamp before searching, so cross-process write
    // interleaving on the shared O_APPEND log cannot flip causal order.
    expect_log_sequence(
        shared_log,
        {
            "presence channel='hb.test' state Unregistered->RegRequestPending",
            fmt::format("Broker: REG_REQ accepted role='{}' channel='hb.test'",
                        role_uid),
            "Broker: REG_ACK sending channel='hb.test' heartbeat_interval_ms=",
            "REG_ACK received channel='hb.test' status=success",
            "presence channel='hb.test' state RegRequestPending->Registered",
            // role_cfg (1000ms) > hub_max (500ms) so the role logs
            // the DOWNGRADE-to-hub-max WARN, not the "aligned with
            // hub" INFO.  This IS axis-1 negotiation: hub authority
            // wins, role's preference is overridden.
            "heartbeat: configured interval 1000 ms exceeds hub's tolerated max",
            "heartbeat: periodic tick installed at ",
            "Broker: first heartbeat received from role='",
        },
        milliseconds{kLongTimeoutMs});

    // ── 4. Wait for role to finish naturally + signal broker to stop ──
    //
    // The role drives its own measurement window and exits when done
    // (logs its sent-count summary as part of `stop_handler_threads`).
    // The broker subprocess continuously emits "counter snapshot" log
    // lines while alive — we read the LATEST one for the broker-side
    // count.  Then we signal broker to stop (the snapshot log already
    // has what we need; broker shutdown timing no longer matters).
    role.wait_for_exit();

    // ── 5. Wait for the role's shutdown summary + at least one broker
    //       snapshot line containing received > 0 ──
    expect_log_sequence(
        shared_log,
        {
            "heartbeat counter: sent=",                  // role side
            "Pattern4Broker: counter snapshot ",         // broker side, any tick
        },
        milliseconds{kMidTimeoutMs});

    broker.wait_for_exit();

    // ── 6. Parse + assert all four axes ──
    const std::string log = read_shared_log(shared_log);
    ASSERT_FALSE(log.empty()) << "shared log unreadable";

    // Axis 1 — Negotiation: install_interval ≤ ack_interval.  Role cfg
    // is 1000 ms; hub's typical default is 500 ms; expect install at 500.
    const int ack_interval =
        extract_int_after(log, "Broker: REG_ACK sending channel='hb.test'",
                          "heartbeat_interval_ms=");
    const int install_interval =
        extract_int_after(log, "heartbeat: periodic tick installed at ",
                          "at ");
    ASSERT_GT(ack_interval, 0)
        << "broker REG_ACK didn't include heartbeat_interval_ms";
    ASSERT_GT(install_interval, 0)
        << "role didn't log heartbeat install marker";
    EXPECT_LE(install_interval, ack_interval)
        << "axis 1 negotiation: role's effective cadence ("
        << install_interval << " ms) exceeds hub's tolerated max ("
        << ack_interval
        << " ms) — HEP-CORE-0023 §2.5.1 violated.  Role role_cfg_ms="
        << role_cfg_ms;

    // Axis 3 — Cadence × steady-state (rate-based).
    //
    // The role's shutdown line logs BOTH `sent=N` AND `over Mms` so we
    // compute the OBSERVED RATE = N / (M/1000) Hz directly, then
    // compare to the EXPECTED RATE = 1000 / install_interval.  This
    // makes the assertion independent of whether the role measured for
    // 2s or for the safety-timeout — what matters is the rate.
    //
    // Tolerance band: ±30% locally, ±50% on CI (per PYLABHUB_CI_BUILD).
    // CI scheduling under -j 2 can stretch a 500 ms periodic tick to
    // 600-700 ms occasionally.  The tighter local band is the
    // mutation-discipline floor.
    const int sent_count =
        extract_int_after(log, "heartbeat counter: sent=", "sent=");
    const int elapsed_ms =
        extract_int_after(log, "heartbeat counter: sent=", "over ");
    // For the broker side, take the LAST snapshot line — the count is
    // monotone non-decreasing, so the latest is the most accurate.
    const int received_count =
        extract_int_after_last(log, "Pattern4Broker: counter snapshot ",
                               "received=");
    ASSERT_GE(sent_count, 0)
        << "role didn't log shutdown counter — install_heartbeat failed?";
    ASSERT_GT(elapsed_ms, 0)
        << "role didn't log elapsed time alongside sent count";
    ASSERT_GE(received_count, 0)
        << "broker didn't emit periodic snapshot — broker worker bug?";

#ifdef PYLABHUB_CI_BUILD
    constexpr double kRateTolerance = 0.50;  // CI -j 2 scheduling
#else
    constexpr double kRateTolerance = 0.30;  // local mutation-discipline floor
#endif

    const double observed_rate = sent_count * 1000.0 / elapsed_ms;
    const double expected_rate = 1000.0 / install_interval;
    EXPECT_GE(observed_rate, expected_rate * (1.0 - kRateTolerance))
        << "axis 3 cadence: observed " << observed_rate << " Hz from sent="
        << sent_count << " over " << elapsed_ms
        << " ms is below expected " << expected_rate
        << " Hz (tolerance " << (kRateTolerance * 100) << "%)";
    EXPECT_LE(observed_rate, expected_rate * (1.0 + kRateTolerance))
        << "axis 3 cadence: observed " << observed_rate << " Hz exceeds "
        << "expected " << expected_rate << " Hz (tolerance "
        << (kRateTolerance * 100) << "%) — heartbeat sending faster than "
        << "negotiated; cadence-gate broken?";

    // Axis 4 — Symmetry: broker received within tolerance of role sent.
    // Localhost CURVE: no realistic loss path, so use a TIGHT tolerance
    // here (1 frame slack).  If broker-received is wildly off from
    // role-sent, the counter wiring is the suspect.
    EXPECT_GE(received_count, sent_count - 1)
        << "axis 4 symmetry: broker received=" << received_count
        << " is far below role sent=" << sent_count
        << "; suspect handle_heartbeat_req drop path or counter wiring";
    EXPECT_LE(received_count, sent_count + 1)
        << "axis 4 symmetry: broker received=" << received_count
        << " exceeds role sent=" << sent_count
        << "; impossible without retransmits — counter bug";

    ExpectWorkerOk(broker);
    ExpectWorkerOk(role);
}

} // namespace
