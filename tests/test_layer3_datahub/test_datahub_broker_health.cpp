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

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace pylabhub::tests;
namespace fs = std::filesystem;

class DatahubBrokerHealthTest : public IsolatedProcessTest
{
};

TEST_F(DatahubBrokerHealthTest, ProducerGetsClosingNotify)
{
    // Cat 1: heartbeat timeout (1s) — producer's on_channel_closing fires.
    auto proc = SpawnWorker("broker_health.producer_gets_closing_notify", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerHealthTest, ConsumerAutoDeregisters)
{
    // Consumer::close() sends CONSUMER_DEREG_REQ; broker consumer_count drops to 0.
    auto proc = SpawnWorker("broker_health.consumer_auto_deregisters", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerHealthTest, ProducerAutoDeregisters)
{
    // Producer::close() sends DEREG_REQ; same channel re-created immediately (no timeout).
    auto proc = SpawnWorker("broker_health.producer_auto_deregisters", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerHealthTest, DeadConsumerDetected)
{
    // Cat 2: consumer crashes (no clean deregister); broker liveness check (1s) detects
    // dead PID and sends CONSUMER_DIED_NOTIFY to producer.
    //
    // Two worker subprocesses coordinate via a temp file:
    //   orchestrator: starts broker + producer, writes connection info to temp file,
    //                 signals ready, then waits for CONSUMER_DIED_NOTIFY.
    //   exiter:       reads temp file, connects consumer, then calls _exit(0).

    // Create a temp file path for inter-process coordination.
    fs::path tmp = fs::temp_directory_path() /
                   ("plh_dead_consumer_" + std::to_string(getpid()) + ".txt");
    const std::string tmp_str = tmp.string();

    auto orchestrator = SpawnWorkerWithReadySignal(
        "broker_health.dead_consumer_orchestrator", {tmp_str});

    // Block until orchestrator has written the temp file and is ready.
    orchestrator.wait_for_ready();

    // Now spawn the exiter: it reads the temp file, connects, then _exit(0).
    auto exiter = SpawnWorker("broker_health.dead_consumer_exiter", {tmp_str});

    // Exiter should exit quickly (it calls _exit after connecting).
    exiter.wait_for_exit();
    // The exiter calls _exit(0) so the exit code is 0, but it bypasses gtest machinery.
    // We just verify it exited cleanly (code 0 or the C++ runner may differ — allow any).

    // Orchestrator waits for CONSUMER_DIED_NOTIFY then exits with gtest result.
    orchestrator.wait_for_exit();
    ExpectWorkerOk(orchestrator);

    // Cleanup temp file.
    std::error_code ec;
    fs::remove(tmp, ec);
}

TEST_F(DatahubBrokerHealthTest, SchemaMismatchNotify)
{
    // Cat 1: Producer B tries to register the same channel as Producer A with a
    // different schema_hash. Broker rejects B and sends CHANNEL_ERROR_NOTIFY to A.
    auto proc = SpawnWorker("broker_health.schema_mismatch_notify", {});
    // Cat 1 mismatch intentionally produces ERROR-level logs:
    //  - broker: "Cat1 schema mismatch" (sent to existing producer)
    //  - broker notifies via "CHANNEL_ERROR_NOTIFY"
    //  - messenger B: "REG_ACK failed: Schema hash differs" (rejected producer gets error back)
    ExpectWorkerOk(proc, {}, {"Cat1 schema mismatch", "CHANNEL_ERROR_NOTIFY",
                              "REG_ACK failed: Schema hash differs"});
}
