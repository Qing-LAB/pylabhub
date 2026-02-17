/**
 * @file test_c_api_slot_protocol.cpp
 * @brief C API slot protocol tests: write/read roundtrip, commit/abort, ring buffer
 *        policies, timeout behavior, and metrics.
 *
 * Each test spawns an isolated worker process that exercises DataBlockProducer/Consumer
 * directly via the C++ wrappers (no RAII templates, no schema validation).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubCApiSlotProtocolTest : public IsolatedProcessTest
{
};

// ─── Roundtrip ────────────────────────────────────────────────────────────────

TEST_F(DatahubCApiSlotProtocolTest, WriteSlotReadSlotRoundtrip)
{
    auto proc = SpawnWorker("c_api_slot_protocol.write_slot_read_slot_roundtrip", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── Metrics: commit vs abort ─────────────────────────────────────────────────

TEST_F(DatahubCApiSlotProtocolTest, CommitAdvancesMetrics)
{
    auto proc = SpawnWorker("c_api_slot_protocol.commit_advances_metrics", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiSlotProtocolTest, AbortDoesNotCommit)
{
    auto proc = SpawnWorker("c_api_slot_protocol.abort_does_not_commit", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── ConsumerSyncPolicy: Latest_only ─────────────────────────────────────────

TEST_F(DatahubCApiSlotProtocolTest, LatestOnlyReadsLatest)
{
    auto proc = SpawnWorker("c_api_slot_protocol.latest_only_reads_latest", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── ConsumerSyncPolicy: Single_reader ───────────────────────────────────────

TEST_F(DatahubCApiSlotProtocolTest, SingleReaderReadsSequentially)
{
    auto proc = SpawnWorker("c_api_slot_protocol.single_reader_reads_sequentially", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── Ring buffer full / empty ─────────────────────────────────────────────────

TEST_F(DatahubCApiSlotProtocolTest, WriteReturnsNullWhenRingFull)
{
    auto proc = SpawnWorker("c_api_slot_protocol.write_returns_null_when_ring_full", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiSlotProtocolTest, ReadReturnsNullOnEmptyRing)
{
    auto proc = SpawnWorker("c_api_slot_protocol.read_returns_null_on_empty_ring", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── Metrics accumulation ─────────────────────────────────────────────────────

TEST_F(DatahubCApiSlotProtocolTest, MetricsAccumulateAcrossWrites)
{
    auto proc = SpawnWorker("c_api_slot_protocol.metrics_accumulate_across_writes", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
