// tests/test_layer3_datahub/workers/error_handling_workers.h
#pragma once

/**
 * Workers that exercise DataBlock/slot error paths and recoverable failure modes.
 * Ensures we return false / nullptr / empty instead of segfaulting when preconditions fail.
 */
namespace pylabhub::tests::worker::error_handling
{

/** Consumer acquire_consume_slot times out when no producer has committed → nullptr. */
int acquire_consume_slot_timeout_returns_null();
/** find_datablock_consumer with wrong shared_secret → nullptr (no attach). */
int find_consumer_wrong_secret_returns_null();
/** release_write_slot with default-constructed (invalid) handle → false. */
int release_write_slot_invalid_handle_returns_false();
/** release_consume_slot with default-constructed (invalid) handle → false. */
int release_consume_slot_invalid_handle_returns_false();
/** write(offset+len > buffer_size) and write(len==0) return false. */
int write_bounds_return_false();
/** commit(bytes_written > buffer_size) returns false. */
int commit_bounds_return_false();
/** read(offset+len > buffer_size) returns false. */
int read_bounds_return_false();
/** Second release_write_slot on same handle is idempotent (returns true). */
int double_release_write_slot_idempotent();
/** try_next(timeout) returns ok=false when no slot available (timeout). */
int slot_iterator_try_next_timeout_returns_not_ok();

} // namespace pylabhub::tests::worker::error_handling
