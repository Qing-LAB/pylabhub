// tests/test_layer3_datahub/workers/datahub_c_api_draining_workers.h
#pragma once

namespace pylabhub::tests::worker::c_api_draining
{

int draining_state_entered_on_wraparound();
int draining_rejects_new_readers();
int draining_resolves_after_reader_release();
int draining_timeout_restores_committed();
int no_reader_races_on_clean_wraparound();
int single_reader_ring_full_blocks_not_draining();
int sync_reader_ring_full_blocks_not_draining();

} // namespace pylabhub::tests::worker::c_api_draining
