/**
 * @file test_datahub_broker.cpp
 * @brief Phase C — BrokerService integration tests.
 *
 * Tests the real BrokerService (ChannelRegistry + ROUTER loop) via
 * BrokerRequestComm (for happy paths) and raw ZMQ (for error-path verification).
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
    // Full REG/DISC round-trip: BrokerRequestComm → real BrokerService.
    auto proc = SpawnWorker("broker.broker_reg_disc_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, SchemaMismatch)
{
    // Re-register same channel with different schema_hash → broker rejects with Cat1 error.
    // Broker logs LOGGER_ERROR "Cat1 schema mismatch". Positively verify it appeared.
    auto proc = SpawnWorker("broker.broker_schema_mismatch", {});
    ExpectWorkerOk(proc, {}, {"Cat1 schema mismatch", "CHANNEL_ERROR_NOTIFY"});
}

TEST_F(DatahubBrokerTest, ChannelNotFound)
{
    // Discover unknown channel -> BRC returns nullopt (verified inside worker).
    // Under HEP-CORE-0023 three-response DISC_REQ, broker replies with ERROR
    // payload (error_code=CHANNEL_NOT_FOUND); no ERROR log is emitted.
    auto proc = SpawnWorker("broker.broker_channel_not_found", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, DeregHappyPath)
{
    // Register -> discover (found) -> deregister -> discover -> nullopt.
    // Second discover returns CHANNEL_NOT_FOUND via wire; no ERROR log expected.
    auto proc = SpawnWorker("broker.broker_dereg_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerTest, DeregPidMismatch)
{
    // Deregister with wrong pid → NOT_REGISTERED (raw ZMQ); broker logs LOGGER_WARN only.
    // No ERROR-level log expected; use plain ExpectWorkerOk to catch any unexpected ERRORs.
    auto proc = SpawnWorker("broker.broker_dereg_pid_mismatch", {});
    ExpectWorkerOk(proc);
}
