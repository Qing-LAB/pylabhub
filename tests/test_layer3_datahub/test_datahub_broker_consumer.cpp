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

TEST_F(DatahubBrokerConsumerTest, ConsumerRegUnknownRole)
{
    auto w = SpawnWorker("broker_consumer.consumer_reg_unknown_role");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerRegPubkeyMismatch)
{
    auto w = SpawnWorker("broker_consumer.consumer_reg_pubkey_mismatch");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerRegAckEmitsProducersZmq)
{
    auto w = SpawnWorker("broker_consumer.consumer_reg_ack_emits_producers_zmq");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, GetChannelAuthReturnsAllowlist)
{
    auto w = SpawnWorker("broker_consumer.get_channel_auth_returns_allowlist");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, GetChannelAuthRejectsNonProducer)
{
    auto w = SpawnWorker("broker_consumer.get_channel_auth_rejects_non_producer");
    ExpectWorkerOk(w);
}

// ── HEP-CORE-0041 §9 D4 — CONSUMER_ATTACH_REQ broker handler ──────────

TEST_F(DatahubBrokerConsumerTest, ConsumerAttachAuthorized)
{
    auto w = SpawnWorker("broker_consumer.consumer_attach_authorized");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerAttachDenied)
{
    auto w = SpawnWorker("broker_consumer.consumer_attach_denied");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerAttachChannelNotFound)
{
    auto w = SpawnWorker("broker_consumer.consumer_attach_channel_not_found");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerAttachNonProducer)
{
    auto w = SpawnWorker("broker_consumer.consumer_attach_non_producer");
    ExpectWorkerOk(w);
}

TEST_F(DatahubBrokerConsumerTest, ConsumerAttachInvalidRequest)
{
    auto w = SpawnWorker("broker_consumer.consumer_attach_invalid_request");
    ExpectWorkerOk(w);
}
