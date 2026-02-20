#pragma once
// tests/test_layer3_datahub/workers/datahub_stress_raii_workers.h
//
// RAII layer multi-process stress tests: full-capacity ring buffer, deterministic
// payload patterns, enforced BLAKE2b checksums, and random read/write timing to
// exercise scheduling races.
//
// Two test scenarios:
//
//   1. MultiProcessFullCapacityStress (Latest_only):
//      Producer writes kNumSlots × 4096-byte slots with random 0–5 ms delays.
//      Two consumers read concurrently with random 0–10 ms delays.
//      Ring capacity = 32 slots ⇒ ≈15 full wraparounds.
//      Verifies: BLAKE2b (enforced), app-level byte-pattern, monotone sequence.
//
//   2. SingleReaderBackpressure (Single_reader):
//      Producer writes kNumSlotsBP slots; ring capacity = 8.
//      Consumer adds 0–20 ms random delays, forcing producer to block.
//      Verifies: ALL slots delivered in exact order, zero checksum failures.

namespace pylabhub::tests::worker::stress_raii
{

/**
 * Multi-process stress orchestrator (Latest_only).
 * Spawns one producer + two consumer sub-workers; waits for all; checks results.
 * argv[2] = channel_name (pre-computed by the GTest orchestrator).
 */
int multi_process_stress_orchestrator(int argc, char **argv);

/**
 * Producer sub-worker: creates DataBlock, signals ready, writes kNumSlots × 4096-byte
 * slots with random 0–5 ms inter-write delays.
 * argv[2] = channel_name
 */
int stress_producer(int argc, char **argv);

/**
 * Consumer sub-worker (Latest_only): attaches, reads until it sees the terminal
 * sequence, validates every slot. Random 0–10 ms inter-read delays.
 * argv[2] = channel_name
 * argv[3] = consumer index ("0" or "1") — used only in log messages
 */
int stress_consumer(int argc, char **argv);

/**
 * Single-reader back-pressure orchestrator.
 * Spawns one producer + one consumer (Single_reader); waits for both; checks results.
 * argv[2] = channel_name
 */
int backpressure_orchestrator(int argc, char **argv);

/**
 * Back-pressure producer sub-worker (Single_reader): writes kNumSlotsBP slots;
 * blocks when ring is full; random 0–5 ms delays.
 * argv[2] = channel_name
 */
int backpressure_producer(int argc, char **argv);

/**
 * Back-pressure consumer sub-worker (Single_reader): reads exactly kNumSlotsBP slots
 * in strict order with random 0–20 ms delays, verifying every byte and checksum.
 * argv[2] = channel_name
 */
int backpressure_consumer(int argc, char **argv);

} // namespace pylabhub::tests::worker::stress_raii
