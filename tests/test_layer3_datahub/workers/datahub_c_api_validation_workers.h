// tests/test_layer3_datahub/workers/datahub_c_api_validation_workers.h
#pragma once

namespace pylabhub::tests::worker::c_api_validation
{

int validate_integrity_on_fresh_datablock();
int validate_integrity_nonexistent_fails();
int get_metrics_fresh_has_zero_commits();
int diagnose_slot_fresh_is_free();
int diagnose_all_slots_returns_capacity();

} // namespace pylabhub::tests::worker::c_api_validation
