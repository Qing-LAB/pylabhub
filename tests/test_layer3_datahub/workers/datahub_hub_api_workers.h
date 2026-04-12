#pragma once
// tests/test_layer3_datahub/workers/datahub_hub_api_workers.h
//
// Hub Producer/Consumer unified API tests (Phase: hub_api).

namespace pylabhub::tests::worker::hub_api
{

/** Producer::create(PubSub, no shm): is_valid, channel_name, close. */
int producer_create_pubsub(int argc, char **argv);

/** Producer::create(has_shm): synced_write(job) executes sync slot; push async. */
int producer_create_with_shm(int argc, char **argv);

/** Non-template create/connect (no schema); factory returns valid Producer/Consumer. */
int non_template_factory(int argc, char **argv);

/** ManagedProducer + LifecycleGuard: get() valid post-init; close() on teardown. */
int managed_producer_lifecycle(int argc, char **argv);

/** Producer push → consumer set_read_handler fires with correct data (active SHM path). */
int consumer_shm_read_e2e(int argc, char **argv);

/** Producer synced_write (sync) → Consumer pull (sync): data round-trip verified. */
int consumer_read_shm_sync(int argc, char **argv);

/** start()/stop()/close() called twice each is safe (idempotency invariant). */
int producer_consumer_idempotency(int argc, char **argv);

/** Producer with identity: hub_uid/producer_name written to SHM; C API returns correct values. */
int producer_channel_identity(int argc, char **argv);

/** Consumer with uid/name: heartbeat slot carries identity; list_consumers returns it; close clears. */
int consumer_identity_in_shm(int argc, char **argv);

// ── Queue abstraction forwarding API tests (Phase 4) ─────────────────────────

/** Producer::write_acquire/commit through forwarding API; Consumer reads back correct data. */
int producer_consumer_forwarding_api(int argc, char **argv);

/** Construction-time checksum: update_checksum in Options → Consumer verifies on read. */
int construction_time_checksum(int argc, char **argv);

/** Flexzone access through Producer/Consumer (DataBlock-direct path). */
int flexzone_through_service_layer(int argc, char **argv);

/** queue_metrics() returns valid data through Producer/Consumer forwarding. */
int queue_metrics_forwarding(int argc, char **argv);

/** ZMQ transport: write/read through Producer/Consumer forwarding; metrics; flexzone=null. */
int zmq_forwarding_api(int argc, char **argv);

/** Forwarding error paths: no queue (item_size=0) returns safe defaults. */
int forwarding_error_paths(int argc, char **argv);

/** Runtime set_verify_checksum toggle: disable→read OK, enable→checksum reject. */
int runtime_verify_checksum_toggle(int argc, char **argv);

/** Checksum Enforced: Producer stamps checksum, Consumer verifies — round-trip succeeds. */
int checksum_enforced_roundtrip(int argc, char **argv);
/** Checksum Manual no-stamp: Producer does not stamp, Consumer verifies — read_acquire returns nullptr. */
int checksum_manual_no_stamp_mismatch(int argc, char **argv);
/** Checksum None: No checksum at all — round-trip succeeds without checksumming. */
int checksum_none_roundtrip(int argc, char **argv);
/** open_inbox_client: full broker path with numeric-only schema — verifies item_size. */
int open_inbox_client_numeric_schema(int argc, char **argv);
/** SHM multi-field schema: Producer writes float64+int32+uint8, Consumer reads all fields. */
int shm_multifield_schema_roundtrip(int argc, char **argv);
/** ZMQ multi-field schema: same as SHM but through ZMQ transport. */
int zmq_multifield_schema_roundtrip(int argc, char **argv);

/** SHM packed: 6-field schema with packed packing — no alignment padding. */
int shm_multifield_packed_roundtrip(int argc, char **argv);
/** ZMQ packed: 6-field schema with packed packing through ZMQ wire. */
int zmq_multifield_packed_roundtrip(int argc, char **argv);
/** Checksum Enforced + complex 6-field schema: stamp + verify round-trip. */
int checksum_enforced_complex_schema(int argc, char **argv);

/** Spinlock via RoleAPIBase — producer holds lock, signals ready, holds 500ms. */
int spinlock_producer_hold(int argc, char **argv);
/** Spinlock via RoleAPIBase — consumer contends, verifies cross-process blocking. */
int spinlock_consumer_contend(int argc, char **argv);

} // namespace pylabhub::tests::worker::hub_api
