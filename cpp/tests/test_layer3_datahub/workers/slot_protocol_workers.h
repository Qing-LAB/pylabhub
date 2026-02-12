// tests/test_layer3_datahub/workers/slot_protocol_workers.h
#pragma once

namespace pylabhub::tests::worker::slot_protocol
{

int write_read_succeeds_in_process();
int structured_slot_data_passes();
int checksum_update_verify_succeeds();
int layout_with_checksum_and_flexible_zone_succeeds();
int diagnostic_handle_opens_and_accesses_header();

/** Iterate ring-buffer units: write N distinct frames, read N and verify content per slot. */
int ring_buffer_iteration_content_verified();

/** Deliberate contention: reader holds slot, writer blocks (timeout), then unblocks when reader releases. */
int writer_blocks_on_reader_then_unblocks();

/** Cross-process: writer only. Expects channel in argv[2]. Does not cleanup. */
int cross_process_writer(int argc, char **argv);
/** Cross-process: reader only. Expects channel in argv[2]. Verifies content then cleanup. */
int cross_process_reader(int argc, char **argv);

/** High-contention: writer blocks when ring full and readers hold, unblocks when one reader releases. */
int high_contention_wrap_around();
/** Zombie writer: acquires write slot then _exit(0). Expects channel in argv[2]. POSIX only. */
int zombie_writer_acquire_then_exit(int argc, char **argv);
/** Reclaimer: after zombie exited, acquire_write_slot succeeds via force reclaim. Expects channel in argv[2]. */
int zombie_writer_reclaimer(int argc, char **argv);

/** ConsumerSyncPolicy tests: explicit policy and ordered read / backpressure. */
int policy_latest_only();
int policy_single_reader();
int policy_sync_reader();

/** High-load integrity test for Single_reader policy (many write/read cycles). */
int high_load_single_reader();

} // namespace pylabhub::tests::worker::slot_protocol
