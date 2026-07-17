/**
 * @file test_datahub_slot_allocation.cpp
 * @brief DataBlock slot-allocation tests: schema-size → cache-line rounding,
 *        ring-capacity → total_block_size scaling, and write/read roundtrip.
 *
 * Each test spawns an isolated worker process that builds fd-source
 * producers/pairs (`make_fd_backed_pair`) and asserts the HEP-CORE-0002 layout
 * invariants directly against the mapped SharedMemoryHeader.  This suite is the
 * fd-source destination ② for the retired ShmQueue slot-allocation coverage
 * (#275-S2; see docs/todo/TESTING_TODO.md retirement row for
 * `test_datahub_hub_queue.cpp`).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubSlotAllocationTest : public IsolatedProcessTest
{
};

// Schema sizes 1B→100KB: logical_unit_size cache-line-rounded, capacity stored
// verbatim, total_block_size >= capacity × slot stride, slot span == stride.
TEST_F(DatahubSlotAllocationTest, VariedSchemaSizesAllocation)
{
    auto proc = SpawnWorker("slot_allocation.varied_schema_sizes_allocation", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// Ring capacities 1→1000: capacity stored verbatim, total_block_size scales.
TEST_F(DatahubSlotAllocationTest, RingBufferScaling)
{
    auto proc = SpawnWorker("slot_allocation.ring_buffer_scaling", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// Write known byte patterns into slots of varying sizes, read back through a
// consumer, and verify byte-for-byte integrity across the allocated stride.
TEST_F(DatahubSlotAllocationTest, WriteReadRoundtripVariedSizes)
{
    auto proc = SpawnWorker("slot_allocation.write_read_roundtrip_varied_sizes", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
