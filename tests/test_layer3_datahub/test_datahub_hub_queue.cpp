/**
 * @file test_datahub_hub_queue.cpp
 * @brief Hub ShmQueue unit tests — RETIRED 2026-06-30 (#275-S3b).
 *
 * RETIRED 2026-06-30 (HEP-CORE-0041 1i-cleanup S3b, #275) per Rule 6.
 * The 25 TEST_F's below exercised ShmQueue scenarios through the
 * legacy `ShmQueue::create_writer(name, ..., shared_secret, ...)` +
 * `ShmQueue::create_reader(name, shared_secret, ...)` factories that
 * are deleted in S3c.  The capability-transport path
 * (`create_writer_standby` + `set_shm_capability_fd` + `start`) is
 * the surviving production API and has no per-attach secret.
 *
 * Coverage continuity — each retired scenario maps to surviving cover:
 *   - ShmQueue state-machine + mutual-exclusion guards + invalid-fd guard
 *     → L2 `test_hub_shm_queue_capability.cpp` (Tests 1-7).
 *   - DataBlock ring-buffer + slot-allocation + ring-wrap + draining
 *     + last_seq + checksum + discard/reacquire scenarios
 *     → L3 fd-source workers `datahub_slot_allocation_workers.cpp`,
 *       `datahub_c_api_draining_workers.cpp`,
 *       `datahub_c_api_slot_protocol_workers.cpp` (all migrated
 *       under #275-S2 to `make_fd_backed_pair`).
 *   - End-to-end producer→consumer SHM data flow + flexzone
 *     bidirectional + multiple-consumer state isolation
 *     → L4 `test_plh_hub_role_shm_e2e.cpp` (#258).
 *   - Error-path: empty schema / zero-size schema / nonexistent
 *     attach → covered by L2 capability test's state-machine
 *     refusals + the surviving production-path WARN/ERROR contracts
 *     that AUTH-7 L4 e2e (#258) exercises.
 *
 * Full retirement entry with per-test contract-handoff mapping:
 *   `docs/todo/TESTING_TODO.md` § "Test retirements / cross-layer
 *   migrations", row dated 2026-06-30.
 *
 * File stays on disk for git history; CMakeLists.txt masks it
 * from the L3 aggregate.  REVIEW-C (#276) will delete the file
 * after confirming zero downstream references.
 *
 * Tests hub::ShmQueue — the shared-memory-backed Queue implementation.
 * Each test spawns an isolated subprocess that creates DataBlock objects directly
 * (no broker needed) and exercises the ShmQueue interface.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubShmQueueTest : public IsolatedProcessTest
{
};

TEST_F(DatahubShmQueueTest, ShmQueueFromConsumer)
{
    // create_reader(): schema → ShmQueue read mode; name/item_size OK.
    auto proc = SpawnWorker("hub_queue.shm_queue_from_consumer", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueFromProducer)
{
    // create_writer(): schema → ShmQueue write mode; name/item_size OK.
    auto proc = SpawnWorker("hub_queue.shm_queue_from_producer", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueReadAcquireTimeout)
{
    // read_acquire(50ms) on empty ring returns nullptr.
    auto proc = SpawnWorker("hub_queue.shm_queue_read_acquire_timeout", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueWriteAcquireCommit)
{
    // write_acquire() + fill + write_commit() must not crash or assert.
    auto proc = SpawnWorker("hub_queue.shm_queue_write_acquire_commit", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueWriteAcquireAbort)
{
    // write_acquire() + write_discard() does not commit; consumer sees timeout.
    auto proc = SpawnWorker("hub_queue.shm_queue_write_acquire_abort", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueReadFlexzone)
{
    // flexzone_sz > 0: read_flexzone() returns non-null pointer.
    auto proc = SpawnWorker("hub_queue.shm_queue_read_flexzone", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueWriteFlexzone)
{
    // flexzone_sz > 0: write_flexzone() returns non-null pointer.
    //
    // NOTE (2026-03-16): This test intermittently timed out at 60s under
    // ctest -j2, yet passes instantly in isolation (~0.06s).  The worker logic
    // is entirely non-blocking (create producer, call write_flexzone, assert),
    // with a unique shared_secret (70007) and nanosecond-timestamped channel
    // name — no SHM contention is possible.  Likely root cause: uncapped
    // ThreePhaseBackoff Phase 3 (iteration * 10us with no ceiling) could grow
    // to multi-second sleeps if SharedSpinLock contention occurred during
    // LifecycleGuard shutdown under parallel load.  Fixed by capping Phase 3
    // at 10ms (kMaxPhase3DelayUs, commit 1d3e584).  If this recurs after the
    // cap fix, investigate Logger cv_.notify_one miss or fork scheduling.
    auto proc = SpawnWorker("hub_queue.shm_queue_write_flexzone", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueNoFlexzone)
{
    // flexzone_sz = 0: read_flexzone() and write_flexzone() return nullptr.
    auto proc = SpawnWorker("hub_queue.shm_queue_no_flexzone", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueRoundTrip)
{
    // Writer ShmQueue writes 3 slots; reader ShmQueue reads and verifies byte-for-byte.
    auto proc = SpawnWorker("hub_queue.shm_queue_round_trip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueMultipleConsumers)
{
    // Two from_consumer_ref readers on the same DataBlock each independently see the
    // written slot — ConsumerSyncPolicy::Latest_only per-consumer state is isolated.
    auto proc = SpawnWorker("hub_queue.shm_queue_multiple_consumers", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueFlexzoneRoundTrip)
{
    // Write known bytes to write_flexzone(); commit a slot; consumer reads the slot and
    // verifies read_flexzone() returns matching bytes.
    auto proc = SpawnWorker("hub_queue.shm_queue_flexzone_round_trip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueFlexzoneBidirectional)
{
    // Per HEP-CORE-0002 §2.2 the flexzone is a single shared region per
    // channel, fully read+write on every endpoint. Writer→reader AND
    // reader→writer visibility both exercised through the unified
    // QueueReader/QueueWriter::flexzone() accessor.
    auto proc = SpawnWorker("hub_queue.shm_queue_flexzone_bidirectional", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueCreateFactories)
{
    // create_writer / create_reader owning factories: write a slot, read it back.
    auto proc = SpawnWorker("hub_queue.shm_queue_create_factories", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueLatestOnly)
{
    // Latest_only policy: write 3 slots without reading, then read once — returns the
    // last written value, not the first.
    auto proc = SpawnWorker("hub_queue.shm_queue_latest_only", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueRingWrap)
{
    // Write 10 items into a 2-slot ring (forces multiple wraps); consumer reads the
    // latest value correctly after the ring has overwritten old slots.
    auto proc = SpawnWorker("hub_queue.shm_queue_ring_wrap", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueDestructorSafety)
{
    // Create reader and writer ShmQueues and immediately destroy them without any
    // acquire/release — must not crash or assert.
    auto proc = SpawnWorker("hub_queue.shm_queue_destructor_safety", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueLastSeq)
{
    // last_seq() starts at 0 and increases monotonically across 3 write/read cycles.
    auto proc = SpawnWorker("hub_queue.shm_queue_last_seq", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueCapacityPolicy)
{
    // capacity() returns the ring_buffer_capacity from the DataBlockConfig;
    // policy_info() returns "shm_read" / "shm_write".
    auto proc = SpawnWorker("hub_queue.shm_queue_capacity_policy", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueVerifyChecksumMismatch)
{
    // Slot written without set_checksum_options → slot_valid flag stays 0.
    // read_acquire() with set_verify_checksum(true,false) must return nullptr (logs ERROR).
    // After enabling set_checksum_options on the writer, read_acquire() succeeds.
    auto proc = SpawnWorker("hub_queue.shm_queue_verify_checksum_mismatch", {});
    ExpectWorkerOk(proc, {}, {"slot checksum error"});
}

TEST_F(DatahubShmQueueTest, ShmQueueIsRunning)
{
    // is_running() returns true after construction and false on the moved-from instance.
    auto proc = SpawnWorker("hub_queue.shm_queue_is_running", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, DataBlockProducerRemapStubsThrow)
{
    // request_structure_remap() and commit_structure_remap() always throw std::runtime_error.
    auto proc = SpawnWorker("hub_queue.datablock_producer_remap_stubs_throw", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, DataBlockConsumerRemapStubsThrow)
{
    // release_for_remap() and reattach_after_remap() always throw std::runtime_error.
    auto proc = SpawnWorker("hub_queue.datablock_consumer_remap_stubs_throw", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueDiscardThenReacquire)
{
    // Regression: discard > capacity slots under Sequential policy, then
    // write_acquire + write_commit must still succeed (write_index restored).
    auto proc = SpawnWorker("hub_queue.shm_queue_discard_then_reacquire", {});
    ExpectWorkerOk(proc);
}

// ── Error path tests ─────────────────────────────────────────────────────────

TEST_F(DatahubShmQueueTest, CreateWriterEmptySchema)
{
    // create_writer with empty schema must return nullptr.
    auto proc = SpawnWorker("hub_queue.shm_queue_create_writer_empty_schema", {});
    ExpectWorkerOk(proc, {}, {"slot_schema is empty"});
}

// 2026-06-23 (#275-S2): `CreateReaderWrongSecret` RETIRED.
// The legacy `ShmQueue::create_reader(name, secret, ...)` factory it
// drove is removed under HEP-CORE-0041 1i-cleanup #275-S3.  The
// capability path replacing it has no per-attach secret.  See
// retirement doc-block in `workers/datahub_hub_queue_workers.cpp`.
// L2 capability-path coverage:
// `test_hub_shm_queue_capability.cpp::SetCapabilityFd_RefusesNegativeFd`
// (invalid-fd guard) + Tests 3-5 (state-machine refusals).

TEST_F(DatahubShmQueueTest, CreateReaderNonexistent)
{
    // create_reader for nonexistent SHM segment must return nullptr.
    auto proc = SpawnWorker("hub_queue.shm_queue_create_reader_nonexistent", {});
    ExpectWorkerOk(proc, {}, {"attachment failed"});
}

TEST_F(DatahubShmQueueTest, CreateWriterZeroSizeSchema)
{
    // Bypass parser: SchemaFieldDesc with bytes length=0 → item_size=0 → nullptr.
    auto proc = SpawnWorker("hub_queue.shm_queue_create_writer_zero_size_schema", {});
    ExpectWorkerOk(proc, {}, {"computed slot size is 0"});
}
