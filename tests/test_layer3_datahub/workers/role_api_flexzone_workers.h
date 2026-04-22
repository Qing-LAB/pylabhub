#pragma once
/**
 * @file role_api_flexzone_workers.h
 * @brief Workers for L3 role-API flexzone integration tests.
 *
 * Each body spawns a `RoleAPIBase` + real SHM queue(s) under a
 * `LifecycleGuard` owned by the worker subprocess via `run_gtest_worker`.
 * The flexzone round-trip test wires both producer and consumer
 * `RoleAPIBase` instances in the same process and verifies the TABLE 1
 * bidirectional shared region.
 */

namespace pylabhub::tests::worker::role_api_flexzone
{

/// T2 + T3 combined: producer writes sentinel to Tx flexzone, consumer
/// reads it back on Rx, then consumer writes ack, producer reads it —
/// verifies the shared physical region is bidirectional.
int shm_roundtrip();

/// ZMQ-only producer — api.flexzone(Tx) returns nullptr, size == 0,
/// tx_has_shm() == false.
int zmq_tx_null();

/// ZMQ-only consumer — api.flexzone(Rx) returns nullptr, size == 0,
/// rx_has_shm() == false.
int zmq_rx_null();

/// Round-trip + checksum: producer updates flexzone checksum after a
/// sentinel write, consumer verifies checksum matches (verify_checksum
/// must return true).
int shm_checksum_roundtrip();

/// Payload-depth: `padding_schema()` (float64 + uint8 + int32).  Producer
/// writes distinctive values at the layout-computed offsets; consumer
/// reads back bit-exact at the SAME computed offsets.  Catches layout
/// mismatch, partial overwrite, alignment gap corruption.
int shm_roundtrip_padding_sensitive();

/// Payload-depth: `all_types_schema()` — 13 fields covering every scalar
/// type (bool, int8/16/32/64, uint8/16/32/64, float32, float64, bytes,
/// string).  Producer writes type-distinctive sentinel values; consumer
/// reads back bit-exact.  Catches per-type offset or size miscomputation.
int shm_roundtrip_all_types();

/// Payload-depth: `fz_array_schema()` (uint32 + float64[2]).  Exercises
/// array-count + alignment padding between scalar and array.  Producer
/// writes `version=7, values=[1.5, 2.5]`; consumer reads back matching.
int shm_roundtrip_array_field();

// ── Negative paths — prove the error surface is live ─────────────────────────

/// Consumer attach with wrong shared_secret must fail; subsequent
/// attach with correct secret must succeed (no sticky corruption).
int shm_consumer_wrong_secret_rejected();

/// Consumer attach to nonexistent SHM segment must fail.
int shm_consumer_nonexistent_rejected();

/// Producer commits a slot then corrupts its payload in SHM; consumer
/// attached with Enforced checksum_policy must detect the mismatch via
/// read_acquire returning null OR checksum_error_count incrementing.
int shm_slot_checksum_corrupt_detected();

/// Symmetric twin for the flexzone-checksum path: producer writes flexzone,
/// updates its checksum, then corrupts the flexzone buffer.  Consumer with
/// flexzone_checksum enabled + Enforced policy must detect via read_acquire
/// returning null OR checksum_error_count incrementing.
int shm_flexzone_checksum_corrupt_detected();

} // namespace pylabhub::tests::worker::role_api_flexzone
