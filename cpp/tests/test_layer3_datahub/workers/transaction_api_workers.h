// tests/test_layer3_datahub/workers/transaction_api_workers.h
#pragma once

namespace pylabhub::tests::worker::transaction_api
{

/** with_write_transaction success path: write and commit via transaction API. */
int with_write_transaction_success();

/** with_write_transaction timeout: writer blocks (no slot available), times out. */
int with_write_transaction_timeout();

/** WriteTransactionGuard: lambda throws before commit; destructor releases slot; slot becomes available. */
int WriteTransactionGuard_exception_releases_slot();

/** ReadTransactionGuard: lambda throws; destructor releases slot. */
int ReadTransactionGuard_exception_releases_slot();

/** with_typed_write / with_typed_read: type-safe slot access succeeds. */
int with_typed_write_read_succeeds();

/** with_next_slot: iterator-based consume. */
int with_next_slot_iterator();

} // namespace pylabhub::tests::worker::transaction_api
