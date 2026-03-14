#pragma once
// tests/test_layer3_datahub/workers/datahub_broker_workers.h
// Phase C — BrokerService integration test worker declarations.

namespace pylabhub::tests::worker::broker
{

/** Pure ChannelRegistry unit tests (no ZMQ, no lifecycle). */
int channel_registry_ops();

/** Register a channel via Messenger, discover it back — full REG/DISC round-trip. */
int broker_reg_disc_happy_path();

/** Re-register same channel with different schema_hash → broker replies SCHEMA_MISMATCH. */
int broker_schema_mismatch();

/** Discover a channel that was never registered → Messenger returns nullopt. */
int broker_channel_not_found();

/** Register, deregister (correct pid), discover → nullopt (CHANNEL_NOT_FOUND). */
int broker_dereg_happy_path();

/** Deregister with wrong pid → broker replies NOT_REGISTERED; channel still discoverable. */
int broker_dereg_pid_mismatch();

} // namespace pylabhub::tests::worker::broker
