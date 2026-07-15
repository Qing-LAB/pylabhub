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

// ChecksumErrorReport_ForwardedToProducer MIGRATED to
// tests/test_layer3_pattern4/ (task #54 Round 1).

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
// DuplicateReg_* MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (task #54 Round 1 — HubHostBrokerHandle antipattern sweep).

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
// RolePresenceReq_* / RoleInfoReq_* MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (task #54 Round 1 — HubHostBrokerHandle antipattern sweep).

// ============================================================================
// 6. Transport arbitration
// ============================================================================
// TransportMismatch_* / TransportMatch_* MIGRATED to
// tests/test_layer3_pattern4/ (task #54 Round 1).

// ============================================================================
// 7. REG_ACK / CONSUMER_REG_ACK heartbeat-negotiation block
// ============================================================================
// RegAck_ContainsHeartbeatBlock_Defaults,
// RegAck_HeartbeatBlock_HonorsCustomConfig (broker "hb_custom" profile),
// ConsumerRegAck_ContainsHeartbeatBlock MIGRATED to
// tests/test_layer3_pattern4/ (task #54 Round 1).

// ============================================================================
// 8. CHANNEL_BROADCAST_SEND_NOTIFY — fan-out to producer + ALL consumers
// ============================================================================

// BroadcastFanOut_DeliveredToProducerAndAllConsumers,
// BroadcastFanOut_DataPayloadRoundTrip, and
// BroadcastUnknownChannel_NoNotifyDelivered MIGRATED to
// tests/test_layer3_pattern4/ (task #54 Round 1).

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

// WireConformance_RegAck_Shape, WireConformance_ConsumerRegAck_Shape,
// WireConformance_RoleInfoAck_Shape, WireConformance_BandAck_Shapes, and
// WireConformance_Band_CorrIdEcho MIGRATED to
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
