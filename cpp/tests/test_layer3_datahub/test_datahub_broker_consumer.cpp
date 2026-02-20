/**
 * @file test_datahub_broker_consumer.cpp
 * @brief Consumer registration protocol integration tests.
 *
 * Tests the CONSUMER_REG_REQ / CONSUMER_DEREG_REQ broker protocol and the
 * consumer_count field in DISC_ACK, via both Messenger and raw ZMQ.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubBrokerConsumerTest : public IsolatedProcessTest
{
};

TEST_F(DatahubBrokerConsumerTest, ChannelRegistryConsumerOps)
{
    // Pure ChannelRegistry consumer CRUD — no ZMQ, no lifecycle.
    auto proc = SpawnWorker("broker_consumer.channel_registry_consumer_ops", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerRegChannelNotFound)
{
    // CONSUMER_REG_REQ for an unknown channel → CHANNEL_NOT_FOUND error response (raw ZMQ).
    // Broker logs LOGGER_WARN only; no ERROR-level log expected.
    auto proc = SpawnWorker("broker_consumer.consumer_reg_channel_not_found", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerRegHappyPath)
{
    // Messenger register_consumer → CONSUMER_REG_ACK; DISC_ACK shows consumer_count ≥ 1.
    auto proc = SpawnWorker("broker_consumer.consumer_reg_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerDeregHappyPath)
{
    // Register consumer (raw), deregister (correct pid) → success; consumer_count drops to 0.
    auto proc = SpawnWorker("broker_consumer.consumer_dereg_happy_path", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerDeregPidMismatch)
{
    // Deregister with wrong pid → NOT_REGISTERED error response (raw ZMQ).
    // Broker logs LOGGER_WARN only; no ERROR-level log expected.
    auto proc = SpawnWorker("broker_consumer.consumer_dereg_pid_mismatch", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubBrokerConsumerTest, DiscShowsConsumerCount)
{
    // DISC_ACK consumer_count: 0 initially → 1 after register_consumer → 0 after deregister.
    auto proc = SpawnWorker("broker_consumer.disc_shows_consumer_count", {});
    ExpectWorkerOk(proc);
}
