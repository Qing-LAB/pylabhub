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
/** write_acquire() + write_abort() succeeds without committing. */
int shm_queue_write_acquire_abort();
/** flexzone_sz > 0: read_flexzone() returns non-null pointer. */
int shm_queue_read_flexzone();
/** flexzone_sz > 0: write_flexzone() returns non-null pointer. */
int shm_queue_write_flexzone();
/** flexzone_sz = 0: read_flexzone() and write_flexzone() return nullptr. */
int shm_queue_no_flexzone();
/** Round-trip: write-mode ShmQueue writes 3 slots; read-mode ShmQueue reads and verifies. */
int shm_queue_round_trip();

} // namespace pylabhub::tests::worker::hub_queue
