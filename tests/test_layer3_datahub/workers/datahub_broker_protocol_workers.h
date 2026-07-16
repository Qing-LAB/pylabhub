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

// Checksum error forwarding — both MIGRATED to tests/test_layer3_pattern4/
// (forwarded_to_producer: Round 1; unknown_channel_silent: Round 3 via a
// wire-liveness probe).

// Closing notify fanout — RETIRED 2026-06-28: contract absorbed by
// task #225 (Pattern 4 rung 8 `Pattern4ChannelNotifiesTest`).  See
// driver file's retirement doc-block for full reasoning.

// Duplicate registration — MIGRATED to tests/test_layer3_pattern4/
// (task #54 Round 1).

// Heartbeat → Ready transition + keying.  transitions_to_ready +
// wire_payload MIGRATED to Pattern 4 (Round 3 — read the broker's
// first-heartbeat / producer-presence sub-Live traces).  keying stays
// here (distinct-row HubState inspection — needs presence-row logging).
int heartbeat_keying_producer_vs_consumer_distinct_rows();

// Role presence / info queries, Transport arbitration, and REG_ACK /
// CONSUMER_REG_ACK heartbeat-block tests MIGRATED to
// tests/test_layer3_pattern4/ (task #54 Round 1).

// CHANNEL_BROADCAST_SEND_NOTIFY fan-out.  delivered / data_payload /
// unknown_channel MIGRATED to tests/test_layer3_pattern4/ (task #54
// Round 1).  hub_queue_path stays here — in-process broadcast trigger
// (request_broadcast_channel) with no wire equivalent (Round 3 RATIONALE).
int broadcast_fan_out_hub_queue_path_fans_out_same();

// Audit TR1 — wire-conformance ACK-shape regressions (2026-05-17)
// reg_ack / consumer_reg_ack / role_info_ack / band_ack shapes +
// band_corr_id_echo MIGRATED to tests/test_layer3_pattern4/
// (task #54 Round 1 — HubHostBrokerHandle antipattern sweep).
// R3.6 retired — CHANNEL_NOTIFY_REQ wire path deleted (no caller anywhere).

} // namespace broker_protocol
} // namespace pylabhub::tests::worker
