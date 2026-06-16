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

// ─── Rung 4: CONSUMER_REG_REQ/ACK + rx queue Standby→Configured→Active ──────

TEST_F(Pattern4ConsumerLifecycleTest, ConsumerRegistersAndQueueAdvances)
{
    using namespace std::chrono;
    using pylabhub::kLongTimeoutMs;
    using pylabhub::kMidTimeoutMs;

    const std::string producer_uid = pylabhub::scripting::make_role_uid(
        pylabhub::scripting::RoleUidTag::Producer, "pattern4cons", 1u);
    const std::string consumer_uid = pylabhub::scripting::make_role_uid(
        pylabhub::scripting::RoleUidTag::Consumer, "pattern4cons", 1u);

    const fs::path temp_dir = make_test_temp_dir("consumer_registers");
    // Both role uids share the keystore so the consumer subprocess's
    // ks_fixture can seed its own kRoleIdentityName from the same
    // setup bundle (CurveKeyStoreFixture::add_identity per uid).
    auto setup            = make_pattern4_setup({producer_uid, consumer_uid});
    setup.shared_log_path = (temp_dir / "shared.log").string();
    write_pattern4_setup(setup, temp_dir / "setup.json");

    const fs::path shared_log = setup.shared_log_path;

    // ── 1. Broker subprocess (held via quit-pipe) ──
    auto broker = SpawnWorkerWithQuitSignal(
        "pattern4_consumer_lifecycle.broker", {temp_dir.string()});
    expect_log_sequence(
        shared_log,
        {"Pattern4Broker: bound endpoint='"},
        milliseconds{kMidTimeoutMs});

    // ── 2. Producer subprocess (held via quit-pipe; sends heartbeat) ──
    //
    // Waiting for the producer's "first heartbeat received" broker-side
    // marker BEFORE spawning the consumer eliminates the consumer's
    // retry loop on `awaiting_first_heartbeat` — keeps the test's
    // marker sequence deterministic.
    auto producer = SpawnWorkerWithQuitSignal(
        "pattern4_consumer_lifecycle.producer_role",
        {temp_dir.string()});
    expect_log_sequence(
        shared_log,
        {
            fmt::format("presence channel='data.test' state "
                        "RegRequestPending->Registered"),
            "Broker: first heartbeat received from role='" + producer_uid + "'",
        },
        milliseconds{kLongTimeoutMs});

    // ── 3. Consumer subprocess (exits after register_consumer +
    //      apply_consumer_reg_ack drives the rx queue Active) ──
    auto consumer = SpawnWorker(
        "pattern4_consumer_lifecycle.consumer_role",
        {temp_dir.string()});

    // ── 4. Pin the 7-marker rung 4 contract sequence ──
    //
    // All markers are one-shot INFO emitted from production code
    // (role_api_base.cpp + broker_service.cpp + hub_zmq_queue.cpp).
    // Substring matches tolerate stable evolution of intra-line
    // payloads (e.g. producers=[...] content) while pinning the
    // identifying anchors.
    // Chronological order (forward-only search):
    //   1. consumer FSM Unregistered->RegRequestPending
    //      (RoleAPIBase::register_consumer entry — role_api_base.cpp:1056)
    //   2. broker CONSUMER_REG_REQ accepted
    //   3. broker CONSUMER_REG_ACK sending
    //   4. consumer FSM RegRequestPending->Registered
    //      (RoleAPIBase::register_consumer exit — role_api_base.cpp:1127,
    //      BEFORE the worker calls apply_consumer_reg_ack)
    //   5. consumer-side apply_consumer_reg_ack head log
    //      (CONSUMER_REG_ACK received — role_api_base.cpp:497)
    //   6. queue PULL Standby->Configured (inside apply_master_approval)
    //   7. queue PULL Configured->Active
    expect_log_sequence(
        shared_log,
        {
            "presence channel='data.test' role_type=consumer state "
            "Unregistered->RegRequestPending (CONSUMER_REG_REQ sending)",
            fmt::format("Broker: CONSUMER_REG_REQ accepted role='{}' "
                        "channel='data.test' consumer_pubkey='",
                        consumer_uid),
            "Broker: CONSUMER_REG_ACK sending channel='data.test' producers=",
            "presence channel='data.test' role_type=consumer state "
            "RegRequestPending->Registered",
            "CONSUMER_REG_ACK received channel='data.test' status=success",
            "[hub::ZmqQueue] PULL state Standby->Configured",
            "[hub::ZmqQueue] PULL state Configured->Active",
        },
        milliseconds{kLongTimeoutMs});

    // ── 5. Termination: consumer exits on its own; producer + broker via pipe ──
    consumer.wait_for_exit();
    producer.signal_quit();
    producer.wait_for_exit();
    broker.signal_quit();
    broker.wait_for_exit();

    ExpectWorkerOk(broker);
    ExpectWorkerOk(producer);
    ExpectWorkerOk(consumer);
}

} // namespace
