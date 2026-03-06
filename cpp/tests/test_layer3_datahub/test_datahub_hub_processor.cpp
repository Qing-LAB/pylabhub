/**
 * @file test_datahub_hub_processor.cpp
 * @brief Hub Processor unit tests.
 *
 * Tests hub::Processor — the demand-driven transform pipeline.
 * Each test spawns an isolated subprocess that sets up two ShmQueues (in/out),
 * creates a Processor, runs a scenario, and asserts the expected results.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubHubProcessorTest : public IsolatedProcessTest
{
};

TEST_F(DatahubHubProcessorTest, ProcessorCreate)
{
    // create() with two ShmQueues succeeds; is_running() false before start().
    auto proc = SpawnWorker("hub_processor.processor_create", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorNoHandler)
{
    // start() without a handler: in_received stays 0; no output produced.
    auto proc = SpawnWorker("hub_processor.processor_no_handler", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorSetHandler)
{
    // set_process_handler<>() → has_process_handler() true; nullptr → false.
    auto proc = SpawnWorker("hub_processor.processor_set_handler", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorTransform)
{
    // 3 input slots → handler doubles value → 3 output slots verified byte-for-byte.
    auto proc = SpawnWorker("hub_processor.processor_transform", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorDropFalse)
{
    // Handler returns false → out_drop_count() == 1; out_slots_written() == 0.
    auto proc = SpawnWorker("hub_processor.processor_drop_false", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorOverflowDrop)
{
    // OverflowPolicy::Drop: round-trip without deadlock; accounting correct.
    auto proc = SpawnWorker("hub_processor.processor_overflow_drop", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorOverflowBlock)
{
    // OverflowPolicy::Block: round-trip with active reader; no deadlock.
    auto proc = SpawnWorker("hub_processor.processor_overflow_block", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorCounters)
{
    // in_received / out_written / out_drops match alternating commit/drop handler.
    auto proc = SpawnWorker("hub_processor.processor_counters", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorStopClean)
{
    // start() → stop() → is_running() false; clean thread shutdown.
    auto proc = SpawnWorker("hub_processor.processor_stop_clean", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorClose)
{
    // close() → is_running() false; idempotent.
    auto proc = SpawnWorker("hub_processor.processor_close", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorHandlerHotSwap)
{
    // Start with handler A (doubles); swap to handler B (triples); verify B takes effect.
    auto proc = SpawnWorker("hub_processor.processor_handler_hot_swap", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ProcessorHandlerRemoval)
{
    // set_process_handler(nullptr) while running; no crash; re-install works.
    auto proc = SpawnWorker("hub_processor.processor_handler_removal", {});
    ExpectWorkerOk(proc);
}

// ── Part 0: Enhanced Processor features ──────────────────────────────────────

TEST_F(DatahubHubProcessorTest, TimeoutHandler_ProducesOutput)
{
    // No input → timeout handler called → output committed.
    auto proc = SpawnWorker("hub_processor.timeout_handler_produces_output", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, TimeoutHandler_NullOutputOnDrop)
{
    // Drop mode + full output → handler gets nullptr out_data.
    auto proc = SpawnWorker("hub_processor.timeout_handler_null_output_on_drop", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, IterationCount_AdvancesOnTimeout)
{
    // iteration_count > in_slots_received when timeouts occur.
    auto proc = SpawnWorker("hub_processor.iteration_count_advances_on_timeout", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, CriticalError_StopsLoop)
{
    // Handler calls set_critical_error → loop exits → reason readable.
    auto proc = SpawnWorker("hub_processor.critical_error_stops_loop", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, CriticalError_FromTimeoutHandler)
{
    // Timeout handler triggers critical error.
    auto proc = SpawnWorker("hub_processor.critical_error_from_timeout_handler", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, PreHook_CalledBeforeHandler)
{
    // Pre-hook increments counter; verified == handler call count.
    auto proc = SpawnWorker("hub_processor.pre_hook_called_before_handler", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, PreHook_CalledBeforeTimeout)
{
    // Pre-hook also fires on timeout path.
    auto proc = SpawnWorker("hub_processor.pre_hook_called_before_timeout", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ZeroFill_OutputZeroed)
{
    // zero_fill_output=true → output bytes are 0 before handler writes.
    auto proc = SpawnWorker("hub_processor.zero_fill_output_zeroed", {});
    ExpectWorkerOk(proc);
}

// ── ZmqQueue integration tests ──────────────────────────────────────────────

TEST_F(DatahubHubProcessorTest, ZmqQueue_Roundtrip)
{
    // Processor with ZmqQueue in + out, data transforms correctly.
    auto proc = SpawnWorker("hub_processor.zmq_queue_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ZmqQueue_NullFlexzone)
{
    // Handler receives nullptr flexzone, no crash.
    auto proc = SpawnWorker("hub_processor.zmq_queue_null_flexzone", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ZmqQueue_TimeoutHandler)
{
    // Timeout handler works with ZmqQueue transport.
    auto proc = SpawnWorker("hub_processor.zmq_queue_timeout_handler", {});
    ExpectWorkerOk(proc);
}

// ── Mixed transport tests ────────────────────────────────────────────────────

TEST_F(DatahubHubProcessorTest, ShmInZmqOut)
{
    // ShmQueue(read) → Processor → ZmqQueue(write): mixed transport bridge.
    auto proc = SpawnWorker("hub_processor.shm_in_zmq_out", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubHubProcessorTest, ZmqInShmOut)
{
    // ZmqQueue(read) → Processor → ShmQueue(write): mixed transport bridge.
    auto proc = SpawnWorker("hub_processor.zmq_in_shm_out", {});
    ExpectWorkerOk(proc);
}
