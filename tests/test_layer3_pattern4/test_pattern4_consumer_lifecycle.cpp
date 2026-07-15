/**
 * @file test_pattern4_consumer_lifecycle.cpp
 * @brief Pattern 4 rung 4 — Pattern4ConsumerLifecycleTest (task #222).
 *
 * Verifies the consumer registration + channel-state lifecycle
 * contract end-to-end across three subprocesses:
 *
 *   1. Parent spawns broker (held via quit-pipe) + producer (held via
 *      quit-pipe so its presence stays kLive while the consumer
 *      registers) + consumer (exits after register_consumer +
 *      apply_consumer_reg_ack).
 *   2. Producer registers + installs heartbeat, so the broker
 *      observes `first_heartbeat_seen=true` — the gate for admitting
 *      CONSUMER_REG_REQ per HEP-CORE-0036 §5.2 R6.
 *   3. Consumer calls register_consumer (which internally retries
 *      `CHANNEL_NOT_READY/awaiting_first_heartbeat` until the producer
 *      first heartbeat lands), then calls apply_consumer_reg_ack
 *      which drives the rx queue Standby → Configured → Active per
 *      HEP-CORE-0036 §6.7.
 *   4. Parent pins the cross-process marker sequence on the shared log.
 *
 * Production INFO markers exercised (forward-only chronological order;
 * see test body for the rationale on why FSM Registered lands BEFORE
 * the queue transitions):
 *   - role  (consumer): "presence channel='data.test' role_type=consumer
 *                       state Unregistered->RegRequestPending (CONSUMER_REG_REQ sending)"
 *   - broker:           "Broker: CONSUMER_REG_REQ accepted role='<uid>'
 *                       channel='data.test' consumer_pubkey='...'"
 *   - broker:           "Broker: CONSUMER_REG_ACK sending channel='data.test'
 *                       producers=[...]"
 *   - role  (consumer): "presence channel='data.test' role_type=consumer
 *                       state RegRequestPending->Registered"
 *   - role  (consumer): "CONSUMER_REG_ACK received channel='data.test'
 *                       status=success producers=[...]"
 *   - queue (rx):       "[hub::ZmqQueue] PULL state Standby->Configured ..."
 *   - queue (rx):       "[hub::ZmqQueue] PULL state Configured->Active ..."
 *
 * See docs/README/README_testing.md § "Pattern 4 — ... — Test ladder"
 * rung 4 for the contract this test pins.
 */
#include "broker_wire_client.h"
#include "pattern4_helpers.h"

#include "shared_test_helpers.h"
#include "test_patterns.h"

#include "utils/role_reg_payload.hpp"
#include "utils/role_uid.hpp"
#include "utils/timeout_constants.hpp"

#include <cppzmq/zmq.hpp>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using pylabhub::tests::IsolatedProcessTest;
using pylabhub::tests::pattern4::BrokerWireClient;
using pylabhub::tests::pattern4::expect_log;
using pylabhub::tests::pattern4::expect_log_sequence;
using pylabhub::tests::pattern4::make_pattern4_setup;
using pylabhub::tests::pattern4::make_temp_dir;
using pylabhub::tests::pattern4::pick_unused_port;
using pylabhub::tests::pattern4::write_pattern4_setup;

namespace
{

class Pattern4ConsumerLifecycleTest : public IsolatedProcessTest
{
protected:
    void TearDown() override
    {
        if (std::getenv("PLH_TEST_KEEP_TEMP") != nullptr)
        {
            for (const auto &p : paths_to_clean_)
                fmt::print(stderr, "[KEEP_TEMP] {}\n", p.string());
            paths_to_clean_.clear();
            return;
        }
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

// ─── Rung 4 happy-path RETIRED 2026-07-02 (Phase 3b.2) ──────────────
//
// The former `ConsumerRegistersAndQueueAdvances` pinned the 7-marker
// sequence for "consumer REGs against an idle producer → rx queue
// PULL Configured → Active".  That contract held ONLY under the pre-
// Phase-3b consumer flow, where `apply_consumer_reg_ack` handed the
// broker's ACK directly to `rx_queue->apply_master_approval` without
// coordinating with the producer.
//
// Phase 3b.2 (commit this file) makes `apply_consumer_reg_ack` issue
// HEP-CORE-0042 §7.1 `CONSUMER_ATTACH_REQ_ZMQ` for each declared
// producer BEFORE the queue starts.  Against the idle
// `pattern4_consumer_lifecycle.producer_role` worker (which never
// runs a data-loop cycle, so it never processes NOTIFY and never
// emits CHANNEL_AUTH_APPLIED_REQ), the broker's timeout drain (§5.4
// wait-path + §5.6 `producer_apply_wait_ms`) is the CORRECT protocol
// outcome — the queue must stay in Standby and the consumer's
// `apply_consumer_reg_ack` must return false.  A "success" outcome
// under those inputs would violate HEP-CORE-0042 §4 P4 (security-
// correctness invariant: the consumer only dials producers the broker
// has confirmed have the consumer's pubkey in their cache).
//
// Happy-path coverage moved to L4 (`test_plh_hub_role_zmq_e2e.cpp`)
// where the real `plh_role` binary + Lua script produce a cycling
// producer that DOES process NOTIFY and DOES emit APPLIED_REQ — the
// end-to-end scenario the retired L3 test approximated.  Error-path
// coverage stays at L3 via `IdleProducerYieldsTimeoutDrainToConsumer`
// (Phase 3a.4 Test A, below).
//
// See `docs/HEP/HEP-CORE-0042-Channel-Attach-Coordination-Protocol.md`
// §7.1 (consumer flow) + `docs/todo/TESTING_TODO.md` (Phase 3a L4
// follow-up section) for the migration record.

// ─── Error-path: idle real producer → broker timeout drain ────────────
//
// HEP-CORE-0042 §5.4 timeout drain + §5.6 taxonomy.  Exercises the
// same protocol path as
// `Pattern4AttachCoordinationTest.WaitPathTimeoutOnMissingAppliedReq`
// (shipped 2026-07-01 in commit `6a7a7508`), but with a REAL producer
// role subprocess as the "not responding" source instead of a raw
// wire client that never sends APPLIED_REQ.  This closes the
// contract-vs-reality gap: a producer that is registered + alive
// (heartbeats fine) but whose worker cycle is idle — never processes
// CHANNEL_AUTH_CHANGED_NOTIFY — is EXACTLY what §5.4 timeout drain
// is designed for.  The `pattern4_consumer_lifecycle.producer_role`
// worker matches this profile: it registers + heartbeats + then
// holds via quit-pipe, never iterating the cycle that runs
// `handle_channel_auth_notifies` (role_api_base.cpp:1403).
//
// Sequence:
//   1. Broker + real (idle) producer subprocesses; producer
//      registers, emits initial-REG APPLIED_REQ (applied_version=0),
//      then holds.
//   2. Wire client acts as consumer: sends CONSUMER_REG_REQ →
//      broker admits + bumps channel_version[K] to 1 + fires NOTIFY
//      at producer.  Producer NEVER processes it (idle cycle).
//   3. Wire client sends CONSUMER_ATTACH_REQ_ZMQ fire-and-forget.
//      Broker: consumer.pubkey ∈ allowlist ✓, producer live ✓,
//      confirmed_version(0) < channel_version(1) → §5.4 step 5
//      wait-path enqueue.
//   4. `producer_apply_wait_ms` (default 3000ms) elapses without
//      APPLIED_REQ from the producer.
//   5. Broker's `sweep_pending_attach_timeouts_` drains with
//      {status="timeout", reason=
//       "producer_did_not_confirm_within_budget"}.
//   6. Wire client receives the timeout reply.
//
// Assertions:
//   - Producer's initial-REG emission log fired (proves Phase 3a.3b
//     path (1) works via real producer).
//   - Wire client receives the §5.6 timeout drain reply within a
//     ~6s budget (matches attach_coordination test's cushion).
//   - Wire client receives NOTHING within the first 500ms — the
//     deferred-reply contract must hold (broker does not premature-
//     reply on §5.4 step 5).

TEST_F(Pattern4ConsumerLifecycleTest, IdleProducerYieldsTimeoutDrainToConsumer)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = pylabhub::scripting::make_role_uid(
        pylabhub::scripting::RoleUidTag::Producer, "pattern4cons", 1u);
    const std::string consumer_uid = pylabhub::scripting::make_role_uid(
        pylabhub::scripting::RoleUidTag::Consumer, "pattern4cons", 1u);

    const fs::path temp_dir = make_test_temp_dir("consumer_registers_timeout");
    auto setup            = make_pattern4_setup({producer_uid, consumer_uid});
    setup.shared_log_path = (temp_dir / "shared.log").string();
    write_pattern4_setup(setup, temp_dir / "setup.json");
    const fs::path shared_log = setup.shared_log_path;

    // ── 1. Broker + real producer (idle, holds via quit-pipe) ──
    auto broker = SpawnWorkerWithQuitSignal(
        "pattern4_consumer_lifecycle.broker", {temp_dir.string()});
    expect_log_sequence(
        shared_log,
        {"Pattern4Broker: bound endpoint='"},
        milliseconds{kMidTimeoutMs});

    auto producer = SpawnWorkerWithQuitSignal(
        "pattern4_consumer_lifecycle.producer_role",
        {temp_dir.string()});
    // Physical wall-clock order (verified from a captured run):
    //   1. Broker receives APPLIED_REQ + logs `event=ChannelAuthApplied
    //      producer_uid=...` (broker-side receipt).
    //   2. Broker sends APPLIED_ACK; producer receives it + logs
    //      `event=ChannelAuthApplied channel='data.test'
    //      applied_version=0` (role-side send-then-ack).
    //   3. Producer's first heartbeat lands at the broker.
    // Forward-only sequence order must MATCH this — if reversed, the
    // sequence walker searches forward-only from an already-past
    // marker and hangs on the missing one.
    expect_log_sequence(
        shared_log,
        {
            // Broker's receive-side confirmation (advances
            // confirmed_version[K][P] to 0, no-op advance on this
            // fresh channel).
            "event=ChannelAuthApplied channel='data.test' producer_uid='" +
                producer_uid + "' instance=1 applied_version=0 confirmed_version=0",
            // Producer's send-side emission log (Phase 3a.3b path (1)
            // — the log line proves the real producer AUTO-EMITTED
            // APPLIED_REQ from `apply_producer_reg_ack`).
            "event=ChannelAuthApplied channel='data.test' applied_version=0",
            // Producer has heartbeated at least once — required for
            // CONSUMER_REG_REQ to bypass `awaiting_first_heartbeat`.
            "Broker: first heartbeat received from role='" + producer_uid + "'",
        },
        milliseconds{kLongTimeoutMs});

    // ── 2. Wire client acts as consumer (own DEALER; same identity
    //      as `setup.curve.role(consumer_uid)` so the broker's
    //      §I10 one-pubkey-per-uid invariant is satisfied) ──
    zmq::context_t ctx;
    const auto &cons_kp = setup.curve.role(consumer_uid);
    BrokerWireClient::Config cons_cfg;
    cons_cfg.broker_endpoint  = setup.broker_endpoint;
    cons_cfg.broker_pubkey    = setup.curve.hub.public_z85;
    cons_cfg.client_pubkey    = cons_kp.public_z85;
    cons_cfg.client_seckey    = cons_kp.secret_z85;
    cons_cfg.client_role_uid  = consumer_uid;
    BrokerWireClient cons_client(ctx, cons_cfg);

    // ── 3. Consumer REG — bumps channel_version[K] 0→1 and fires
    //      NOTIFY at producer.  Producer is idle → NOTIFY sits in
    //      its BRC inbox unread. ──
    {
        pylabhub::hub::ConsumerRegInputs in;
        in.channel        = "data.test";
        in.role_uid       = consumer_uid;
        in.role_name      = "pattern4cons";
        in.role_type      = "consumer";
        in.data_transport = "zmq";
        in.zmq_pubkey     = cons_kp.public_z85;
        auto payload = pylabhub::hub::build_consumer_reg_payload(in);
        auto reply = cons_client.request(
            "CONSUMER_REG_REQ", payload, "CONSUMER_REG_ACK",
            milliseconds{kLongTimeoutMs});
        ASSERT_TRUE(reply.has_value())
            << "wire consumer REG_REQ: no reply";
        ASSERT_EQ(reply->value("status", ""), "success")
            << "wire consumer REG_REQ failed: " << reply->dump();
    }

    // ── 4. Consumer ATTACH_REQ — fire-and-forget.  Broker will
    //      enqueue (wait-path) since confirmed(0) < channel_version(1). ──
    {
        nlohmann::json attach;
        attach["channel_name"]      = "data.test";
        attach["consumer_role_uid"] = consumer_uid;
        attach["consumer_pubkey"]   = cons_kp.public_z85;
        attach["producer_role_uid"] = producer_uid;
        cons_client.send("CONSUMER_ATTACH_REQ_ZMQ", attach);
    }

    // Log-barrier: prove the wait-path enqueue happened.  This is
    // the anti-race pin — if the producer's initial-REG emission
    // somehow raced ahead of the consumer's REG (which SHOULD have
    // bumped channel_version to 1 before our ATTACH), we'd hit a
    // §5.4 fast-path admit instead of the timeout drain we want.
    // Uses `expect_log_sequence` (which polls the SHARED log file
    // where broker markers land) — NOT `expect_log` (which polls
    // worker stderr; broker's own log lines flow to shared_log
    // per the Pattern4 multi-process aggregation contract).
    //
    // No premature-reply check here (unlike the raw-wire
    // WaitPathTimeoutOnMissingAppliedReq sibling): the sweep budget
    // is 3s + heartbeat_interval slack, so by the time expect_log
    // has polled the log stream + found the enqueue marker + we
    // reach a receive() call, we're already past the sweep-firing
    // window.  The deferred-reply contract itself IS verified by
    // its dedicated raw-wire attach-coordination test — no need to
    // re-verify from this test which is scoped to real-producer.
    expect_log_sequence(
        shared_log,
        {"event=AttachReqZmqEnqueued channel='data.test'"},
        milliseconds{kMidTimeoutMs});

    // ── 5. Timeout drain.  Broker's sweep fires at
    //      producer_apply_wait_ms + ~heartbeat_interval ≈ 3.5s.
    //      Poll for 6s to absorb CI jitter (matches
    //      Pattern4AttachCoordinationTest.WaitPathTimeoutOn... cushion). ──
    {
        auto reply = cons_client.receive(milliseconds{6000});
        ASSERT_TRUE(reply.has_value())
            << "consumer did not receive timeout reply within 6s — "
               "broker sweep did not drain, or producer somehow "
               "responded (which would contradict its idle-cycle "
               "assumption)";
        EXPECT_EQ(reply->first, "CONSUMER_ATTACH_ACK_ZMQ");
        EXPECT_EQ(reply->second.value("status", ""), "timeout")
            << reply->second.dump();
        EXPECT_EQ(reply->second.value("reason", ""),
                   "producer_did_not_confirm_within_budget")
            << reply->second.dump();
    }

    // ── 6. Termination ──
    producer.signal_quit();
    producer.wait_for_exit();
    broker.signal_quit();
    broker.wait_for_exit();

    ExpectWorkerOk(broker);
    ExpectWorkerOk(producer);
}

} // namespace
