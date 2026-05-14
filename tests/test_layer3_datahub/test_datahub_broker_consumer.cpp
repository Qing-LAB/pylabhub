/**
 * @file test_datahub_broker_consumer.cpp
 * @brief Pattern 3 driver — consumer registration protocol tests
 *        (CONSUMER_REG_REQ / CONSUMER_DEREG_REQ + consumer_count in DISC_ACK).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/broker_consumer_workers.cpp`.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class DatahubBrokerConsumerTest : public IsolatedProcessTest
{
};

TEST_F(DatahubBrokerConsumerTest, ConsumerRegChannelNotFound)
{
    auto w = SpawnWorker("broker_consumer.consumer_reg_channel_not_found");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerRegHappyPath)
{
    auto w = SpawnWorker("broker_consumer.consumer_reg_happy_path");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerDeregHappyPath)
{
    auto w = SpawnWorker("broker_consumer.consumer_dereg_happy_path");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerDeregPidMismatch)
{
    auto w = SpawnWorker("broker_consumer.consumer_dereg_pid_mismatch");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, DiscShowsConsumerCount)
{
    auto w = SpawnWorker("broker_consumer.disc_shows_consumer_count");
    ExpectWorkerOk(w);
}
