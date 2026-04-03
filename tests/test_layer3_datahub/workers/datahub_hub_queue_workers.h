#pragma once
// tests/test_layer3_datahub/workers/datahub_hub_queue_workers.h
//
// Worker functions for hub::ShmQueue unit tests.
// Each function is a self-contained test scenario run in an isolated subprocess.

namespace pylabhub::tests::worker::hub_queue
{

/** create_reader(): creates ShmQueue in read mode, checks name(). */
int shm_queue_from_consumer();
/** create_writer(): creates ShmQueue in write mode, checks name(). */
int shm_queue_from_producer();
/** read_acquire() with 50ms timeout on empty ring → nullptr. */
int shm_queue_read_acquire_timeout();
/** write_acquire() + fill buffer + write_commit() succeeds (no error). */
int shm_queue_write_acquire_commit();
/** write_acquire() + write_discard() succeeds without committing. */
int shm_queue_write_acquire_abort();
/** flexzone_sz > 0: read_flexzone() returns non-null pointer. */
int shm_queue_read_flexzone();
/** flexzone_sz > 0: write_flexzone() returns non-null pointer. */
int shm_queue_write_flexzone();
/** flexzone_sz = 0: read_flexzone() and write_flexzone() return nullptr. */
int shm_queue_no_flexzone();
/** Round-trip: write-mode ShmQueue writes 3 slots; read-mode ShmQueue reads and verifies. */
int shm_queue_round_trip();

/** Two create_reader readers on the same DataBlock each see the written slot. */
int shm_queue_multiple_consumers();
/** Write data to flexzone and slot; consumer reads back both via read_flexzone(). */
int shm_queue_flexzone_round_trip();
/** create_writer + create_reader: owning factories; write a slot, read it back. */
int shm_queue_create_factories();
/** Latest_only policy: write 3 slots without reading, then read once → last slot value. */
int shm_queue_latest_only();
/** Ring wrap: write capacity+1 items into a 2-slot ring; final read returns the latest value. */
int shm_queue_ring_wrap();
/** Destructor safety: create and immediately destroy reader/writer without any acquire. */
int shm_queue_destructor_safety();
/** last_seq() advances monotonically from 0 across 3 interleaved write/read cycles. */
int shm_queue_last_seq();
/** capacity() returns ring_buffer_capacity; policy_info() returns "shm_read" / "shm_write". */
int shm_queue_capacity_policy();
/** set_verify_checksum: slot written WITHOUT checksum update → read_acquire returns nullptr. */
int shm_queue_verify_checksum_mismatch();

/** is_running(): true after construction, false after move, true on move target. */
int shm_queue_is_running();
/** request_structure_remap() and commit_structure_remap() both throw std::runtime_error. */
int datablock_producer_remap_stubs_throw();
/** release_for_remap() and reattach_after_remap() both throw std::runtime_error. */
int datablock_consumer_remap_stubs_throw();

/** Discard > capacity slots under Sequential policy, then write_acquire+commit succeeds. */
int shm_queue_discard_then_reacquire();

// ── Error path tests ─────────────────────────────────────────────────────────

/** create_writer with empty schema → returns nullptr. */
int shm_queue_create_writer_empty_schema();
/** create_reader with wrong shared secret → returns nullptr. */
int shm_queue_create_reader_wrong_secret();
/** create_reader for nonexistent SHM segment → returns nullptr. */
int shm_queue_create_reader_nonexistent();

} // namespace pylabhub::tests::worker::hub_queue
