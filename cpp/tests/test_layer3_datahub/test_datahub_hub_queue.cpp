/**
 * @file test_datahub_hub_queue.cpp
 * @brief Hub ShmQueue unit tests.
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
    // from_consumer(): DataBlockConsumer → ShmQueue; name/item_size/flexzone_size OK.
    auto proc = SpawnWorker("hub_queue.shm_queue_from_consumer", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubShmQueueTest, ShmQueueFromProducer)
{
    // from_producer(): DataBlockProducer → ShmQueue; name/item_size/flexzone_size OK.
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
    // write_acquire() + write_abort() does not commit; consumer sees timeout.
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
