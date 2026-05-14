/**
 * @file test_datahub_broker_protocol.cpp
 * @brief Pattern 3 driver — broker control-plane protocol tests.
 *
 * Migrated 2026-05-14 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/datahub_broker_protocol_workers.cpp`.  Matches the
 * sibling broker-family migrations (broker_consumer, broker_schema,
 * broker_admin) — each `TEST_F` here is a thin `SpawnWorker(...)` +
 * `ExpectWorkerOk(w)` stub that runs the body in a fresh subprocess
 * with its own `LifecycleGuard`.
 *
 * Suite name `BrokerProtocolTest` is preserved so `ctest` discovery
 * is unchanged; the test names below match the original gtest names
 * one-for-one (just the bodies moved out).
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class BrokerProtocolTest : public IsolatedProcessTest
{
};

// ============================================================================
// 1. CHECKSUM_ERROR_REPORT — broker forwards as CHANNEL_EVENT_NOTIFY
// ============================================================================

TEST_F(BrokerProtocolTest, ChecksumErrorReport_ForwardedToProducer)
{
    auto w = SpawnWorker(
        "broker_protocol.checksum_error_report_forwarded_to_producer");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, ChecksumErrorReport_UnknownChannel_Silent)
{
    auto w = SpawnWorker(
        "broker_protocol.checksum_error_report_unknown_channel_silent");
    ExpectWorkerOk(w);
}

// ============================================================================
// 2. CHANNEL_CLOSING_NOTIFY — delivery to ALL registered members
// ============================================================================

TEST_F(BrokerProtocolTest, ClosingNotify_DeliveredToProducerAndConsumer)
{
    auto w = SpawnWorker(
        "broker_protocol.closing_notify_delivered_to_producer_and_consumer");
    ExpectWorkerOk(w);
}

// ============================================================================
// 3. Duplicate REG_REQ — SHM cardinality + schema hash conflict
// ============================================================================

TEST_F(BrokerProtocolTest,
       DuplicateReg_TwoDistinctProducers_OnShmChannel_RejectedShmCardinality)
{
    auto w = SpawnWorker("broker_protocol.duplicate_reg_shm_cardinality");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, DuplicateReg_DifferentSchemaHash_Rejected)
{
    auto w = SpawnWorker(
        "broker_protocol.duplicate_reg_different_schema_hash");
    ExpectWorkerOk(w);
}

// ============================================================================
// 4. HEARTBEAT_REQ — PendingReady → Ready + wire payload + keying
// ============================================================================

TEST_F(BrokerProtocolTest, Heartbeat_TransitionsToReady)
{
    auto w = SpawnWorker("broker_protocol.heartbeat_transitions_to_ready");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, Heartbeat_WirePayloadIncludesUidAndRoleType)
{
    auto w = SpawnWorker(
        "broker_protocol.heartbeat_wire_payload_includes_uid_and_role_type");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, HeartbeatKeying_ProducerVsConsumer_DistinctRows)
{
    auto w = SpawnWorker(
        "broker_protocol.heartbeat_keying_producer_vs_consumer_distinct_rows");
    ExpectWorkerOk(w);
}

// ============================================================================
// 5. ROLE_PRESENCE_REQ + ROLE_INFO_REQ
// ============================================================================

TEST_F(BrokerProtocolTest, RolePresenceReq_UnknownUid_ReturnsFalse)
{
    auto w = SpawnWorker("broker_protocol.role_presence_req_unknown_uid");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, RoleInfoReq_UnknownUid_NotFound)
{
    auto w = SpawnWorker("broker_protocol.role_info_req_unknown_uid");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, RolePresenceReq_ProducerUid_ReturnsTrue)
{
    auto w = SpawnWorker("broker_protocol.role_presence_req_producer_uid");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, RolePresenceReq_ConsumerUid_ReturnsTrue)
{
    auto w = SpawnWorker("broker_protocol.role_presence_req_consumer_uid");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, RoleInfoReq_WithInbox_ReturnsInfo)
{
    auto w = SpawnWorker("broker_protocol.role_info_req_with_inbox");
    ExpectWorkerOk(w);
}

// ============================================================================
// 6. Transport arbitration
// ============================================================================

TEST_F(BrokerProtocolTest, TransportMismatch_ShmProducer_ZmqConsumer_Fails)
{
    auto w = SpawnWorker(
        "broker_protocol.transport_mismatch_shm_producer_zmq_consumer");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, TransportMatch_ShmConsumer_ShmProducer_Succeeds)
{
    auto w = SpawnWorker(
        "broker_protocol.transport_match_shm_consumer_shm_producer");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, TransportMatch_NoDriverField_AlwaysSucceeds)
{
    auto w = SpawnWorker("broker_protocol.transport_match_no_driver_field");
    ExpectWorkerOk(w);
}

// ============================================================================
// 7. REG_ACK / CONSUMER_REG_ACK heartbeat-negotiation block
// ============================================================================

TEST_F(BrokerProtocolTest, RegAck_ContainsHeartbeatBlock_Defaults)
{
    auto w = SpawnWorker(
        "broker_protocol.reg_ack_contains_heartbeat_block_defaults");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, RegAck_HeartbeatBlock_HonorsCustomConfig)
{
    auto w = SpawnWorker(
        "broker_protocol.reg_ack_heartbeat_block_honors_custom_config");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, ConsumerRegAck_ContainsHeartbeatBlock)
{
    auto w = SpawnWorker(
        "broker_protocol.consumer_reg_ack_contains_heartbeat_block");
    ExpectWorkerOk(w);
}

// ============================================================================
// 8. CHANNEL_BROADCAST_REQ — fan-out to producer + ALL consumers
// ============================================================================

TEST_F(BrokerProtocolTest, BroadcastFanOut_DeliveredToProducerAndAllConsumers)
{
    auto w = SpawnWorker(
        "broker_protocol.broadcast_fan_out_delivered_to_producer_and_consumers");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, BroadcastFanOut_DataPayloadRoundTrip)
{
    auto w = SpawnWorker(
        "broker_protocol.broadcast_fan_out_data_payload_round_trip");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, BroadcastUnknownChannel_NoNotifyDelivered)
{
    auto w = SpawnWorker(
        "broker_protocol.broadcast_unknown_channel_no_notify_delivered");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, BroadcastFanOut_HubQueuePath_FansOutSame)
{
    auto w = SpawnWorker(
        "broker_protocol.broadcast_fan_out_hub_queue_path_fans_out_same");
    ExpectWorkerOk(w);
}
