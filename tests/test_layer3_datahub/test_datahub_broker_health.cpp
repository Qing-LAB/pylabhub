/**
 * @file test_datahub_broker_health.cpp
 * @brief Broker/Producer/Consumer health and notification tests.
 *
 * Tests Cat 1 and Cat 2 error detection and notification:
 *  - Cat 1: heartbeat timeout → producer receives CHANNEL_CLOSING_NOTIFY
 *  - Cat 1: schema mismatch → existing producer receives CHANNEL_ERROR_NOTIFY
 *  - Cat 2: dead consumer PID → producer receives CONSUMER_DIED_NOTIFY
 *  - Correctness: Consumer::close() sends CONSUMER_DEREG_REQ
 *  - Correctness: Producer::close() sends DEREG_REQ for immediate re-registration
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

#include "plh_platform.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace pylabhub::tests;
namespace fs = std::filesystem;

class DatahubBrokerHealthTest : public IsolatedProcessTest
{
};

// ProducerGetsClosingNotify / ProducerAutoDeregisters / SchemaMismatchNotify
// MIGRATED to tests/test_layer3_pattern4/test_pattern4_broker_health.cpp
// (task #52 Round 2).

TEST_F(DatahubBrokerHealthTest, ConsumerAutoDeregisters)
{
    // Consumer::close() sends CONSUMER_DEREG_REQ; broker consumer_count drops to 0.
    auto proc = SpawnWorker("broker_health.consumer_auto_deregisters", {});
    ExpectWorkerOk(proc);
}

// DeadConsumerDetected MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_health.cpp (task #52 Round 2 — a
// "pattern4_broker_protocol.dying_consumer" subprocess registers then
// std::_Exit(0); the "dead_consumer" broker profile detects the dead PID).

// ── HEP-CORE-0039 P8 migration prerequisites ────────────────────────────────
// Three behaviors that MUST hold before `check_heartbeat_timeouts` can be
// migrated to `for_each_presence_matching`.  See
// `docs/todo/QUERY_LAYER_TODO.md` Pattern P8.

TEST_F(DatahubBrokerHealthTest, MultiProducer_PartialPendingTimeout_ChannelSurvives)
{
    // Two producers on one channel; B stops heartbeating, A keeps going.
    // B's presence is removed but the channel SURVIVES.  A migration that
    // accidentally tears the channel down on any producer drop would fail
    // here (surviving producer A would receive CHANNEL_CLOSING_NOTIFY).
    auto proc = SpawnWorker("broker_health.multi_producer_partial_pending_timeout", {});
    ExpectWorkerOk(proc);
}

// ConsumerHeartbeatTimeout_NotifyBodyShape MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_health.cpp
// (task #52 Round 2 — silent parent-side consumer + "consumer_timeout"
// broker profile; reason="heartbeat_timeout").

TEST_F(DatahubBrokerHealthTest, TwoSnapshotInvariant_DemotionAndTerminationSeparateTicks)
{
    // A presence demoted Connected→Pending in tick T MUST NOT also be
    // terminated Pending→Disconnected in tick T.  Pin via timing:
    // CHANNEL_CLOSING_NOTIFY must not fire before
    // (ready_timeout + pending_timeout - sweep slop) ≈ 800ms.  A
    // single-snapshot migration of `check_heartbeat_timeouts` would
    // collapse the two passes and NOTIFY would fire ~500ms — this test
    // would fail.
    auto proc = SpawnWorker("broker_health.two_snapshot_invariant", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerHealthTest, ChannelTornDown_ConsumerPass2Skipped)
{
    // `channel_torn_down` short-circuit (HEP-0039 P8 Step B prerequisite).
    // Producer + consumer both Pending in same sweep tick; producer
    // Pass-2 evicts channel; consumer Pass-2 MUST be skipped (no stray
    // CONSUMER_DIED_NOTIFY to a vanished channel).  A migration that
    // drops the short-circuit would silently emit a CONSUMER_DIED_NOTIFY
    // after CHANNEL_CLOSING_NOTIFY — this test asserts the count stays 0.
    auto proc = SpawnWorker("broker_health.channel_torn_down_consumer_pass2_skipped", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerHealthTest, CtrlZapDenyPath)
{
    // HEP-CORE-0035 §4.2 + PeerAdmission Phase D step D2 — the broker's
    // CTRL ROUTER ZAP gate is the production deny-by-default security
    // boundary.  Without this test, every other CURVE-using L3 worker
    // either bypasses the gate (`enforce_ctrl_admission = false`) or
    // exercises only the allow path (L4 roundtrip via --add-known-role).
    // Audit B2: shipping a deny-by-default gate with zero negative-path
    // coverage is a security-gate test rigor failure.
    //
    // Worker constructs a broker with `enforce=true` + empty allowlist
    // + empty federation peers, then connects a BRC client with an
    // explicit (test-owned) CURVE keypair that is NOT in any
    // allowlist.  Expected outcome:
    //   - BRC TCP connect succeeds (transport-level)
    //   - REG_REQ times out (CURVE handshake denied by ZAP)
    //   - `ZapRouter::denied_count()` increases (PATH-DISCRIMINATING:
    //     the timeout came from the ZAP gate, not from any other
    //     misconfig)
    auto proc = SpawnWorker("broker_health.ctrl_zap_deny_path", {});
    ExpectWorkerOk(proc);
}
