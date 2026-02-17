// tests/test_layer3_datahub/workers/c_api_slot_protocol_workers.h
#pragma once

namespace pylabhub::tests::worker::c_api_slot_protocol
{

int write_slot_read_slot_roundtrip();
int commit_advances_metrics();
int abort_does_not_commit();
int latest_only_reads_latest();
int single_reader_reads_sequentially();
int write_returns_null_when_ring_full();
int read_returns_null_on_empty_ring();
int metrics_accumulate_across_writes();

} // namespace pylabhub::tests::worker::c_api_slot_protocol
