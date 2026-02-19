#pragma once
// tests/test_layer3_datahub/workers/datahub_e2e_workers.h
// End-to-end multi-process integration test worker declarations.
//
// The E2E test exercises the full pipeline: real broker (in-thread) + real producer process
// + real consumer process exchanging live DataBlock data.

namespace pylabhub::tests::worker::e2e
{

/**
 * Orchestrator: starts broker in-thread, spawns producer and consumer sub-workers,
 * coordinates ready-signal handoff, verifies both succeed.
 */
int orchestrator(int argc, char** argv);

/**
 * Producer sub-worker: creates DataBlock, registers with broker, writes 5 slots,
 * signals ready, sleeps 5s to keep shm alive, then cleans up.
 *
 * argv[2] = broker endpoint
 * argv[3] = broker server public key
 * argv[4] = channel name
 */
int e2e_producer(int argc, char** argv);

/**
 * Consumer sub-worker: connects to broker, discovers channel, attaches to DataBlock,
 * registers as consumer, reads and verifies latest slot, then deregisters.
 *
 * argv[2] = broker endpoint
 * argv[3] = broker server public key
 * argv[4] = channel name
 */
int e2e_consumer(int argc, char** argv);

} // namespace pylabhub::tests::worker::e2e
