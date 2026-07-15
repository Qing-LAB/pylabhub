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

// RETIRED 2026-06-28 — `ClosingNotify_DeliveredToProducerAndConsumer`
// pinned the CHANNEL_CLOSING_NOTIFY fan-out-to-all-members invariant
// (broker emits the notify to BOTH producer-side AND consumer-side
// BRCs after `broker.request_close_channel()`).  That contract is
// canonical multi-process wire-protocol territory and is now tracked
// at L4 by Pattern 4 rung 8 (task #225 `Pattern4ChannelNotifiesTest`).
// Task #225's description was extended on retirement to explicitly
// absorb the fan-out cardinality + dual-receipt invariant + in-process
// `request_close_channel` trigger path that this L3 test was the only
// site pinning.  See README_testing rule 6 (RETIRE obsolete tests) +
// `docs/code_review/REVIEW_AUTH6_TestDisposition_2026-06-27.md` (audit
// did not flag this as DELETE — retirement decision made during C1
// migration after explicit overlap analysis against Pattern 4 ladder).

// ============================================================================
// 3. Duplicate REG_REQ — SHM cardinality + schema hash conflict
// ============================================================================

TEST_F(BrokerProtocolTest,
       DuplicateReg_TwoDistinctProducers_OnShmChannel_RejectedOneToOneCardinality)
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
// 4. HEARTBEAT_NOTIFY — PendingReady → Ready + wire payload + keying
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
// 8. CHANNEL_BROADCAST_SEND_NOTIFY — fan-out to producer + ALL consumers
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

// ============================================================================
// Audit TR1 — wire-conformance ACK-shape regression tests (2026-05-17)
// ============================================================================
// Pre-2026-05-17 the test suite had no test that pinned a wire
// payload key set directly against its authoritative HEP §.  Audit
// B1 found that BRC + broker had agreed on the wrong band wire key
// (`channel` instead of `band` per HEP-CORE-0030 §5.1) for over a
// year — round-trip tests passed because both ends used the same
// wrong name.  These tests lock down the observable shape of major
// ACK families against their authoritative HEP, asserting both
// REQUIRED keys AND ABSENCE of legacy keys.  The shared helpers
// (`tests/test_framework/wire_conformance.h`) emit precise
// diagnostics naming the missing/forbidden key + the HEP § the rule
// comes from.

TEST_F(BrokerProtocolTest, WireConformance_RegAck_Shape)
{
    auto w = SpawnWorker("broker_protocol.wire_conformance_reg_ack_shape");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, WireConformance_ConsumerRegAck_Shape)
{
    auto w = SpawnWorker(
        "broker_protocol.wire_conformance_consumer_reg_ack_shape");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, WireConformance_RoleInfoAck_Shape)
{
    auto w = SpawnWorker(
        "broker_protocol.wire_conformance_role_info_ack_shape");
    ExpectWorkerOk(w);
}

TEST_F(BrokerProtocolTest, WireConformance_BandAck_Shapes)
{
    auto w = SpawnWorker("broker_protocol.wire_conformance_band_ack_shapes");
    ExpectWorkerOk(w);
}

// WireConformance_Band_CorrIdEcho migrated to
// `tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp`
// (Round 1 of HubHostBrokerHandle sweep, task #54).

// Audit R3.6 (2026-05-17) — `WireConformance_ChannelNotifyReq_FederationRelay`
// retired.  When I tried to add a regression test for the broker's
// `handle_channel_notify_req` (which O1 left in place "for federation
// peer relay"), I discovered federation actually uses HUB_RELAY_MSG —
// not CHANNEL_NOTIFY_REQ.  The handler was 100% dead.  Resolution:
// deleted the handler entirely (broker_service.cpp + dispatch entry
// + known_msg_type list).  No regression test needed — old clients
// sending CHANNEL_NOTIFY_REQ now receive UNKNOWN_MSG_TYPE (covered
// by the existing unknown-msg-type test path).
