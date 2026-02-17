// tests/test_layer3_datahub/workers/policy_enforcement_workers.h
#pragma once

namespace pylabhub::tests::worker::policy_enforcement
{

// --- Checksum policy ---
int checksum_enforced_write_read_roundtrip();
int checksum_enforced_flexzone_only_write();
int checksum_enforced_verify_detects_corruption();
int checksum_none_skips_update_verify();
int checksum_manual_requires_explicit_call();

// --- Heartbeat (all policies) ---
int consumer_auto_registers_heartbeat_on_construction();
int consumer_auto_unregisters_heartbeat_on_destroy();
int all_policy_consumers_have_heartbeat();

// --- Sync_reader backpressure ---
int sync_reader_producer_respects_consumer_position();

// --- Auto-heartbeat in iterator ---
int producer_operator_increment_updates_heartbeat();
int consumer_operator_increment_updates_heartbeat();

} // namespace pylabhub::tests::worker::policy_enforcement
