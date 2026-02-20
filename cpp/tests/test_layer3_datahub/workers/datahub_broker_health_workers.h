#pragma once
// tests/test_layer3_datahub/workers/datahub_broker_health_workers.h
//
// Broker/Producer/Consumer health and notification tests.

namespace pylabhub::tests::worker::broker_health
{

/** Broker sends CHANNEL_CLOSING_NOTIFY to producer on heartbeat timeout (Cat 1). */
int producer_gets_closing_notify(int argc, char **argv);

/** Consumer::close() sends CONSUMER_DEREG_REQ; broker consumer_count drops to 0. */
int consumer_auto_deregisters(int argc, char **argv);

/** Producer::close() sends DEREG_REQ; same channel re-created immediately (no timeout). */
int producer_auto_deregisters(int argc, char **argv);

/**
 * Multi-process: orchestrator side.
 * Starts broker (liveness_check=1s), creates producer, writes endpoint+pubkey to temp file,
 * signals ready, waits for consumer to connect, then waits for on_consumer_died to fire.
 * argv[2] = temp file path.
 */
int dead_consumer_orchestrator(int argc, char **argv);

/**
 * Multi-process: consumer-exiter side.
 * Reads endpoint+pubkey from temp file, connects consumer, then calls _exit(0) to
 * simulate a crashed process without clean deregistration.
 * argv[2] = temp file path.
 */
int dead_consumer_exiter(int argc, char **argv);

/**
 * Single worker with two Messenger instances.
 * Producer A creates channel with schema_hash A; registers on_channel_error.
 * A second (manual) Messenger tries to create same channel with schema_hash B.
 * Broker rejects the second registration and sends CHANNEL_ERROR_NOTIFY to producer A.
 */
int schema_mismatch_notify(int argc, char **argv);

} // namespace pylabhub::tests::worker::broker_health
