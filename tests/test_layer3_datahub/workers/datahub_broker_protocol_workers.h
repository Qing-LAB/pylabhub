#pragma once
/**
 * @file datahub_broker_protocol_workers.h
 * @brief Worker bodies for broker control-plane protocol tests
 *        (Pattern 3).  Migrated 2026-05-14 from in-process
 *        `SetUpTestSuite`-owned `LifecycleGuard`.
 */

namespace pylabhub::tests::worker
{
namespace broker_protocol
{

// Checksum error forwarding
int checksum_error_report_forwarded_to_producer();
int checksum_error_report_unknown_channel_silent();

// Closing notify fanout — RETIRED 2026-06-28: contract absorbed by
// task #225 (Pattern 4 rung 8 `Pattern4ChannelNotifiesTest`).  See
// driver file's retirement doc-block for full reasoning.

// Duplicate registration
int duplicate_reg_shm_cardinality();
int duplicate_reg_different_schema_hash();

// Heartbeat → Ready transition + wire payload
int heartbeat_transitions_to_ready();
int heartbeat_wire_payload_includes_uid_and_role_type();
int heartbeat_keying_producer_vs_consumer_distinct_rows();

// Role presence / info queries
int role_presence_req_unknown_uid();
int role_info_req_unknown_uid();
int role_presence_req_producer_uid();
int role_presence_req_consumer_uid();
int role_info_req_with_inbox();

// Transport arbitration
int transport_mismatch_shm_producer_zmq_consumer();
int transport_match_shm_consumer_shm_producer();
int transport_match_no_driver_field();

// REG_ACK heartbeat-negotiation block
int reg_ack_contains_heartbeat_block_defaults();
int reg_ack_heartbeat_block_honors_custom_config();
int consumer_reg_ack_contains_heartbeat_block();

// CHANNEL_BROADCAST_SEND_NOTIFY fan-out
int broadcast_fan_out_delivered_to_producer_and_consumers();
int broadcast_fan_out_data_payload_round_trip();
int broadcast_unknown_channel_no_notify_delivered();
int broadcast_fan_out_hub_queue_path_fans_out_same();

// Audit TR1 — wire-conformance ACK-shape regressions (2026-05-17)
int wire_conformance_reg_ack_shape();
int wire_conformance_consumer_reg_ack_shape();
int wire_conformance_role_info_ack_shape();
int wire_conformance_band_ack_shapes();
// wire_conformance_band_corr_id_echo migrated to
// tests/test_layer3_pattern4/ (task #54 Round 1).
// R3.6 retired — CHANNEL_NOTIFY_REQ wire path deleted (no caller anywhere).

} // namespace broker_protocol
} // namespace pylabhub::tests::worker
