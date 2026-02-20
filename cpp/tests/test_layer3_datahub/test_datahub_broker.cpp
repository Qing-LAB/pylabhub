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
    // Re-register same channel with different schema_hash → broker rejects with Cat1 error.
    // Broker logs LOGGER_ERROR "Cat1 schema mismatch". Positively verify it appeared.
    auto proc = SpawnWorker("broker.broker_schema_mismatch", {});
    ExpectWorkerOk(proc, {}, {"Cat1 schema mismatch"});
}

TEST_F(DatahubBrokerTest, ChannelNotFound)
{
    // Discover unknown channel → Messenger returns nullopt.
    // Messenger logs LOGGER_ERROR "discover_producer(...) failed". Positively verify it appeared.
    auto proc = SpawnWorker("broker.broker_channel_not_found", {});
    ExpectWorkerOk(proc, {}, {"discover_producer"});
}

TEST_F(DatahubBrokerTest, DeregHappyPath)
{
    // Register → discover (found) → deregister (correct pid) → discover → nullopt.
    // Second discover fails with CHANNEL_NOT_FOUND; Messenger logs LOGGER_ERROR. Verify it appeared.
    auto proc = SpawnWorker("broker.broker_dereg_happy_path", {});
    ExpectWorkerOk(proc, {}, {"discover_producer"});
}

TEST_F(DatahubBrokerTest, DeregPidMismatch)
{
    // Deregister with wrong pid → NOT_REGISTERED (raw ZMQ); broker logs LOGGER_WARN only.
    // No ERROR-level log expected; use plain ExpectWorkerOk to catch any unexpected ERRORs.
    auto proc = SpawnWorker("broker.broker_dereg_pid_mismatch", {});
    ExpectWorkerOk(proc);
}
