#pragma once
// tests/test_layer3_datahub/workers/datahub_hub_processor_workers.h
//
// Worker functions for hub::Processor unit tests.
// Each function is a self-contained test scenario run in an isolated subprocess.

namespace pylabhub::tests::worker::hub_processor
{

/** create(): two ShmQueues → Processor::create() succeeds; not yet running. */
int processor_create();
/** start() without a handler: runs; in_received stays 0; no output. */
int processor_no_handler();
/** set_process_handler<>() → has_process_handler() true; clear → false. */
int processor_set_handler();
/** 3 input slots → handler doubles value → 3 output slots verified byte-for-byte. */
int processor_transform();
/** Handler returns false → out_drop_count() == 1; out_slots_written() == 0. */
int processor_drop_false();
/** OverflowPolicy::Drop: normal round-trip; drop accounting correct. */
int processor_overflow_drop();
/** OverflowPolicy::Block: round-trip with reader; no deadlock. */
int processor_overflow_block();
/** in_received / out_written / out_drops match alternating commit/drop handler. */
int processor_counters();
/** start() → stop() → is_running() false; clean shutdown. */
int processor_stop_clean();
/** close() → is_running() false; releases resources. */
int processor_close();
/** start with handler A; swap to handler B mid-stream; verify B takes effect. */
int processor_handler_hot_swap();
/** set_process_handler(nullptr) while running; no crash; re-install works. */
int processor_handler_removal();

// ── Part 0: Enhanced Processor features ──────────────────────────────────────

/** No input → timeout handler called → output committed. */
int timeout_handler_produces_output();
/** Drop mode + full output → handler gets nullptr out_data. */
int timeout_handler_null_output_on_drop();
/** iteration_count > in_slots_received when timeouts occur. */
int iteration_count_advances_on_timeout();
/** Handler calls set_critical_error → loop exits → reason readable. */
int critical_error_stops_loop();
/** Timeout handler triggers critical error. */
int critical_error_from_timeout_handler();
/** Pre-hook increments counter; verified == handler call count. */
int pre_hook_called_before_handler();
/** Pre-hook also fires on timeout path. */
int pre_hook_called_before_timeout();
/** zero_fill_output=true → output bytes are 0 before handler writes. */
int zero_fill_output_zeroed();

// ── ZmqQueue integration tests ──────────────────────────────────────────────

/** Processor with ZmqQueue in + out, data transforms correctly. */
int zmq_queue_roundtrip();
/** Handler receives nullptr flexzone, no crash. */
int zmq_queue_null_flexzone();
/** Timeout handler works with ZmqQueue transport. */
int zmq_queue_timeout_handler();
/** ShmQueue(read) → Processor → ZmqQueue(write): mixed transport. */
int shm_in_zmq_out();
/** ZmqQueue(read) → Processor → ShmQueue(write): mixed transport. */
int zmq_in_shm_out();

} // namespace pylabhub::tests::worker::hub_processor
