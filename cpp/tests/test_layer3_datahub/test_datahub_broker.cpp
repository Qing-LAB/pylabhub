/**
 * @file test_datahub_broker.cpp
 * @brief Phase C — BrokerService integration tests.
 *
 * Tests the real BrokerService (ChannelRegistry + ROUTER loop) via both
 * Messenger (for happy paths) and raw ZMQ (for error-path verification).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubBrokerTest : public IsolatedProcessTest
{
};

TEST_F(DatahubBrokerTest, ChannelRegistryOps)
{
    // Pure ChannelRegistry unit tests — no ZMQ, no lifecycle.
    auto proc = SpawnWorker("broker.channel_registry_ops", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, RegDiscHappyPath)
{
    // Full REG/DISC round-trip: Messenger → real BrokerService.
    auto proc = SpawnWorker("broker.broker_reg_disc_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, SchemaMismatch)
{
    // Re-register same channel with different schema_hash → ERROR SCHEMA_MISMATCH.
    // Broker and Messenger both log at WARN/ERROR level for this case.
    auto proc = SpawnWorker("broker.broker_schema_mismatch", {});
    ExpectWorkerOk(proc, {}, /*allow_expected_logger_errors=*/true);
}

TEST_F(DatahubBrokerTest, ChannelNotFound)
{
    // Discover unknown channel → Messenger returns nullopt.
    // Messenger logs ERROR when broker replies CHANNEL_NOT_FOUND.
    auto proc = SpawnWorker("broker.broker_channel_not_found", {});
    ExpectWorkerOk(proc, {}, /*allow_expected_logger_errors=*/true);
}

TEST_F(DatahubBrokerTest, DeregHappyPath)
{
    // Register → discover (found) → deregister (correct pid) → discover → nullopt.
    // Messenger logs ERROR when discover returns CHANNEL_NOT_FOUND after deregister.
    auto proc = SpawnWorker("broker.broker_dereg_happy_path", {});
    ExpectWorkerOk(proc, {}, /*allow_expected_logger_errors=*/true);
}

TEST_F(DatahubBrokerTest, DeregPidMismatch)
{
    // Deregister with wrong pid → ERROR NOT_REGISTERED; channel still discoverable.
    auto proc = SpawnWorker("broker.broker_dereg_pid_mismatch", {});
    ExpectWorkerOk(proc, {}, /*allow_expected_logger_errors=*/true);
}
