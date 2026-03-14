#pragma once
// tests/test_layer3_datahub/workers/datahub_broker_consumer_workers.h
// Consumer registration protocol test worker declarations.

namespace pylabhub::tests::worker::broker_consumer
{

/** Pure ChannelRegistry consumer CRUD tests (no ZMQ, no lifecycle). */
int channel_registry_consumer_ops();

/** CONSUMER_REG_REQ for a channel that is not registered → ERROR CHANNEL_NOT_FOUND. */
int consumer_reg_channel_not_found();

/** Messenger register_consumer → CONSUMER_REG_ACK; DISC_ACK shows consumer_count=1. */
int consumer_reg_happy_path();

/** Register consumer then deregister with correct pid → CONSUMER_DEREG_ACK success. */
int consumer_dereg_happy_path();

/** Deregister with wrong pid → ERROR NOT_REGISTERED; consumer still registered. */
int consumer_dereg_pid_mismatch();

/** DISC_ACK consumer_count increments after register_consumer and decrements after deregister. */
int disc_shows_consumer_count();

} // namespace pylabhub::tests::worker::broker_consumer
