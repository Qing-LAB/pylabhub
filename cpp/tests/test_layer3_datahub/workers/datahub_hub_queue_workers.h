#pragma once
// tests/test_layer3_datahub/workers/datahub_hub_queue_workers.h
//
// Worker functions for hub::ShmQueue unit tests.
// Each function is a self-contained test scenario run in an isolated subprocess.

namespace pylabhub::tests::worker::hub_queue
{

/** from_consumer(): creates a DataBlockConsumer, wraps in ShmQueue, checks name(). */
int shm_queue_from_consumer();
/** from_producer(): creates a DataBlockProducer, wraps in ShmQueue, checks name(). */
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

/** Two from_consumer_ref readers on the same DataBlock each see the written slot. */
int shm_queue_multiple_consumers();
/** Write data to flexzone and slot; consumer reads back both via read_flexzone(). */
int shm_queue_flexzone_round_trip();
/** from_producer_ref + from_consumer_ref: non-owning factories; underlying objects survive queue teardown. */
int shm_queue_ref_factories();
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

} // namespace pylabhub::tests::worker::hub_queue
