// tests/test_layer3_datahub/workers/datahub_recovery_scenario_workers.h
#pragma once

/**
 * Workers for recovery scenario tests (zombie detection, force-reset, dead consumer cleanup).
 *
 * These tests exercise the recovery API paths that require a "broken" shared memory state:
 * zombie writer (dead PID in write_lock), zombie readers (stale reader_count), and dead
 * consumer heartbeats.  State is injected via DiagnosticHandle after creation.
 *
 * Secrets start at 77001.
 */
namespace pylabhub::tests::worker::recovery_scenarios
{

/** Zombie writer (dead PID in write_lock) → release_zombie_writer succeeds. */
int zombie_writer_detected_and_released();
/** Zombie readers (reader_count > 0, no live write_lock) → release_zombie_readers succeeds. */
int zombie_readers_force_cleared();
/** Dead-writer slot in WRITING state → force_reset_slot succeeds without force flag. */
int force_reset_slot_on_dead_writer();
/** Heartbeat entry with dead consumer PID → cleanup_dead_consumers removes it. */
int dead_consumer_cleanup();
/** datablock_is_process_alive returns false for a nonexistent PID, true for self. */
int is_process_alive_false_for_nonexistent();
/** force_reset_slot returns RECOVERY_UNSAFE when writer is an alive process. */
int force_reset_unsafe_when_writer_alive();

} // namespace pylabhub::tests::worker::recovery_scenarios
